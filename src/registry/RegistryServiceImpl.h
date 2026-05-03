/**
 * @file RegistryServiceImpl.h
 * @brief Phase 1.2 W3.D1-D2：进程内 RegistryService（in-memory 单点）
 *
 * 职责：
 *  - Register：插入 endpoint，分配单调递增 lease_id，置 lastSeen=now
 *  - Deregister：删 endpoint，广播 REMOVED
 *  - KeepAlive：lease_id 找 instance_id，刷新 lastSeen，返回剩余 TTL
 *  - List / Watch：按 service 过滤
 *
 * TTL GC（2026-05-03 加）：后台线程每 kGcIntervalSec 扫一次 endpoints，超过
 * kTtlSec 没收到 KeepAlive 的视为死亡，删除并广播 REMOVED。这条修了之前
 * "logic OOM kill 后僵尸记录留在 registry 里"的缺口。
 */
#pragma once

#include "im/registry.grpc.pb.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <list>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

class RegistryServiceImpl final : public im::RegistryService::Service {
public:
    /// instance 多久没 KeepAlive 视为死亡（秒）
    static constexpr int kTtlSec        = 30;
    /// GC 扫描周期（秒）。kGcIntervalSec < kTtlSec/3 才有意义
    static constexpr int kGcIntervalSec = 5;

    RegistryServiceImpl() = default;

    /// 启动后台 GC 线程；server.BuildAndStart 之后调一次
    void startGc() {
        if (gcRunning_.exchange(true)) return;
        gcThread_ = std::thread([this]() { gcLoop(); });
    }

    /// 停止 GC（dtor 自动调）
    void stopGc() {
        if (!gcRunning_.exchange(false)) return;
        {
            std::lock_guard<std::mutex> lk(gcMu_);
            gcCv_.notify_all();
        }
        if (gcThread_.joinable()) gcThread_.join();
    }

    ~RegistryServiceImpl() { stopGc(); }

    grpc::Status Register(grpc::ServerContext* /*ctx*/,
                          const im::RegisterRequest* req,
                          im::RegisterResponse* resp) override {
        std::lock_guard<std::mutex> lk(mu_);
        const auto& ep = req->ep();

        // 同 instance_id 重新注册：清掉旧 lease 防止 lease 表泄漏
        auto oldIt = instanceToLease_.find(ep.instance_id());
        if (oldIt != instanceToLease_.end()) {
            leaseToInstance_.erase(oldIt->second);
        }

        endpoints_[ep.instance_id()] = ep;
        lastSeen_[ep.instance_id()] = nowSteady();

        int64_t leaseId = ++leaseSeq_;
        leaseToInstance_[leaseId] = ep.instance_id();
        instanceToLease_[ep.instance_id()] = leaseId;

        broadcastEvent(im::WatchEvent::ADDED, ep);
        resp->mutable_status()->set_code(im::StatusCode::OK);
        resp->set_lease_id(leaseId);
        std::cerr << "[registry] register " << ep.service() << "/"
                  << ep.instance_id() << " addr=" << ep.addr()
                  << " lease=" << leaseId << "\n";
        return grpc::Status::OK;
    }

    grpc::Status Deregister(grpc::ServerContext* /*ctx*/,
                            const im::DeregisterRequest* req,
                            im::DeregisterResponse* resp) override {
        std::lock_guard<std::mutex> lk(mu_);
        removeInstanceLocked(req->instance_id(), "deregister");
        resp->mutable_status()->set_code(im::StatusCode::OK);
        return grpc::Status::OK;
    }

    grpc::Status KeepAlive(grpc::ServerContext* /*ctx*/,
                           const im::KeepAliveRequest* req,
                           im::KeepAliveResponse* resp) override {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = leaseToInstance_.find(req->lease_id());
        if (it == leaseToInstance_.end()) {
            // lease 不存在：要么 client 用了过期 lease，要么 GC 已经踢了
            resp->mutable_status()->set_code(im::StatusCode::NOT_FOUND);
            resp->mutable_status()->set_msg("lease not found");
            resp->set_ttl_remaining(0);
            return grpc::Status::OK;
        }
        lastSeen_[it->second] = nowSteady();
        resp->mutable_status()->set_code(im::StatusCode::OK);
        resp->set_ttl_remaining(kTtlSec);
        return grpc::Status::OK;
    }

