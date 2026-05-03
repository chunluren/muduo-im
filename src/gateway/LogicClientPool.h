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
#include "util/EtcdClient.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
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

    /// Phase 5: 设置本 gateway 所在 AZ。空字符串 = AZ-blind（默认）。
    /// 设置后路由优先选 same-AZ 的 logic；same-AZ 全挂时 fallback 到任意 logic。
    void setLocalAz(std::string az) { localAz_ = std::move(az); }

    /// 改走 etcd 服务发现（替代默认的 gRPC RegistryService Watch）
    /// 必须在 start() 之前调用
    void enableEtcd(std::string host, int port,
                    std::string apiPrefix = "/v3beta",
                    std::string prefix = "services/logic/",
                    int pollIntervalMs = 1000) {
        etcdEnabled_      = true;
        etcdHost_         = std::move(host);
        etcdPort_         = port;
        etcdApiPrefix_    = std::move(apiPrefix);
        etcdPrefix_       = std::move(prefix);
        etcdPollMs_       = pollIntervalMs;
    }

    void start() {
        if (running_.exchange(true)) return;
        if (etcdEnabled_) {
            watchThread_ = std::thread([this]() { etcdPollLoop(); });
        } else {
            bootstrap();
            watchThread_ = std::thread([this]() { watchLoop(); });
        }
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (watchCtx_) watchCtx_->TryCancel();
        if (watchThread_.joinable()) watchThread_.join();
        std::lock_guard<std::mutex> lk(mu_);
        clients_.clear();
    }

    /// 上行 unary：按 routing_key 选 logic；UNAVAILABLE/DEADLINE 时尝试一次备用
    /// 备用挑选：从 clients_ 里找一个 healthy() 且 addr 不同的，轮询遍历最多一次
    grpc::Status handleMessage(int64_t uid, const std::string& deviceId,
                               const std::string& connId,
                               const std::string& routingKey,
                               const std::string& payload, std::string* reply) {
        std::shared_ptr<LogicClient> primary;
        std::string primaryAddr;
        {
            std::lock_guard<std::mutex> lk(mu_);
            // Phase 5: 优先 same-AZ 环；空 / 没节点时退到全局环
            if (!localAz_.empty()) {
                auto it = ringByAz_.find(localAz_);
                if (it != ringByAz_.end() && !it->second.empty()) {
                    primaryAddr = it->second.get(routingKey);
                }
            }
            if (primaryAddr.empty()) {
                if (ring_.empty()) {
                    return grpc::Status(grpc::StatusCode::UNAVAILABLE, "no logic instance");
                }
                primaryAddr = ring_.get(routingKey);
            }
            auto cit = clients_.find(primaryAddr);
            if (cit == clients_.end()) {
                return grpc::Status(grpc::StatusCode::UNAVAILABLE,
                                    "no client for " + primaryAddr);
            }
            primary = cit->second;
        }

        auto st = primary->handleMessage(uid, deviceId, connId, payload, reply);
        if (st.ok()) return st;
        if (st.error_code() != grpc::StatusCode::UNAVAILABLE &&
            st.error_code() != grpc::StatusCode::DEADLINE_EXCEEDED) {
            return st;  // 业务错或其它，不重试
        }

        // 重试：找一个 addr 不同且 healthy 的备用，same-AZ 优先
        std::shared_ptr<LogicClient> backup;
        {
            std::lock_guard<std::mutex> lk(mu_);
            // 1) same-AZ 备用
            if (!localAz_.empty()) {
                for (auto& [addr, c] : clients_) {
                    if (addr == primaryAddr || !c->healthy()) continue;
                    auto azIt = addrToAz_.find(addr);
                    if (azIt != addrToAz_.end() && azIt->second == localAz_) {
                        backup = c;
                        break;
                    }
                }
            }
            // 2) 跨 AZ fail-open
            if (!backup) {
                for (auto& [addr, c] : clients_) {
                    if (addr == primaryAddr || !c->healthy()) continue;
                    backup = c;
                    break;
                }
            }
        }
        if (!backup) return st;  // 没备用 → 把 primary 的状态原样返
        std::cerr << "[pool] retry " << primaryAddr << " → " << backup->addr()
                  << " (primary " << st.error_message() << ")\n";
        return backup->handleMessage(uid, deviceId, connId, payload, reply);
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

    /// 轮询 etcd prefix，diff 出 added/removed/updated → 复用 addEndpoint/removeEndpoint
    void etcdPollLoop() {
        EtcdClient etcd(etcdHost_, etcdPort_, etcdApiPrefix_);
        // pollOnce 第一次调用时 lastSnapshot_ 为空 → 不会触发 cb；这里手动 bootstrap 一次
        {
            auto kvs = etcd.getPrefix(etcdPrefix_);
            for (auto& kv : kvs) {
                im::Endpoint ep;
                std::string az;
                if (parseEndpoint(kv.key, kv.value, ep, az)) addEndpoint(ep, az);
            }
            // 把首批写入 EtcdClient::lastSnapshot_，避免下一轮再触发 added
            etcd.pollOnce(etcdPrefix_, [](auto&, auto&, auto&) {});
        }

        auto cb = [this](const std::vector<EtcdClient::KV>& added,
                         const std::vector<std::string>& removedKeys,
                         const std::vector<EtcdClient::KV>& updated) {
            for (auto& kv : added) {
                im::Endpoint ep;
                std::string az;
                if (parseEndpoint(kv.key, kv.value, ep, az)) addEndpoint(ep, az);
            }
            for (auto& key : removedKeys) {
                std::string instId = instanceIdFromKey(key);
                if (!instId.empty()) removeEndpoint(instId);
            }
            for (auto& kv : updated) {
                // 简化：先 remove 再 add（与 watchLoop 行为一致）
                im::Endpoint ep;
                std::string az;
                if (parseEndpoint(kv.key, kv.value, ep, az)) {
                    removeEndpoint(ep.instance_id());
                    addEndpoint(ep, az);
                }
            }
        };

        while (running_.load()) {
            try {
                etcd.pollOnce(etcdPrefix_, cb);
            } catch (const std::exception& e) {
                std::cerr << "[pool] etcd pollOnce failed: " << e.what() << "\n";
            }
            // 可中断 sleep（每 100ms 检查 running_）
            int slept = 0;
            while (running_.load() && slept < etcdPollMs_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                slept += 100;
            }
        }
    }

    /// "services/logic/logic-pid-123" → "logic-pid-123"
    std::string instanceIdFromKey(const std::string& key) const {
        if (key.size() <= etcdPrefix_.size()) return {};
        if (key.compare(0, etcdPrefix_.size(), etcdPrefix_) != 0) return {};
        return key.substr(etcdPrefix_.size());
    }

    /// 把 etcd 里的 JSON value 解析成 im::Endpoint + AZ tag。失败返回 false
    bool parseEndpoint(const std::string& key, const std::string& value,
                       im::Endpoint& out, std::string& az) const {
        auto j = nlohmann::json::parse(value, nullptr, false);
        if (j.is_discarded() || !j.contains("addr")) {
            std::cerr << "[pool] etcd value malformed key=" << key << "\n";
            return false;
        }
        out.set_instance_id(j.value("instance", instanceIdFromKey(key)));
        out.set_service(j.value("service", "logic"));
        out.set_addr(j["addr"].get<std::string>());
        out.set_weight(j.value("weight", 1));
        az = j.value("az", "");
        return true;
    }

    /// 添加 endpoint。`az` 为空表示 AZ-blind（registry gRPC 路径 default）；
    /// etcd 路径会从 JSON value 里取 "az" 字段并传进来（Phase 5）。
    void addEndpoint(const im::Endpoint& ep, const std::string& az = "") {
        std::lock_guard<std::mutex> lk(mu_);
        if (clients_.count(ep.addr())) return;
        auto c = std::make_shared<LogicClient>(ep.addr(), gatewayId_);
        c->setPushCallback(pushCb_);
        c->start();
        clients_[ep.addr()] = c;
        instanceToAddr_[ep.instance_id()] = ep.addr();
        ring_.addNode(ep.addr());
        if (!az.empty()) {
            ringByAz_[az].addNode(ep.addr());
            addrToAz_[ep.addr()] = az;
        }
        std::cerr << "[pool] add logic instance=" << ep.instance_id()
                  << " addr=" << ep.addr()
                  << (az.empty() ? std::string{}
                                 : (" az=" + az + (az == localAz_ ? " (local)" : " (remote)")))
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
            auto azIt = addrToAz_.find(addr);
            if (azIt != addrToAz_.end()) {
                auto rIt = ringByAz_.find(azIt->second);
                if (rIt != ringByAz_.end()) rIt->second.removeNode(addr);
                addrToAz_.erase(azIt);
            }
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

    // etcd 服务发现（可选，互斥于 RegistryService）
    bool        etcdEnabled_   = false;
    std::string etcdHost_;
    int         etcdPort_      = 2379;
    std::string etcdApiPrefix_ = "/v3beta";
    std::string etcdPrefix_    = "services/logic/";
    int         etcdPollMs_    = 1000;

    mutable std::mutex mu_;
    std::unordered_map<std::string, std::shared_ptr<LogicClient>> clients_;  // key = addr
    std::unordered_map<std::string, std::string> instanceToAddr_;            // instance_id → addr
    ConsistentHashRing ring_{200};

    // Phase 5: AZ-aware routing
    std::string localAz_;                                                    // 本 gateway 所在 AZ
    std::unordered_map<std::string, std::string> addrToAz_;                  // addr → az
    std::unordered_map<std::string, ConsistentHashRing> ringByAz_;           // az → ring
};
