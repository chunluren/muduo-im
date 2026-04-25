/**
 * @file InstanceRouter.h
 * @brief 跨实例消息路由（Phase 1.3）
 *
 * 解决问题：多实例部署时 user A 在 instance-1，user B 在 instance-2，
 * A 发给 B 的消息默认只投递到 instance-1 的本地 OnlineManager（找不到 B）。
 *
 * 方案：Redis Pub/Sub
 * - 启动时订阅频道 `msg:instance:<instance_id>`（独立 hiredis 连接 + 单独线程）
 * - 投递失败时（本地查不到 session），调 publishToUser：
 *     1. 查 Redis 找 B 的 instance_id（通过 online:{uid}:* keys → 任一 value）
 *     2. PUBLISH 到对应实例频道
 * - 接收端订阅回调 → 调用本地交付函数
 *
 * 简化（vs 完整 Plan B § 1.3）：
 * - 不用 protobuf，用 JSON（便于调试）
 * - 没做消息可靠送达保证（Pub/Sub 是 at-most-once）
 *   关键消息走 MySQL 兜底（消息已落库，对端重连拉历史也能恢复）
 *
 * 单实例环境：仍可启用（订阅 + 发布到自己），用于验证机制。
 */
#pragma once

#include "common/Logging.h"
#include <hiredis/hiredis.h>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <thread>

class InstanceRouter {
public:
    using DeliverCallback = std::function<void(int64_t /*uid*/,
                                                  const std::string& /*deviceId*/,
                                                  const std::string& /*payload*/)>;

    InstanceRouter(const std::string& redisHost, int redisPort,
                    const std::string& instanceId)
        : host_(redisHost), port_(redisPort), instanceId_(instanceId) {}

    ~InstanceRouter() { stop(); }

    /// 设置消息到达回调（订阅频道收到 PUBLISH 时调用）
    void setDeliverCallback(DeliverCallback cb) { deliverCb_ = std::move(cb); }

    /// 启动订阅线程
    void start() {
        if (running_.exchange(true)) return;
        thread_ = std::thread([this]() { subscribeLoop(); });
    }

    /// 停止
    void stop() {
        if (!running_.exchange(false)) return;
        if (subCtx_) {
            // 用 disconnect 让 redisGetReply 解除阻塞
            redisFree(subCtx_);
            subCtx_ = nullptr;
        }
        if (thread_.joinable()) thread_.join();
    }

    /**
     * @brief 跨实例发送消息
     *
     * 流程：
     * 1. 用独立的 publish 连接 KEYS online:{uid}:* 找 B 在哪个实例
     * 2. PUBLISH 到 msg:instance:<targetInstance> 频道，payload 含 uid + deviceId + body
     *
     * @param uid 目标用户
     * @param payload 已序列化的 ws 消息（JSON）
     * @return 0 表示用户不在任何实例（包括本地）；>0 表示发给的实例数
     */
    int publishToUser(int64_t uid, const std::string& payload) {
        ensurePubCtx();
        if (!pubCtx_) return 0;
        // KEYS online:<uid>:* → 拿到 device key 列表
        // 简化：用一个 GET online:<uid>:* 不可行，改用 SCAN
        // 进一步简化：单 device 模式下 key 是 online:<uid>:<device_id>，
        // 我们用 KEYS 模式（生产数据量大不建议；这里是开发期可接受）
        std::string keysCmd = "KEYS online:" + std::to_string(uid) + ":*";
        redisReply* keysReply = (redisReply*)redisCommand(pubCtx_, keysCmd.c_str());
        if (!keysReply) return 0;
        std::set<std::string> targets;
        if (keysReply->type == REDIS_REPLY_ARRAY) {
            for (size_t i = 0; i < keysReply->elements; ++i) {
                redisReply* elem = keysReply->element[i];
                if (elem->type != REDIS_REPLY_STRING) continue;
                // GET 拿 instance_id
                redisReply* val = (redisReply*)redisCommand(pubCtx_, "GET %s", elem->str);
                if (val && val->type == REDIS_REPLY_STRING) {
                    targets.insert(val->str);
                }
                if (val) freeReplyObject(val);
            }
        }
        freeReplyObject(keysReply);
        if (targets.empty()) return 0;

        int published = 0;
        for (auto& targetInstance : targets) {
            if (targetInstance == instanceId_) continue;  // 本地不发跨实例
            std::string channel = "msg:instance:" + targetInstance;
            // payload 由调用方包含 uid 信息，这里直接转发
            redisReply* pubReply = (redisReply*)redisCommand(
                pubCtx_, "PUBLISH %s %b", channel.c_str(),
                payload.c_str(), payload.size());
            if (pubReply) {
                published++;
                freeReplyObject(pubReply);
            }
        }
        return published;
    }

    bool isRunning() const { return running_.load(); }

private:
    void ensurePubCtx() {
        if (pubCtx_ && pubCtx_->err == 0) return;
        if (pubCtx_) { redisFree(pubCtx_); pubCtx_ = nullptr; }
        struct timeval tv{1, 0};
        pubCtx_ = redisConnectWithTimeout(host_.c_str(), port_, tv);
        if (pubCtx_ && pubCtx_->err) {
            redisFree(pubCtx_);
            pubCtx_ = nullptr;
        }
    }

    void subscribeLoop() {
        std::string channel = "msg:instance:" + instanceId_;
        while (running_.load()) {
            // 重连
            struct timeval tv{2, 0};
            subCtx_ = redisConnectWithTimeout(host_.c_str(), port_, tv);
            if (!subCtx_ || subCtx_->err) {
                LOG_WARN_JSON("instance_router_redis_connect_fail", host_);
                if (subCtx_) { redisFree(subCtx_); subCtx_ = nullptr; }
                std::this_thread::sleep_for(std::chrono::seconds(2));
                continue;
            }
            redisReply* sub = (redisReply*)redisCommand(
                subCtx_, "SUBSCRIBE %s", channel.c_str());
            if (sub) freeReplyObject(sub);

            LOG_EVENT("instance_router_subscribed", channel);

            // 阻塞读消息
            while (running_.load() && subCtx_ && subCtx_->err == 0) {
                redisReply* reply = nullptr;
                if (redisGetReply(subCtx_, (void**)&reply) != REDIS_OK) break;
                if (!reply) break;
                handleSubReply(reply);
                freeReplyObject(reply);
            }
            if (subCtx_) { redisFree(subCtx_); subCtx_ = nullptr; }
            if (running_) std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    void handleSubReply(redisReply* reply) {
        // 期望格式: ["message", channel, payload]
        if (reply->type != REDIS_REPLY_ARRAY) return;
        if (reply->elements < 3) return;
        if (!reply->element[0] || !reply->element[2]) return;
        if (std::string(reply->element[0]->str) != "message") return;
        std::string payload = reply->element[2]->str;

        // 解析 payload，提取 uid / deviceId（可选）
        // payload 来自上层，已经是完整 ws 消息 JSON。这里我们让回调自行解析。
        if (deliverCb_) {
            // uid / deviceId 上层从 payload 里自己解
            deliverCb_(0, "", payload);
        }
    }

    std::string host_;
    int port_;
    std::string instanceId_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    redisContext* pubCtx_ = nullptr;
    redisContext* subCtx_ = nullptr;
    DeliverCallback deliverCb_;
};
