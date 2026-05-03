/**
 * @file test_outbox_e2e.cpp
 * @brief Phase 1.2 Outbox 端到端：DB 写 + outbox INSERT 同事务 →
 *        OutboxRelay → KafkaProducer → Kafka topic im.messages
 *
 * 前置：MySQL 起 + outbox 表已建（来自 sql/init.sql）+ Kafka @ localhost:9092
 *       + im.messages topic 已建。
 */
#include "server/OutboxService.h"
#include "util/KafkaConsumer.h"
#include "pool/MySQLPool.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <iostream>
#include <random>
#include <string>
#include <thread>

#define TEST(name) static void name()
#define RUN_TEST(name) do { std::cerr << "[run] " #name "\n"; name(); std::cerr << "[ok]  " #name "\n"; } while (0)

static std::shared_ptr<MySQLPool> mkPool() {
    MySQLPoolConfig cfg;
    cfg.host = "127.0.0.1"; cfg.port = 3306;
    cfg.user = "root"; cfg.password = "";
    cfg.database = "muduo_im";
    cfg.minSize = 2; cfg.maxSize = 4;
    return std::make_shared<MySQLPool>(cfg);
}

static std::shared_ptr<KafkaProducer> mkKafka() {
    auto k = std::make_shared<KafkaProducer>(
        std::vector<std::string>{"localhost:9092"}, "test-outbox");
    k->start();
    return k;
}

TEST(outbox_insert_and_relay_to_kafka) {
    auto db = mkPool();
    auto kafka = mkKafka();
    OutboxService outbox(db, kafka);

    // 唯一 marker 区分本次测试，避免跟其它消息混淆
    std::random_device rd;
    std::string marker = "outbox-test-" + std::to_string(rd());
    std::string payload = R"({"marker":")" + marker + R"(","content":"hello via outbox"})";
    std::string key = "test-key-" + marker;

    // 1) 在事务里 INSERT outbox（业务表为简化此处不写）
    auto conn = db->acquire(2000);
    assert(conn && conn->valid());
    TransactionGuard tx(conn);
    assert(tx.active());
    bool inserted = outbox.insertOutbox(conn, "im.messages", key, payload);
    assert(inserted);
    assert(tx.commit());
    db->release(std::move(conn));

    // 2) 起 KafkaConsumer 等这条消息从 Kafka 出来
    std::string group = "test-outbox-group-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    KafkaConsumer consumer({"localhost:9092"}, group, {"im.messages"});

    std::atomic<bool> seen{false};
    consumer.start([&](const std::string&, int32_t, int64_t,
                       const std::string& k, const std::string& v) -> bool {
        if (v.find(marker) != std::string::npos) {
            seen = true;
        }
        return true;
    });

    // 3) 启动 relay，让它把 pending 推到 Kafka
    outbox.start(/*intervalMs=*/100, /*batch=*/50);

    // 4) 等最多 10s
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!seen.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    assert(seen.load());

    // 5) 验证 outbox 行被标记为 sent
    auto conn2 = db->acquire(1500);
    auto res = conn2->query(
        "SELECT status FROM outbox WHERE msg_key='" + key + "' ORDER BY id DESC LIMIT 1");
    assert(res);
    MYSQL_ROW row = mysql_fetch_row(res.get());
    assert(row && std::string(row[0]) == "sent");
    res.reset();
    db->release(std::move(conn2));

    outbox.stop();
    consumer.stop();
    kafka->stop();
}

int main() {
    if (std::getenv("SKIP_KAFKA_TEST")) {
        std::cerr << "SKIP\n";
        return 0;
    }
    RUN_TEST(outbox_insert_and_relay_to_kafka);
    std::cerr << "ALL OK\n";
    return 0;
}
