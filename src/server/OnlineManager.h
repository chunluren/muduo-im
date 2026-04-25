/**
 * @file OnlineManager.h
 * @brief 在线状态管理器（Phase 3.1：多端 session 重构）
 *
 * 数据模型变更：
 *   v1（旧）: uid → Session*               每用户单 session
 *   v2（本）: uid → map<device_id, Session*>  每用户多设备同时在线
 *
 * 协议升级：
 * - WebSocket 握手 URL 加 device_id 参数（详见 docs/PROTOCOL.md §2.2）
 * - Redis online:{uid} 从 STRING 改为 HASH： {device_id: instance_id, ...}
 *   - 任一字段存在视为在线
 *   - 单设备下线 = HDEL；最后一个设备下线 = DEL 整个 key
 *
 * 向后兼容：
 * - 旧的无 deviceId 调用（addUser/getSession）会用 device_id="default" 占位
 *   保证既有代码继续工作
 *
 * 推送语义：
 * - 私聊推送 → getSessions(uid) 拿所有设备 → 全部 send
 * - 已读 / 编辑等同 uid 内同步 → 用 getOtherSessions(uid, exclude) 跳过自己
 *
 * 线程安全：std::shared_mutex 读写分离（getSessions 高频读、addUser 偶发写）
 */
#pragma once

#include "pool/RedisPool.h"
#include "websocket/WsSession.h"

#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

class OnlineManager {
public:
    static constexpr const char* kDefaultDevice = "default";

    explicit OnlineManager(std::shared_ptr<RedisPool> redis = nullptr,
                            std::string instanceId = "instance-0")
        : redis_(redis), instanceId_(std::move(instanceId)) {}

