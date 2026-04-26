/**
 * @file LogicClientPool.h
 * @brief 多 logic 实例池 + 一致性 hash 路由（Phase 1.2 W3.D1-D2）
 *
 * 行为：
 *   - 通过 RegistryService 的 List + Watch 维护 logic endpoint 列表
 *   - 每个 endpoint 一个 LogicClient（带 RegisterGateway 双向流）
 *   - 上行 HandleMessage：按 routing_key（默认 sender uid）走 ring 选 logic
 *   - 上下线 ConnOpen/ConnClose：broadcast 给所有 logic（让任何 logic 都能反向推送）
 *
 * 简化：未做退避/熔断；endpoint 突发抖动时上行 RPC 可能失败，外层自行兜底（如错误回写 ws）。
 */
#pragma once

#include "ConsistentHashRing.h"
#include "LogicClient.h"
#include "im/registry.grpc.pb.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

class LogicClientPool {
public:
    LogicClientPool(std::string registryAddr, std::string gatewayId,
                    LogicClient::PushCallback pushCb)
        : registryAddr_(std::move(registryAddr)),
          gatewayId_(std::move(gatewayId)),
          pushCb_(std::move(pushCb)) {}

    ~LogicClientPool() { stop(); }

    void start() {
        if (running_.exchange(true)) return;
        bootstrap();
        watchThread_ = std::thread([this]() { watchLoop(); });
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (watchCtx_) watchCtx_->TryCancel();
        if (watchThread_.joinable()) watchThread_.join();
        std::lock_guard<std::mutex> lk(mu_);
        clients_.clear();
    }

    /// 上行 unary：按 routing_key 选 logic
    grpc::Status handleMessage(int64_t uid, const std::string& deviceId,
                               const std::string& connId,
                               const std::string& routingKey,
                               const std::string& payload, std::string* reply) {
        std::shared_ptr<LogicClient> client;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (ring_.empty()) {
                return grpc::Status(grpc::StatusCode::UNAVAILABLE, "no logic instance");
            }
            auto addr = ring_.get(routingKey);
            auto it = clients_.find(addr);
            if (it == clients_.end()) {
                return grpc::Status(grpc::StatusCode::UNAVAILABLE, "no client for " + addr);
            }
            client = it->second;
        }
        return client->handleMessage(uid, deviceId, connId, payload, reply);
    }

    void notifyConnOpen(int64_t uid, const std::string& deviceId) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [_, c] : clients_) c->notifyConnOpen(uid, deviceId);
    }

    void notifyConnClose(int64_t uid, const std::string& deviceId) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [_, c] : clients_) c->notifyConnClose(uid, deviceId);
    }

    size_t size() const {
        std::lock_guard<std::mutex> lk(mu_);
        return clients_.size();
    }

private:
    void bootstrap() {
        // 一次 List 拿到现存 logic（同时确保 Watch 之前已建好客户端）
        auto regChannel = grpc::CreateChannel(registryAddr_, grpc::InsecureChannelCredentials());
        auto regStub = im::RegistryService::NewStub(regChannel);
        im::ListRequest listReq;
        listReq.set_service("logic");
        im::ListResponse listResp;
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(3));
        auto st = regStub->List(&ctx, listReq, &listResp);
        if (!st.ok()) {
            std::cerr << "[pool] registry List failed: " << st.error_message() << "\n";
            return;
        }
        for (auto& ep : listResp.endpoints()) addEndpoint(ep);
    }

    void watchLoop() {
        auto regChannel = grpc::CreateChannel(registryAddr_, grpc::InsecureChannelCredentials());
        auto regStub = im::RegistryService::NewStub(regChannel);
        while (running_.load()) {
            watchCtx_ = std::make_unique<grpc::ClientContext>();
            im::WatchRequest req;
            req.set_service("logic");
            auto reader = regStub->Watch(watchCtx_.get(), req);
            im::WatchEvent ev;
            while (reader && reader->Read(&ev)) {
                switch (ev.type()) {
                    case im::WatchEvent::ADDED:   addEndpoint(ev.ep()); break;
                    case im::WatchEvent::REMOVED: removeEndpoint(ev.ep().instance_id()); break;
                    case im::WatchEvent::UPDATED: /* 简化：先按 ADDED 处理 */
                        addEndpoint(ev.ep()); break;
                    default: break;
                }
            }
            if (reader) reader->Finish();
            watchCtx_.reset();
            if (running_.load()) {
                std::cerr << "[pool] registry Watch ended, reconnect in 2s\n";
                std::this_thread::sleep_for(std::chrono::seconds(2));
            }
        }
    }

    void addEndpoint(const im::Endpoint& ep) {
        std::lock_guard<std::mutex> lk(mu_);
        if (clients_.count(ep.addr())) return;
        auto c = std::make_shared<LogicClient>(ep.addr(), gatewayId_);
        c->setPushCallback(pushCb_);
        c->start();
        clients_[ep.addr()] = c;
        instanceToAddr_[ep.instance_id()] = ep.addr();
        ring_.addNode(ep.addr());
        std::cerr << "[pool] add logic instance=" << ep.instance_id()
                  << " addr=" << ep.addr()
                  << " (size=" << clients_.size() << ")\n";
    }

    void removeEndpoint(const std::string& instanceId) {
        std::lock_guard<std::mutex> lk(mu_);
        auto ait = instanceToAddr_.find(instanceId);
        if (ait == instanceToAddr_.end()) return;
        std::string addr = ait->second;
        instanceToAddr_.erase(ait);
        auto cit = clients_.find(addr);
        if (cit != clients_.end()) {
            cit->second->stop();
            clients_.erase(cit);
            ring_.removeNode(addr);
            std::cerr << "[pool] remove logic instance=" << instanceId
                      << " addr=" << addr << "\n";
        }
    }

    std::string registryAddr_;
    std::string gatewayId_;
    LogicClient::PushCallback pushCb_;

    std::atomic<bool> running_{false};
    std::thread watchThread_;
    std::unique_ptr<grpc::ClientContext> watchCtx_;

    mutable std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<LogicClient>> clients_;  // key = addr
    std::unordered_map<std::string, std::string> instanceToAddr_;            // instance_id → addr
    ConsistentHashRing ring_{200};
};
