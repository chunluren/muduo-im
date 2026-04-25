/**
 * @file PushService.h
 * @brief 离线推送服务（Phase 5.1，APNs/FCM）
 *
 * 当前实现：**mock 模式** —— 把推送任务写入 Redis 队列 `push_queue`，
 * 由独立 push_worker（Python 实现）消费并真实调用 APNs/FCM。
 *
 * 主进程不直接调用第三方 API 的好处：
 * - 主进程不需要 HTTP/2（APNs）和 OAuth2（FCM）依赖
 * - 推送失败不影响业务主流程
 * - worker 可独立扩缩容
 *
 * 触发条件：
 * - 私聊 / 群聊推送时，对端无在线 session（OnlineManager.isOnline == false）
 * - 该用户在 user_devices 有 apns_token 或 fcm_token
 *
 * 任务格式：
 * @code
 * {
 *   "uid": 123,
 *   "device_id": "ios-uuid",
 *   "device_type": "ios",
 *   "apns_token": "...",
 *   "fcm_token": "...",
 *   "title": "张三",
 *   "body": "新消息",
 *   "data": { "msgId": "...", "from": "..." }
 * }
 * @endcode
 */
#pragma once

#include "pool/RedisPool.h"
#include "server/DeviceService.h"
#include <memory>
#include <nlohmann/json.hpp>

class PushService {
public:
    PushService(std::shared_ptr<RedisPool> redis,
                 std::shared_ptr<DeviceService> deviceSvc)
        : redis_(std::move(redis)), deviceSvc_(std::move(deviceSvc)) {}

    /**
     * @brief 推送离线通知（异步，立即返回）
     *
     * @param uid     接收用户
     * @param title   通知标题（如"张三"）
     * @param body    通知内容（如"新消息"）；不传敏感全文，前端拉取时再展开
     * @param data    附加数据（msgId / from / convType…），客户端用于落地页跳转
     * @return 入队的任务数（=该用户有 push token 的设备数）
     */
    int notifyOffline(int64_t uid, const std::string& title,
                       const std::string& body,
                       const nlohmann::json& data = nlohmann::json::object()) {
        if (!redis_ || !deviceSvc_) return 0;
        auto targets = deviceSvc_->getPushTargets(uid);
        if (targets.empty()) return 0;

        auto rconn = redis_->acquire(1000);
        if (!rconn || !rconn->valid()) return 0;
        int enqueued = 0;
        for (auto& t : targets) {
            // 至少一个 token 才入队
            if (t.apnsToken.empty() && t.fcmToken.empty()) continue;
            nlohmann::json job = {
                {"uid", uid},
                {"device_id", t.deviceId},
                {"device_type", t.deviceType},
                {"apns_token", t.apnsToken},
                {"fcm_token", t.fcmToken},
                {"title", title},
                {"body", body},
                {"data", data}
            };
            rconn->lpush("push_queue", job.dump());
            enqueued++;
        }
        redis_->release(std::move(rconn));
        return enqueued;
    }

private:
    std::shared_ptr<RedisPool> redis_;
    std::shared_ptr<DeviceService> deviceSvc_;
};
