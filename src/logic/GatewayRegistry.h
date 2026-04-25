/**
 * @file GatewayRegistry.h
 * @brief Phase 1.2 W2.D4-D5：logic 侧的 gateway 在线表
 *
 * 记录两件事：
 *  1. gateway_id → 当前活跃的 RegisterGateway 双向流写句柄（写下行 PushCommand）
 *  2. uid → gateway_id（uid 当前连在哪个 gateway 上；多 device 同 gateway 共用 gateway_id；
 *     跨 gateway 多端登录暂记最后一次见到的 gateway_id，覆盖前的会被自然失效）
 *
 * 线程安全：用 mutex；写下行的并发由 stream->Write 自身顺序化（gRPC 内部加锁）。
 */
#pragma once

#include "im/logic.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

class GatewayRegistry {
public:
    using StreamPtr =
        grpc::ServerReaderWriter<im::PushCommand, im::GatewayEvent>*;

    /// gateway 注册：在 RegisterGateway 收到第一帧 Hello 时调
    void registerGateway(const std::string& gatewayId, StreamPtr stream) {
        std::lock_guard<std::mutex> lk(mu_);
        gateways_[gatewayId] = stream;
    }

    /// gateway 流断开：清理所有挂在它上面的 uid
    void deregisterGateway(const std::string& gatewayId) {
        std::lock_guard<std::mutex> lk(mu_);
        gateways_.erase(gatewayId);
        for (auto it = userToGateway_.begin(); it != userToGateway_.end();) {
            if (it->second == gatewayId) it = userToGateway_.erase(it);
            else ++it;
        }
    }

    /// 用户在某 gateway 上线
    void onUserOpen(int64_t uid, const std::string& gatewayId) {
        std::lock_guard<std::mutex> lk(mu_);
        userToGateway_[uid] = gatewayId;
    }

    /// 用户离线
    void onUserClose(int64_t uid, const std::string& gatewayId) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = userToGateway_.find(uid);
        if (it != userToGateway_.end() && it->second == gatewayId) {
            userToGateway_.erase(it);
        }
    }

    /**
     * 把 PushToUser 命令推到 uid 所在 gateway 的下行流。
     * @return true 若 uid 在线且 Write 调用返回 true；false 表示 uid 不在或对端流挂了
     */
    bool pushToUser(int64_t uid, const std::string& payload) {
        std::lock_guard<std::mutex> lk(mu_);
        auto pit = userToGateway_.find(uid);
        if (pit == userToGateway_.end()) return false;
        auto git = gateways_.find(pit->second);
        if (git == gateways_.end()) return false;

        im::PushCommand cmd;
        auto* pu = cmd.mutable_push_user();
        pu->set_uid(uid);
        pu->set_payload(payload);
        return git->second->Write(cmd);
    }

private:
    std::mutex mu_;
    std::unordered_map<std::string, StreamPtr> gateways_;
    std::unordered_map<int64_t, std::string> userToGateway_;
};
