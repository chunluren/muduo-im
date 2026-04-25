/**
 * @file LogicClient.h
 * @brief gateway → logic 的 gRPC 客户端封装（Phase 1.2 W2.D1-D5）
 *
 * 两条路径：
 *   1. 上行（ws → logic）：unary HandleMessage，3s 超时
 *   2. 下行（logic → ws）：RegisterGateway 双向流。本类提供线程驱动的注册/读循环 +
 *      自动重连（指数退避）；外部通过 setPushCallback 注入"收到 PushCommand 时
 *      把 payload 投递到本地某 uid 的 ws session"的回调。
 *
 * 同时给外部提供 notifyConnOpen / notifyConnClose 把 ws 上下线事件通过 stream
 * 写给 logic（让 logic 维护 uid→gateway_id 映射）。
 */
#pragma once

#include "im/logic.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class LogicClient {
public:
    /// 收到 logic 反向推送时回调；onPushCallback(target_uid, payload)
    using PushCallback = std::function<void(int64_t, const std::string&)>;

    LogicClient(const std::string& addr, std::string gatewayId)
        : addr_(addr), gatewayId_(std::move(gatewayId)) {
        channel_ = grpc::CreateChannel(addr_, grpc::InsecureChannelCredentials());
        stub_ = im::LogicService::NewStub(channel_);
    }

    ~LogicClient() { stop(); }

    void setPushCallback(PushCallback cb) { pushCb_ = std::move(cb); }

    // ---- 上行 unary ----

    grpc::Status handleMessage(int64_t uid, const std::string& deviceId,
                               const std::string& connId, const std::string& payload,
                               std::string* reply) {
        im::MessageRequest req;
        req.mutable_env()->set_trace_id(connId + "-" + std::to_string(nowMs()));
        req.mutable_env()->set_instance(gatewayId_);
        req.mutable_env()->set_routing_key(std::to_string(uid));
        req.mutable_who()->set_uid(uid);
        req.mutable_who()->set_device_id(deviceId);
        req.mutable_who()->set_conn_id(connId);
        req.set_payload(payload);

        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
        im::MessageResponse resp;
        auto st = stub_->HandleMessage(&ctx, req, &resp);
        if (st.ok() && reply) *reply = resp.inline_reply();
        return st;
    }

    // ---- 下行 stream ----

    /// 启动 RegisterGateway 双向流线程（自动重连）
    void start() {
        if (running_.exchange(true)) return;
        thread_ = std::thread([this]() { streamLoop(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        // wakeup 写线程
        {
            std::lock_guard<std::mutex> lk(qmu_);
            qcv_.notify_all();
        }
        if (ctx_) ctx_->TryCancel();
        if (thread_.joinable()) thread_.join();
    }

    void notifyConnOpen(int64_t uid, const std::string& deviceId) {
        im::GatewayEvent ev;
        auto* who = ev.mutable_conn_open()->mutable_who();
        who->set_uid(uid);
        who->set_device_id(deviceId);
        ev.mutable_env()->set_instance(gatewayId_);
        enqueue(std::move(ev));
    }

    void notifyConnClose(int64_t uid, const std::string& deviceId) {
        im::GatewayEvent ev;
        auto* who = ev.mutable_conn_close()->mutable_who();
        who->set_uid(uid);
        who->set_device_id(deviceId);
        ev.mutable_env()->set_instance(gatewayId_);
        enqueue(std::move(ev));
    }

private:
    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    void enqueue(im::GatewayEvent ev) {
        std::lock_guard<std::mutex> lk(qmu_);
        eventQueue_.emplace_back(std::move(ev));
        qcv_.notify_one();
    }

    void streamLoop() {
        int backoffSec = 1;
        while (running_.load()) {
            ctx_ = std::make_unique<grpc::ClientContext>();
            auto stream = stub_->RegisterGateway(ctx_.get());
            if (!stream) {
                std::cerr << "[gateway] RegisterGateway create failed\n";
                std::this_thread::sleep_for(std::chrono::seconds(backoffSec));
                backoffSec = std::min(backoffSec * 2, 30);
                continue;
            }

            // 第一帧 Hello
            im::GatewayEvent hello;
            hello.mutable_hello()->set_instance_id(gatewayId_);
            hello.mutable_hello()->set_version("0.1");
            hello.mutable_env()->set_instance(gatewayId_);
            if (!stream->Write(hello)) {
                std::cerr << "[gateway] hello write failed, retry\n";
                stream->Finish();
                std::this_thread::sleep_for(std::chrono::seconds(backoffSec));
                backoffSec = std::min(backoffSec * 2, 30);
                continue;
            }
            std::cerr << "[gateway] RegisterGateway up, id=" << gatewayId_ << "\n";
            backoffSec = 1;

            // 写线程：从 eventQueue_ 取事件 Write
            std::atomic<bool> writerAlive{true};
            std::thread writer([this, &stream, &writerAlive]() {
                while (running_.load() && writerAlive.load()) {
                    im::GatewayEvent ev;
                    {
                        std::unique_lock<std::mutex> lk(qmu_);
                        qcv_.wait_for(lk, std::chrono::seconds(15), [this, &writerAlive]() {
                            return !running_.load() || !writerAlive.load() || !eventQueue_.empty();
                        });
                        if (!running_.load() || !writerAlive.load()) return;
                        if (eventQueue_.empty()) {
                            // 心跳
                            ev.mutable_heartbeat()->set_ts_ms(nowMs());
                        } else {
                            ev = std::move(eventQueue_.front());
                            eventQueue_.pop_front();
                        }
                    }
                    if (!stream->Write(ev)) {
                        writerAlive = false;
                        return;
                    }
                }
            });

            // 主线程：读 PushCommand
            im::PushCommand cmd;
            while (stream->Read(&cmd)) {
                if (cmd.has_push_user() && pushCb_) {
                    pushCb_(cmd.push_user().uid(), cmd.push_user().payload());
                }
                // push_conn / kick / pong 暂不处理
            }
            std::cerr << "[gateway] stream Read loop ended\n";

            writerAlive = false;
            { std::lock_guard<std::mutex> lk(qmu_); qcv_.notify_all(); }
            if (writer.joinable()) writer.join();

            auto status = stream->Finish();
            std::cerr << "[gateway] stream finished: " << status.error_code()
                      << " " << status.error_message() << "\n";
            ctx_.reset();
            if (running_.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(backoffSec));
                backoffSec = std::min(backoffSec * 2, 30);
            }
        }
    }

    std::string addr_;
    std::string gatewayId_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<im::LogicService::Stub> stub_;

    std::atomic<bool> running_{false};
    std::thread thread_;
    std::unique_ptr<grpc::ClientContext> ctx_;

    std::mutex qmu_;
    std::condition_variable qcv_;
    std::deque<im::GatewayEvent> eventQueue_;

    PushCallback pushCb_;
};
