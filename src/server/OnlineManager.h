/**
 * @file OnlineManager.h
 * @brief 在线状态管理器，双层存储（本地 sessions + Redis）
 *
 * 详细说明：
 * - 本地层：使用 unordered_map<userId, WsSessionPtr> 存储本机所有活跃的
 *   WebSocket 连接，支持 O(1) 快速查找，适用于单实例部署
 * - Redis 层：将用户在线状态以 "online:{userId}" 为 key 写入 Redis，
 *   设置 30 秒 TTL，客户端通过心跳定期续期；适用于分布式多实例部署，
 *   任何实例都可通过 Redis 判断用户是否在线（可能在其他实例上）
 * - 线程安全：sessions_ map 由 std::mutex 保护，所有读写操作均加锁
 *
 * @example 使用示例
 * @code
 * // 带 Redis 的分布式模式
 * auto redis = std::make_shared<RedisPool>(redisCfg);
 * OnlineManager mgr(redis);
 *
 * // 纯本地模式（不使用 Redis）
 * OnlineManager localMgr;  // redis 默认为 nullptr
 *
 * mgr.addUser(userId, session);
 * if (mgr.isOnline(targetId)) { ... }
 * @endcode
 */
#pragma once

#include "pool/RedisPool.h"
#include "websocket/WsSession.h"
#include <unordered_map>
#include <mutex>
#include <vector>
#include <memory>

/**
 * @class OnlineManager
 * @brief 用户在线状态管理器，采用本地 + Redis 双层存储架构
 *
 * 设计思路：
 * - 本地 sessions_ map 用于快速查找本机上的 WebSocket 连接，消息转发时
 *   直接通过 getSession() 获取连接对象并发送，避免网络开销
 * - Redis 用于分布式多实例场景，当用户不在本机时可通过 isOnline() 的
 *   Redis 回退路径判断用户是否在其他实例上在线
 * - 所有对 sessions_ map 的操作均由 mutex_ 保护，保证线程安全
 */
class OnlineManager {
public:
    /**
     * @brief 构造在线状态管理器
     *
     * @param redis Redis 连接池的共享指针，可选参数。
     *              传入 nullptr（默认值）表示纯本地模式，不使用 Redis 存储在线状态，
     *              适用于单实例部署；传入有效的 RedisPool 则启用双层存储，
     *              适用于分布式多实例部署
     */
    explicit OnlineManager(std::shared_ptr<RedisPool> redis = nullptr)
        : redis_(redis) {}

