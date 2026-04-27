/**
 * @file LogicServiceImpl.h
 * @brief Phase 1.2 W1.D3-D5: LogicService 业务层实现
 *
 * D3 骨架 → D4-D5 接真 MessageService：
 * - HandleMessage 解 payload JSON：type=msg → 持久化到 private_messages 表，回 ack
 *   (server_msg_id = 雪花 ID)；其他 type 暂只回 OK，不落库。
 * - HandleAuth / RegisterGateway 仍是 stub，等 W1.D5+ / W2.D4+。
 */
#pragma once

#include "GatewayRegistry.h"
#include "im/logic.grpc.pb.h"
#include "server/MessageService.h"
#include "util/Snowflake.h"
#include <grpcpp/grpcpp.h>
#include <nlohmann/json.hpp>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

class LogicServiceImpl final : public im::LogicService::Service {
public:
    LogicServiceImpl(std::shared_ptr<MessageService> msgSvc,
                     std::shared_ptr<GatewayRegistry> registry)
        : msgSvc_(std::move(msgSvc)), registry_(std::move(registry)) {}

    grpc::Status HandleMessage(grpc::ServerContext* /*ctx*/,
                               const im::MessageRequest* req,
                               im::MessageResponse* resp) override {
        std::cerr << "[logic] HandleMessage trace=" << req->env().trace_id()
                  << " uid=" << req->who().uid()
                  << " device=" << req->who().device_id()
                  << " bytes=" << req->payload().size() << "\n";

        nlohmann::json j;
        try {
            j = nlohmann::json::parse(req->payload());
        } catch (const std::exception& e) {
            resp->mutable_status()->set_code(im::StatusCode::INVALID);
            resp->mutable_status()->set_msg(std::string("payload parse: ") + e.what());
            return grpc::Status::OK;
        }

        std::string type = j.value("type", "");
        if (type == "msg") {
            // 私聊：to + content 必须有
            if (!j.contains("to") || !j.contains("content")) {
                resp->mutable_status()->set_code(im::StatusCode::INVALID);
                resp->mutable_status()->set_msg("missing to/content");
                return grpc::Status::OK;
            }
            int64_t from = req->who().uid();
            int64_t to   = std::stoll(j["to"].is_string() ? j["to"].get<std::string>()
                                                          : std::to_string(j["to"].get<int64_t>()));
            std::string content = j["content"].get<std::string>();
            int64_t serverMsgId = mymuduo::Snowflake::instance().nextId();
            std::string msgId = std::to_string(serverMsgId);
            int64_t ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();

            bool ok = msgSvc_ && msgSvc_->savePrivateMessage(msgId, from, to, content, ts);
            if (!ok) {
                resp->mutable_status()->set_code(im::StatusCode::INTERNAL);
                resp->mutable_status()->set_msg("savePrivateMessage failed");
                return grpc::Status::OK;
            }

            resp->mutable_status()->set_code(im::StatusCode::OK);
            resp->mutable_status()->set_msg("saved");
            resp->set_server_msg_id(serverMsgId);
            // 回给 gateway 立即返还客户端的 ack
            nlohmann::json ack = {
                {"type", "ack"},
                {"clientMsgId", j.value("clientMsgId", "")},
                {"serverMsgId", msgId},
                {"timestamp", ts},
            };
            resp->set_inline_reply(ack.dump());

            // 通过 RegisterGateway 反向流推给 to 用户所在 gateway
            if (registry_) {
                nlohmann::json fwd = {
                    {"type", "msg"},
                    {"from", std::to_string(from)},
                    {"to", std::to_string(to)},
                    {"content", content},
                    {"msgId", msgId},
                    {"timestamp", ts},
                };
                bool pushed = registry_->pushToUser(to, fwd.dump());
                std::cerr << "[logic] pushToUser uid=" << to
                          << " result=" << (pushed ? "ok" : "offline") << "\n";
            }
            return grpc::Status::OK;
        }

        // 其他 type（typing/recall/edit/...）后续 day 接，先回 OK 不动作
        resp->mutable_status()->set_code(im::StatusCode::OK);
        resp->mutable_status()->set_msg("type=" + type + " not handled in W1.D5");
        return grpc::Status::OK;
    }

    grpc::Status HandleAuth(grpc::ServerContext* /*ctx*/,
                            const im::AuthRequest* /*req*/,
                            im::AuthResponse* resp) override {
        resp->mutable_status()->set_code(im::StatusCode::INTERNAL);
        resp->mutable_status()->set_msg("HandleAuth not implemented yet");
        return grpc::Status::OK;
    }

    /// gateway 静默多久判定为死亡，单位毫秒。
    /// gateway 端 LogicClient 默认每 15s 在 idle 时发一帧 Heartbeat，
    /// 这里给 3 倍余量：连续两次 heartbeat 都丢才判死。
    static constexpr int64_t kGatewaySilenceMs = 45'000;

    grpc::Status RegisterGateway(
        grpc::ServerContext* ctx,
        grpc::ServerReaderWriter<im::PushCommand, im::GatewayEvent>* stream) override {
        std::string gatewayId;
        std::atomic<int64_t> lastSeenMs{nowMs()};
        std::atomic<bool> running{true};

        // Watchdog：周期检查最后一次收到 GatewayEvent 的时间，超阈值就 TryCancel。
        // gRPC 的 TryCancel 会让阻塞中的 stream->Read 返回 false，主循环顺势退出，
        // 接着走正常的 deregisterGateway 路径。
        std::thread watchdog([&]() {
            while (running.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                if (!running.load()) break;
                int64_t silent = nowMs() - lastSeenMs.load();
                if (silent > kGatewaySilenceMs) {
                    std::cerr << "[logic] gateway "
                              << (gatewayId.empty() ? "<no-hello>" : gatewayId)
                              << " silent for " << silent << "ms (>"
                              << kGatewaySilenceMs << "ms), cancel stream\n";
                    ctx->TryCancel();
                    return;
                }
            }
        });

        im::GatewayEvent ev;
        while (stream->Read(&ev)) {
            lastSeenMs.store(nowMs());
            if (ev.has_hello()) {
                gatewayId = ev.hello().instance_id();
                if (registry_) registry_->registerGateway(gatewayId, stream);
                std::cerr << "[logic] gateway hello id=" << gatewayId
                          << " ver=" << ev.hello().version() << "\n";
            } else if (ev.has_conn_open()) {
                int64_t uid = ev.conn_open().who().uid();
                if (registry_) registry_->onUserOpen(uid, gatewayId);
                std::cerr << "[logic] conn_open uid=" << uid << " gw=" << gatewayId << "\n";
            } else if (ev.has_conn_close()) {
                int64_t uid = ev.conn_close().who().uid();
                if (registry_) registry_->onUserClose(uid, gatewayId);
                std::cerr << "[logic] conn_close uid=" << uid << " gw=" << gatewayId << "\n";
            } else if (ev.has_heartbeat()) {
                // 心跳本身就是用来刷新 lastSeenMs 的（前面已经刷过）；不需要其他处理
            }
            // up_msg 留给 W3+：让 gateway 也能把上行走 stream 而不是 unary
        }

        running.store(false);
        if (watchdog.joinable()) watchdog.join();

        if (!gatewayId.empty() && registry_) {
            registry_->deregisterGateway(gatewayId);
            std::cerr << "[logic] gateway gone id=" << gatewayId << "\n";
        }
        return grpc::Status::OK;
    }

private:
    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::shared_ptr<MessageService> msgSvc_;
    std::shared_ptr<GatewayRegistry> registry_;
};
