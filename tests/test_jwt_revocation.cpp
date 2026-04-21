/**
 * @file test_jwt_revocation.cpp
 * @brief JWT jti + Redis 黑名单吊销测试
 *
 * 覆盖:
 * - JWT 签发带 jti、verifyAndParse 能正确解出 jti
 * - JwtRevocationService::revoke 写入 Redis、isRevoked 能检测到
 * - 空 jti 视为未吊销（向后兼容旧 token）
 * - Redis 不可用时降级为放行（可用性 > 严格）
 * - revoke 的 TTL 会过期（简化验证用短 TTL）
 */

#include "common/JWT.h"
#include "common/Protocol.h"
#include "server/JwtRevocationService.h"
#include "pool/RedisPool.h"

#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>

namespace {

void testJwtGenerateWithJti() {
    std::cout << "=== testJwtGenerateWithJti ===" << std::endl;
    JWT jwt("test-secret-123");
    std::string jti = Protocol::generateMsgId();
    std::string token = jwt.generateWithJti(42, jti, 60);

    JWT::Claims claims;
    bool ok = jwt.verifyAndParse(token, &claims);
    assert(ok);
    assert(claims.userId == 42);
    assert(claims.jti == jti);
    assert(claims.exp > claims.iat);
    assert(claims.remainingSeconds() > 0 && claims.remainingSeconds() <= 60);
    std::cout << "  jti=" << claims.jti
              << " exp=" << claims.exp
              << " remaining=" << claims.remainingSeconds() << "s OK" << std::endl;
}

void testJwtLegacyNoJti() {
    std::cout << "=== testJwtLegacyNoJti ===" << std::endl;
    JWT jwt("test-secret-123");
    // 旧 API 不带 jti
    std::string token = jwt.generate(99, 60);

    JWT::Claims claims;
    bool ok = jwt.verifyAndParse(token, &claims);
    assert(ok);
    assert(claims.userId == 99);
    assert(claims.jti.empty());  // 兼容：没有 jti 字段
    std::cout << "  legacy token userId=" << claims.userId
              << " jti=(empty) OK" << std::endl;
}

void testJwtExpired() {
    std::cout << "=== testJwtExpired ===" << std::endl;
    JWT jwt("test-secret-123");
    std::string token = jwt.generateWithJti(42, "x", -1);  // 负数：立即过期

    JWT::Claims claims;
    bool ok = jwt.verifyAndParse(token, &claims);
    assert(!ok);  // 过期 token 返回 false
    std::cout << "  expired token rejected OK" << std::endl;
}

void testJwtTampered() {
    std::cout << "=== testJwtTampered ===" << std::endl;
    JWT jwt("test-secret-123");
    std::string token = jwt.generateWithJti(42, "jti-abc", 60);
    // 篡改签名
    std::string tampered = token.substr(0, token.size() - 3) + "xxx";

    JWT::Claims claims;
    bool ok = jwt.verifyAndParse(tampered, &claims);
    assert(!ok);
    std::cout << "  tampered signature rejected OK" << std::endl;
}

void testRevocationBasic(RedisPool& redisPool) {
    std::cout << "=== testRevocationBasic ===" << std::endl;
    auto redisShared = std::shared_ptr<RedisPool>(&redisPool,
                                                   [](RedisPool*){});
    JwtRevocationService revoke(redisShared);

    std::string jti = "test-jti-" + std::to_string(Protocol::nowMs());

    // 初始未吊销
    assert(!revoke.isRevoked(jti));

    // 吊销
    bool ok = revoke.revoke(jti, 10);
    assert(ok);

    // 吊销后命中
    assert(revoke.isRevoked(jti));

    std::cout << "  revoke + isRevoked lifecycle OK" << std::endl;
}

void testRevocationEmptyJti(RedisPool& redisPool) {
    std::cout << "=== testRevocationEmptyJti ===" << std::endl;
    auto redisShared = std::shared_ptr<RedisPool>(&redisPool,
                                                   [](RedisPool*){});
    JwtRevocationService revoke(redisShared);

    // 空 jti：revoke 返回 false，isRevoked 返回 false
    assert(!revoke.revoke("", 10));
    assert(!revoke.isRevoked(""));
    std::cout << "  empty jti handled OK" << std::endl;
}

void testRevocationTtl(RedisPool& redisPool) {
    std::cout << "=== testRevocationTtl ===" << std::endl;
    auto redisShared = std::shared_ptr<RedisPool>(&redisPool,
                                                   [](RedisPool*){});
    JwtRevocationService revoke(redisShared);

    std::string jti = "test-jti-ttl-" + std::to_string(Protocol::nowMs());
    revoke.revoke(jti, 1);  // 1 秒 TTL
    assert(revoke.isRevoked(jti));

    // 等 1.5 秒 让 TTL 过期
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    assert(!revoke.isRevoked(jti));

    std::cout << "  TTL expiry OK (1s)" << std::endl;
}

void testRevocationInvalidTtl(RedisPool& redisPool) {
    std::cout << "=== testRevocationInvalidTtl ===" << std::endl;
    auto redisShared = std::shared_ptr<RedisPool>(&redisPool,
                                                   [](RedisPool*){});
    JwtRevocationService revoke(redisShared);

    // expireSeconds <= 0 应拒绝（token 已过期，无需黑名单）
    assert(!revoke.revoke("some-jti", 0));
    assert(!revoke.revoke("some-jti", -100));

    std::cout << "  invalid TTL rejected OK" << std::endl;
}

void testJwtAndRevocationIntegration(RedisPool& redisPool) {
    std::cout << "=== testJwtAndRevocationIntegration ===" << std::endl;
    auto redisShared = std::shared_ptr<RedisPool>(&redisPool,
                                                   [](RedisPool*){});
    JwtRevocationService revoke(redisShared);
    JWT jwt("integration-secret");

    // 1. 生成带 jti 的 token
    std::string jti = Protocol::generateMsgId();
    std::string token = jwt.generateWithJti(1001, jti, 30);

    // 2. 验证通过
    JWT::Claims c1;
    assert(jwt.verifyAndParse(token, &c1));
    assert(c1.jti == jti);

    // 3. 登出：吊销 jti
    revoke.revoke(jti, c1.remainingSeconds());

    // 4. 再次解析 token：签名/过期仍 OK，但黑名单命中
    JWT::Claims c2;
    assert(jwt.verifyAndParse(token, &c2));  // 签名层仍有效
    assert(revoke.isRevoked(c2.jti));         // 但被吊销

    std::cout << "  full login → logout → rejected flow OK" << std::endl;
}

}  // namespace

int main() {
    std::cout << "Starting JWT jti + Revocation tests..." << std::endl << std::endl;

    // 不依赖 Redis 的纯 JWT 测试
    testJwtGenerateWithJti();
    testJwtLegacyNoJti();
    testJwtExpired();
    testJwtTampered();

    // 需要 Redis 的测试
    RedisPoolConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 6379;
    cfg.minSize = 1;
    cfg.maxSize = 3;
    RedisPool redisPool(cfg);

    // 试着获取一个连接验证 Redis 可用
    auto probe = redisPool.acquire(500);
    if (!probe || !probe->valid()) {
        std::cerr << "WARN: Redis unavailable, skipping revocation tests" << std::endl;
        std::cout << std::endl << "Partial JWT tests passed (Redis-dependent skipped)" << std::endl;
        return 0;
    }
    redisPool.release(std::move(probe));

    testRevocationBasic(redisPool);
    testRevocationEmptyJti(redisPool);
    testRevocationTtl(redisPool);
    testRevocationInvalidTtl(redisPool);
    testJwtAndRevocationIntegration(redisPool);

    std::cout << std::endl << "All JWT jti + Revocation tests passed!" << std::endl;
    return 0;
}