    /// 注册：(uid, deviceId, session) — 多端模型
    void addDevice(int64_t userId, const std::string& deviceId,
                    const WsSessionPtr& session) {
        std::string dev = deviceId.empty() ? kDefaultDevice : deviceId;
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            sessions_[userId][dev] = session;
        }
        if (redis_) {
            auto conn = redis_->acquire(1000);
            if (conn && conn->valid()) {
                // online:{uid}:{device_id} = instance_id（带 TTL）
                // 拆分 key 是因为 RedisPool 当前不支持 HSET，但语义等价
                conn->set("online:" + std::to_string(userId) + ":" + dev,
                           instanceId_, 30);
                redis_->release(std::move(conn));
            }
        }
    }

    /// 旧 API：默认设备登录（向后兼容）
    void addUser(int64_t userId, const WsSessionPtr& session) {
        addDevice(userId, kDefaultDevice, session);
    }

    /// 单设备下线
    void removeDevice(int64_t userId, const std::string& deviceId) {
        std::string dev = deviceId.empty() ? kDefaultDevice : deviceId;
        bool lastOne = false;
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            auto it = sessions_.find(userId);
            if (it != sessions_.end()) {
                it->second.erase(dev);
                if (it->second.empty()) {
                    sessions_.erase(it);
                    lastOne = true;
                }
            }
        }
        if (redis_) {
            auto conn = redis_->acquire(1000);
            if (conn && conn->valid()) {
                conn->del("online:" + std::to_string(userId) + ":" + dev);
                if (lastOne) {
                    // 兼容旧 key（v1 单 session 模式）
                    conn->del("online:" + std::to_string(userId));
                }
                redis_->release(std::move(conn));
            }
        }
    }

    /// 旧 API：用户全部下线（所有设备）
    void removeUser(int64_t userId) {
        std::vector<std::string> devices;
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            auto it = sessions_.find(userId);
            if (it != sessions_.end()) {
                for (auto& [d, _] : it->second) devices.push_back(d);
                sessions_.erase(it);
            }
        }
        if (redis_) {
            auto conn = redis_->acquire(1000);
            if (conn && conn->valid()) {
                for (auto& d : devices) {
                    conn->del("online:" + std::to_string(userId) + ":" + d);
                }
                conn->del("online:" + std::to_string(userId));
                redis_->release(std::move(conn));
            }
        }
    }

    /// 心跳续期所有该用户的 device key
    void refreshOnline(int64_t userId) {
        std::vector<std::string> devs;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            auto it = sessions_.find(userId);
            if (it != sessions_.end()) {
                for (auto& [d, _] : it->second) devs.push_back(d);
            }
        }
        if (redis_ && !devs.empty()) {
            auto conn = redis_->acquire(1000);
            if (conn && conn->valid()) {
                for (auto& d : devs) {
                    conn->expire("online:" + std::to_string(userId) + ":" + d, 30);
                }
                redis_->release(std::move(conn));
            }
        }
    }

    /// 获取该用户**所有**在线设备的 session（用于消息广播给多端）
    std::vector<WsSessionPtr> getSessions(int64_t userId) {
        std::vector<WsSessionPtr> result;
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = sessions_.find(userId);
        if (it == sessions_.end()) return result;
        for (auto& [_, s] : it->second) {
            if (s && s->isOpen()) result.push_back(s);
        }
        return result;
    }

    /// 旧 API：返回任一设备 session（兼容只考虑单 session 的旧代码）
    WsSessionPtr getSession(int64_t userId) {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = sessions_.find(userId);
        if (it == sessions_.end()) return nullptr;
        for (auto& [_, s] : it->second) {
            if (s && s->isOpen()) return s;
        }
        return nullptr;
    }

    /// 获取该用户除某 deviceId 之外的其他设备 session（用于多端同步：发起端不重发）
    std::vector<WsSessionPtr> getOtherSessions(int64_t userId,
                                                  const std::string& excludeDevice) {
        std::vector<WsSessionPtr> result;
        std::string excl = excludeDevice.empty() ? kDefaultDevice : excludeDevice;
        std::shared_lock<std::shared_mutex> lock(mutex_);
        auto it = sessions_.find(userId);
        if (it == sessions_.end()) return result;
        for (auto& [d, s] : it->second) {
            if (d == excl) continue;
            if (s && s->isOpen()) result.push_back(s);
        }
        return result;
    }

    bool isOnline(int64_t userId) {
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            auto it = sessions_.find(userId);
            if (it != sessions_.end()) {
                for (auto& [_, s] : it->second) {
                    if (s && s->isOpen()) return true;
                }
            }
        }
        // Redis 回退：检查任一 device key（开发期：仍保留旧 STRING key 兼容）
        if (redis_) {
            auto conn = redis_->acquire(1000);
            if (conn && conn->valid()) {
                bool online = conn->exists("online:" + std::to_string(userId));
                redis_->release(std::move(conn));
                return online;
            }
        }
        return false;
    }

    std::vector<int64_t> getOnlineUsers() {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        std::vector<int64_t> result;
        result.reserve(sessions_.size());
        for (auto& [uid, devs] : sessions_) {
            for (auto& [_, s] : devs) {
                if (s && s->isOpen()) {
                    result.push_back(uid);
                    break;  // 一个设备在线就算
                }
            }
        }
        return result;
    }

    /// 反向查找：从 session 找 (userId, deviceId)
    std::pair<int64_t, std::string> findByDeviceSession(const WsSessionPtr& session) {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        for (auto& [uid, devs] : sessions_) {
            for (auto& [d, s] : devs) {
                if (s == session) return {uid, d};
            }
        }
        return {-1, ""};
    }

    /// 兼容旧 API（只返回 userId）
    int64_t getUserId(const WsSessionPtr& session) {
        return findByDeviceSession(session).first;
    }

    /// Phase 3.1：管理端踢指定设备
    /// @return true 已踢出；false 未找到
    bool kickDevice(int64_t userId, const std::string& deviceId) {
        WsSessionPtr session;
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            auto it = sessions_.find(userId);
            if (it == sessions_.end()) return false;
            auto dit = it->second.find(deviceId);
            if (dit == it->second.end()) return false;
            session = dit->second;
            it->second.erase(dit);
            if (it->second.empty()) sessions_.erase(it);
        }
        if (session) {
            // 通知客户端被踢
            json kick = {{"type", "device_kicked"},
                          {"reason", "kicked_by_admin_or_other_device"},
                          {"deviceId", deviceId}};
            session->sendText(kick.dump());
            session->close(1000);
        }
        if (redis_) {
            auto conn = redis_->acquire(1000);
            if (conn && conn->valid()) {
                conn->del("online:" + std::to_string(userId) + ":" + deviceId);
                redis_->release(std::move(conn));
            }
        }
        return true;
    }

    /// 关闭所有 session（优雅关闭用）
    void closeAll() {
        std::vector<WsSessionPtr> toClose;
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            for (auto& [_, devs] : sessions_) {
                for (auto& [_, s] : devs) {
                    if (s && s->isOpen()) toClose.push_back(s);
                }
            }
            sessions_.clear();
        }
        for (auto& s : toClose) s->close(1000);
    }

private:
    using json = nlohmann::json;

    std::shared_ptr<RedisPool> redis_;
    std::string instanceId_;
    std::unordered_map<int64_t, std::unordered_map<std::string, WsSessionPtr>> sessions_;
    mutable std::shared_mutex mutex_;
};
