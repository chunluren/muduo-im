/**
 * @file test_routing_key.cpp
 * @brief Phase 2.2 — Protocol::privateRoutingKey / groupRoutingKey 单元测试
 *
 * 验证：
 * 1. 私聊 routingKey 方向无关 (A,B) == (B,A)
 * 2. 不同会话的 key 不冲突
 * 3. 64-bit 范围内不溢出
 * 4. groupRoutingKey 直通 groupId
 *
 * 业务意义：保证同会话消息在多 Logic 实例下被路由到同一实例处理，串行入库 → 有序。
 */

#include "common/Protocol.h"

#include <cassert>
#include <iostream>
#include <set>

namespace {

void testPrivateRoutingKeySymmetric() {
    std::cout << "=== testPrivateRoutingKeySymmetric ===" << std::endl;
    int64_t k1 = Protocol::privateRoutingKey(100, 200);
    int64_t k2 = Protocol::privateRoutingKey(200, 100);
    assert(k1 == k2);
    std::cout << "  (100,200) == (200,100) = " << k1 << " OK" << std::endl;
}

void testPrivateRoutingKeyDifferent() {
    std::cout << "=== testPrivateRoutingKeyDifferent ===" << std::endl;
    // 不同会话产生不同 key
    std::set<int64_t> keys;
    for (int a = 1; a <= 10; ++a) {
        for (int b = a + 1; b <= 10; ++b) {
            int64_t k = Protocol::privateRoutingKey(a, b);
            // 提出 side effect，避免 NDEBUG 优化掉 assert
            auto [it, inserted] = keys.insert(k);
            (void)it;
            assert(inserted && "duplicate routing key for distinct pair");
        }
    }
    // 10 个用户两两组合 = C(10,2) = 45
    assert(keys.size() == 45);
    std::cout << "  10x10 唯一性 OK，共 " << keys.size() << " 个 key" << std::endl;
}

void testPrivateRoutingKeyLargeIds() {
    std::cout << "=== testPrivateRoutingKeyLargeIds ===" << std::endl;
    // 用 32-bit 边界的 userId
    int64_t k1 = Protocol::privateRoutingKey(0xFFFFFFFFLL, 0x7FFFFFFFLL);
    int64_t k2 = Protocol::privateRoutingKey(0x7FFFFFFFLL, 0xFFFFFFFFLL);
    assert(k1 == k2);
    assert(k1 > 0);
    std::cout << "  large userId 对称 OK key=" << k1 << std::endl;
}

void testPrivateRoutingKeySelf() {
    std::cout << "=== testPrivateRoutingKeySelf ===" << std::endl;
    // 自己给自己（边界情况）
    int64_t k = Protocol::privateRoutingKey(42, 42);
    assert(k == ((42LL << 32) | 42LL));
    std::cout << "  self-message key=" << k << " OK" << std::endl;
}

void testGroupRoutingKey() {
    std::cout << "=== testGroupRoutingKey ===" << std::endl;
    assert(Protocol::groupRoutingKey(100) == 100);
    assert(Protocol::groupRoutingKey(0) == 0);
    assert(Protocol::groupRoutingKey(0x7FFFFFFFFFFFFFFFLL) == 0x7FFFFFFFFFFFFFFFLL);
    std::cout << "  groupRoutingKey 直通 OK" << std::endl;
}

void testHashDistribution() {
    std::cout << "=== testHashDistribution ===" << std::endl;
    // 模拟 4 个 Logic 实例，10000 个会话分布是否均匀
    int instances[4] = {0};
    for (int i = 1; i <= 100; ++i) {
        for (int j = i + 1; j <= 100; ++j) {
            int64_t k = Protocol::privateRoutingKey(i, j);
            instances[k % 4]++;
        }
    }
    int total = instances[0] + instances[1] + instances[2] + instances[3];
    int expected = total / 4;
    int tolerance = expected * 30 / 100;  // ±30% 误差
    for (int i = 0; i < 4; ++i) {
        std::cout << "  instance[" << i << "] = " << instances[i] << std::endl;
        assert(std::abs(instances[i] - expected) <= tolerance);
    }
    std::cout << "  4 实例分布均匀（±30%）OK" << std::endl;
}

}  // namespace

int main() {
    std::cout << "Starting RoutingKey tests..." << std::endl << std::endl;

    testPrivateRoutingKeySymmetric();
    testPrivateRoutingKeyDifferent();
    testPrivateRoutingKeyLargeIds();
    testPrivateRoutingKeySelf();
    testGroupRoutingKey();
    testHashDistribution();

    std::cout << std::endl << "All RoutingKey tests passed!" << std::endl;
    return 0;
}
