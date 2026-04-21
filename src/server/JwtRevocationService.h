/**
 * @file JwtRevocationService.h
 * @brief JWT 主动吊销服务 —— 基于 Redis 黑名单实现精准 token 失效
 *
 * 背景：HS256 JWT 一旦签发，签名和 exp 固定，无法在服务端主动使之失效。
 * 典型需求——登出、密码变更、账号封禁——都需要能立即废掉已签发的 token。
 *
 * 方案：给每个 JWT 加唯一 jti（JWT ID，UUID v4），签发时写 "当前有效 jti"。
 * 当需要吊销时，把 jti 写入 Redis 黑名单 key `jwt_blacklist:{jti}`，
 * TTL = token 剩余寿命（过期后 Redis 自动清理，不占空间）。
 *
 * 每次请求鉴权时除了验证签名 + 过期，还要查黑名单，命中则拒绝。
 *
 * 容错：Redis 不可用时 `isRevoked` 返回 false（放行）+ 触发告警。
 * 这是可用性与安全的 trade-off——Redis 挂了期间已经登出的用户可能重新生效，
 * 但服务不会因为 Redis 挂了整体挂。
 *
 * 线程安全：每次获取 Redis 连接即释放，不持有长连接。
 *
 * @example
 * @code
 * JwtRevocationService revocation(redisPool);
 * // 登出时
 * revocation.revoke(jti, claims.remainingSeconds());
 * // 鉴权时
 * if (revocation.isRevoked(jti)) return 401;
 * @endcode
 */
#pragma once

#include "common/Logging.h"
#include "pool/RedisPool.h"

#include <memory>
#include <string>

/**
 * @class JwtRevocationService
 * @brief Redis 黑名单 JWT 吊销管理
 */
class JwtRevocationService {
public:
    /// Redis key 前缀，用于吊销的 jti 列表
    static constexpr const char* kKeyPrefix = "jwt_blacklist:";

    explicit JwtRevocationService(std::shared_ptr<RedisPool> redis)
        : redis_(std::move(redis)) {}

    /**
     * @brief 把 jti 加入黑名单
     *
     * Redis key `jwt_blacklist:{jti}` 设 TTL = expireSeconds，过期后自动清理。
     * 调用方负责传入 token 的"剩余寿命"（非原 expireSeconds），避免黑名单膨胀。
     *
     * @param jti JWT ID（UUID v4），来自待吊销 token 的 payload
     * @param expireSeconds Redis key TTL，应等于 token 的 `exp - now`
     * @return true 成功写入；false jti 为空 / Redis 不可用
     */
    bool revoke(const std::string& jti, int64_t expireSeconds) {
        if (jti.empty() || expireSeconds <= 0) {
            return false;
        }
        auto conn = redis_->acquire(1000);
        if (!conn || !conn->valid()) {
            LOG_WARN_JSON("jwt_revoke_redis_unavailable", "jti=" + jti);
            return false;
        }
        bool ok = conn->set(kKeyPrefix + jti, "1",
                            static_cast<int>(expireSeconds));
        redis_->release(std::move(conn));
        if (ok) {
            LOG_EVENT("jwt_revoked",
                      "jti=" + jti + " ttl=" + std::to_string(expireSeconds));
        }
        return ok;
    }

    /**
     * @brief 查 jti 是否已被吊销
     *
     * Redis 不可用时返回 false（放行），同时发告警日志。
     *
     * @param jti 从 token payload 解析出的 jti
     * @return true 在黑名单中；false 不在 / 空 jti / Redis 不可用
     */
    bool isRevoked(const std::string& jti) {
        if (jti.empty()) return false;  // 旧 token 没 jti，放行（兼容）
        auto conn = redis_->acquire(500);
        if (!conn || !conn->valid()) {
            LOG_WARN_JSON("jwt_check_redis_unavailable", "jti=" + jti);
            return false;  // 降级：可用性 > 严格安全
        }
        bool revoked = conn->exists(kKeyPrefix + jti);
        redis_->release(std::move(conn));
        return revoked;
    }

private:
    std::shared_ptr<RedisPool> redis_;
};