    grpc::Status List(grpc::ServerContext* /*ctx*/,
                      const im::ListRequest* req,
                      im::ListResponse* resp) override {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& [_, ep] : endpoints_) {
            if (req->service().empty() || ep.service() == req->service()) {
                *resp->add_endpoints() = ep;
            }
        }
        return grpc::Status::OK;
    }

    grpc::Status Watch(grpc::ServerContext* ctx,
                       const im::WatchRequest* req,
                       grpc::ServerWriter<im::WatchEvent>* writer) override {
        Watcher w;
        w.service = req->service();
        {
            std::lock_guard<std::mutex> lk(mu_);
            watchers_.push_back(&w);
            // 全量下发当前条目
            for (auto& [_, ep] : endpoints_) {
                if (w.service.empty() || ep.service() == w.service) {
                    im::WatchEvent ev;
                    ev.set_type(im::WatchEvent::ADDED);
                    *ev.mutable_ep() = ep;
                    w.queue.push_back(std::move(ev));
                }
            }
        }

        while (!ctx->IsCancelled()) {
            im::WatchEvent ev;
            {
                std::unique_lock<std::mutex> lk(mu_);
                w.cv.wait_for(lk, std::chrono::seconds(5), [&]() {
                    return !w.queue.empty() || ctx->IsCancelled();
                });
                if (ctx->IsCancelled()) break;
                if (w.queue.empty()) continue;
                ev = std::move(w.queue.front());
                w.queue.pop_front();
            }
            if (!writer->Write(ev)) break;
        }

        std::lock_guard<std::mutex> lk(mu_);
        watchers_.remove(&w);
        return grpc::Status::OK;
    }

private:
    struct Watcher {
        std::string service;
        std::deque<im::WatchEvent> queue;
        std::condition_variable cv;
    };

    using SteadyTp = std::chrono::steady_clock::time_point;
    static SteadyTp nowSteady() { return std::chrono::steady_clock::now(); }

    /// 调用方持有 mu_
    void broadcastEvent(im::WatchEvent::Type type, const im::Endpoint& ep) {
        for (auto* w : watchers_) {
            if (!w) continue;
            if (!w->service.empty() && w->service != ep.service()) continue;
            im::WatchEvent ev;
            ev.set_type(type);
            *ev.mutable_ep() = ep;
            w->queue.push_back(std::move(ev));
            w->cv.notify_all();
        }
    }

    /// 调用方持有 mu_
    void removeInstanceLocked(const std::string& instanceId, const char* reason) {
        auto it = endpoints_.find(instanceId);
        if (it == endpoints_.end()) return;
        broadcastEvent(im::WatchEvent::REMOVED, it->second);
        std::cerr << "[registry] " << reason << " " << instanceId << "\n";
        endpoints_.erase(it);
        lastSeen_.erase(instanceId);
        auto leaseIt = instanceToLease_.find(instanceId);
        if (leaseIt != instanceToLease_.end()) {
            leaseToInstance_.erase(leaseIt->second);
            instanceToLease_.erase(leaseIt);
        }
    }

    void gcLoop() {
        while (gcRunning_.load()) {
            {
                std::unique_lock<std::mutex> lk(gcMu_);
                gcCv_.wait_for(lk, std::chrono::seconds(kGcIntervalSec),
                               [this]() { return !gcRunning_.load(); });
                if (!gcRunning_.load()) return;
            }

            auto now = nowSteady();
            std::vector<std::string> dead;
            {
                std::lock_guard<std::mutex> lk(mu_);
                for (auto& [id, ts] : lastSeen_) {
                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - ts).count();
                    if (elapsed > kTtlSec) dead.push_back(id);
                }
                for (auto& id : dead) removeInstanceLocked(id, "ttl-expired");
            }
        }
    }

    std::mutex mu_;
    std::unordered_map<std::string, im::Endpoint> endpoints_;
    std::unordered_map<std::string, SteadyTp>     lastSeen_;       // instance_id → 上次 KeepAlive
    std::unordered_map<int64_t, std::string>      leaseToInstance_;
    std::unordered_map<std::string, int64_t>      instanceToLease_; // 反向，方便 re-register
    std::list<Watcher*> watchers_;
    int64_t leaseSeq_{0};

    // GC
    std::atomic<bool> gcRunning_{false};
    std::thread gcThread_;
    std::mutex gcMu_;
    std::condition_variable gcCv_;
};
