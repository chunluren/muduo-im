/**
 * @file RegistryServiceImpl.h
 * @brief Phase 1.2 W3.D1-D2：进程内 RegistryService（in-memory 单点）
 *
 * 简化版（生产换 etcd/Consul）：
 *  - Register：插入 endpoint，分配单调递增 lease_id
 *  - Deregister / KeepAlive：基本支持，KeepAlive 简单返回 ttl=30s 不做真实 TTL 清理
 *  - List：返回某 service 的全量
 *  - Watch：server-stream，注册时一次性下发现存条目（按 ADDED）；后续 add/remove 实时推
 *
 * 内部用一个全局 mutex 保护 endpoints_ + watchers_，watchers_ 用列表保存每个 watcher 的
 * 待写入队列；RegistryServiceImpl::Watch 自身是阻塞流，靠 condition_variable 唤醒。
 */
#pragma once

#include "im/registry.grpc.pb.h"
#include <condition_variable>
#include <deque>
#include <grpcpp/grpcpp.h>
#include <iostream>
#include <list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

class RegistryServiceImpl final : public im::RegistryService::Service {
public:
    grpc::Status Register(grpc::ServerContext* /*ctx*/,
                          const im::RegisterRequest* req,
                          im::RegisterResponse* resp) override {
        std::lock_guard<std::mutex> lk(mu_);
        const auto& ep = req->ep();
        endpoints_[ep.instance_id()] = ep;
        int64_t leaseId = ++leaseSeq_;
        leaseToInstance_[leaseId] = ep.instance_id();

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
        auto it = endpoints_.find(req->instance_id());
        if (it != endpoints_.end()) {
            broadcastEvent(im::WatchEvent::REMOVED, it->second);
            endpoints_.erase(it);
            std::cerr << "[registry] deregister " << req->instance_id() << "\n";
        }
        resp->mutable_status()->set_code(im::StatusCode::OK);
        return grpc::Status::OK;
    }

    grpc::Status KeepAlive(grpc::ServerContext* /*ctx*/,
                           const im::KeepAliveRequest* /*req*/,
                           im::KeepAliveResponse* resp) override {
        // 跳过真实 TTL 清理；写一个固定剩余时间，让客户端继续刷
        resp->mutable_status()->set_code(im::StatusCode::OK);
        resp->set_ttl_remaining(30);
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

    std::mutex mu_;
    std::unordered_map<std::string, im::Endpoint> endpoints_;
    std::unordered_map<int64_t, std::string> leaseToInstance_;
    std::list<Watcher*> watchers_;
    int64_t leaseSeq_{0};
};