    /**
     * @brief 注册用户上线，将会话存入本地 map 并同步到 Redis
     *
     * 操作步骤：
     * 1. 加锁，将 userId → session 映射存入本地 sessions_ map
     * 2. 如果 Redis 可用，设置 "online:{userId}" = "1"，TTL 为 30 秒
     *    （TTL 机制：即使服务器异常崩溃未执行 removeUser，Redis 中的
     *    在线状态也会在 30 秒后自动过期，避免"幽灵在线"问题）
     *
     * @param userId  上线用户的 ID
     * @param session 该用户对应的 WebSocket 会话指针
     */
    void addUser(int64_t userId, const WsSessionPtr& session) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sessions_[userId] = session;
        }
        // Redis: set online status with TTL 30s
        if (redis_) {
            auto conn = redis_->acquire(1000);
            if (conn && conn->valid()) {
                conn->set("online:" + std::to_string(userId), "1", 30);
                redis_->release(std::move(conn));
            }
        }
    }

    /**
     * @brief 注册用户下线，从本地 map 删除并清除 Redis 在线状态
     *
     * 操作步骤：
     * 1. 加锁，从本地 sessions_ map 中移除该用户的会话
     * 2. 如果 Redis 可用，删除 "online:{userId}" 键，立即清除在线状态
     *
     * @param userId 下线用户的 ID
     */
    void removeUser(int64_t userId) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            sessions_.erase(userId);
        }
        // Redis: remove online status
        if (redis_) {
            auto conn = redis_->acquire(1000);
            if (conn && conn->valid()) {
                conn->del("online:" + std::to_string(userId));
                redis_->release(std::move(conn));
            }
        }
    }

    /**
     * @brief 刷新用户在线状态的 TTL（心跳续期）
     *
     * 心跳续期机制：客户端需定期（建议每 15-20 秒）发送心跳包，
     * 服务端收到后调用此方法将 Redis 中 "online:{userId}" 的 TTL
     * 重置为 30 秒。如果客户端停止发送心跳（如网络断开、进程崩溃），
     * TTL 到期后 Redis 自动删除该键，其他实例通过 isOnline() 查询
     * 时将判定该用户为离线
     *
     * @param userId 需要续期的用户 ID
     */
    void refreshOnline(int64_t userId) {
        if (redis_) {
            auto conn = redis_->acquire(1000);
            if (conn && conn->valid()) {
                conn->expire("online:" + std::to_string(userId), 30);
                redis_->release(std::move(conn));
            }
        }
    }

    /**
     * @brief 获取用户在本机上的 WebSocket 会话
     *
     * 仅查找本地 sessions_ map，不查询 Redis。
     * 返回 nullptr 意味着用户不在本机上，或者虽然在 map 中但连接已关闭。
     * 调用方拿到非空结果后可直接调用 session->sendText() 发送消息
     *
     * @param userId 目标用户 ID
     * @return 用户在本机的活跃 WebSocket 会话；不在线或连接已断开返回 nullptr
     */
    WsSessionPtr getSession(int64_t userId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(userId);
        if (it != sessions_.end() && it->second->isOpen()) {
            return it->second;
        }
        return nullptr;
    }

    /**
     * @brief 判断用户是否在线（双层查找）
     *
     * 查找策略：
     * 1. **本地快速路径**：先查本地 sessions_ map，如果找到且连接处于打开状态，
     *    立即返回 true，无网络开销
     * 2. **Redis 回退路径**：本地未找到时，查询 Redis 中 "online:{userId}" 是否存在。
     *    该键存在说明用户在其他服务器实例上在线（分布式多实例场景）
     *
     * @param userId 目标用户 ID
     * @return true 表示用户在线（本机或其他实例），false 表示用户离线
     */
    bool isOnline(int64_t userId) {
        // Check local first (fast path)
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sessions_.find(userId);
            if (it != sessions_.end() && it->second->isOpen()) return true;
        }
        // Fallback to Redis (for multi-instance)
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

    /**
     * @brief 获取本机所有在线用户的 ID 列表
     *
     * 注意：只返回本机 sessions_ map 中连接处于打开状态的用户，
     * 不包含其他服务器实例上的在线用户。如需全局在线用户列表，
     * 需在 Redis 层另行实现（如使用 Redis SET 存储所有在线 userId）
     *
     * @return 本机在线用户 ID 的 vector
     */
    std::vector<int64_t> getOnlineUsers() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<int64_t> users;
        for (auto& [id, session] : sessions_) {
            if (session->isOpen()) users.push_back(id);
        }
        return users;
    }

    /**
     * @brief 根据 WebSocket 会话反向查找用户 ID
     *
     * 遍历本地 sessions_ map，通过指针比较找到匹配的会话。
     * 时间复杂度 O(n)，适用于在线用户数不大的场景
     *
     * @param session 要查找的 WebSocket 会话指针
     * @return 找到则返回对应的用户 ID；未找到返回 -1
     */
    int64_t getUserId(const WsSessionPtr& session) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, s] : sessions_) {
            if (s == session) return id;
        }
        return -1;
    }

private:
    std::shared_ptr<RedisPool> redis_;                      ///< Redis 连接池（可为 nullptr，表示纯本地模式）
    std::unordered_map<int64_t, WsSessionPtr> sessions_;    ///< 本地在线用户表：userId → WebSocket 会话映射
    std::mutex mutex_;                                      ///< 互斥锁，保护 sessions_ map 的线程安全读写
};
