/**
 * @file persister/main.cpp
 * @brief muduo-im-persister — Kafka 消息持久化消费者
 *
 * 角色（Phase 1.3）：消费 im.messages topic，把消息**幂等**地写到 MySQL
 * private_messages / group_messages 表。
 *
 * 当前过渡期：producer (ChatServer) 仍直接写 private_messages。本进程的
 * INSERT 走 ON DUPLICATE KEY UPDATE id=id，重复就静默不动；既不会破坏数据，
 * 也能在"producer 不写 DB 只写 outbox"的目标态下成为唯一写者。
 *
 * 启动:
 *   MUDUO_IM_KAFKA_BROKERS=localhost:9092 \
 *   MUDUO_IM_GROUP=persister \
 *   ./muduo-im-persister ../config.ini
 *
 * 优雅停机：SIGINT/SIGTERM → consumer.stop()，已拉到的消息处理完才退。
 */
#include "common/Config.h"
#include "pool/MySQLPool.h"
#include "util/KafkaConsumer.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <signal.h>

using json = nlohmann::json;

static std::atomic<bool> g_running{true};
static void onSignal(int sig) {
    std::cerr << "[persister] signal " << sig << ", stopping\n";
    g_running.store(false);
}

static bool persistPrivate(MySQLPool& db, const json& j) {
    auto conn = db.acquire(2000);
    if (!conn || !conn->valid()) return false;
    PreparedStatement stmt(conn,
        "INSERT INTO private_messages "
        " (msg_id, from_user, to_user, content, timestamp) "
        " VALUES (?, ?, ?, ?, ?) "
        "ON DUPLICATE KEY UPDATE id=id");  // 幂等：msg_id UNIQUE，重复直接 no-op
    if (!stmt.valid()) {
        db.release(std::move(conn));
        return false;
    }
    stmt.bindString(1, j.value("msgId", ""));
    stmt.bindInt64(2,  j.value("from", (int64_t)0));
    stmt.bindInt64(3,  j.value("to",   (int64_t)0));
    stmt.bindString(4, j.value("content", ""));
    stmt.bindInt64(5,  j.value("timestamp", (int64_t)0));
    bool ok = stmt.execute();
    db.release(std::move(conn));
    return ok;
}

static bool persistGroup(MySQLPool& db, const json& j) {
    auto conn = db.acquire(2000);
    if (!conn || !conn->valid()) return false;
    std::string mentions = j.value("mentions", "");
    const char* sql =
        "INSERT INTO group_messages "
        " (msg_id, group_id, from_user, content, timestamp, mentions) "
        " VALUES (?, ?, ?, ?, ?, CASE WHEN ? = '' THEN NULL ELSE CAST(? AS JSON) END) "
        "ON DUPLICATE KEY UPDATE id=id";
    PreparedStatement stmt(conn, sql);
    if (!stmt.valid()) {
        db.release(std::move(conn));
        return false;
    }
    stmt.bindString(1, j.value("msgId", ""));
    stmt.bindInt64(2,  j.value("groupId", (int64_t)0));
    stmt.bindInt64(3,  j.value("from", (int64_t)0));
    stmt.bindString(4, j.value("content", ""));
    stmt.bindInt64(5,  j.value("timestamp", (int64_t)0));
    stmt.bindString(6, mentions);
    stmt.bindString(7, mentions);
    bool ok = stmt.execute();
    db.release(std::move(conn));
    return ok;
}

int main(int argc, char* argv[]) {
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    std::string configFile = (argc > 1) ? argv[1] : "../config.ini";
    AppConfig config;
    if (!config.load(configFile)) {
        std::cerr << "[persister] WARN: config not found: " << configFile << "\n";
    }

    // MySQL 池
    MySQLPoolConfig dbCfg;
    dbCfg.host     = config.get("mysql.host", "127.0.0.1");
    dbCfg.port     = config.getInt("mysql.port", 3306);
    dbCfg.user     = config.get("mysql.user", "root");
    dbCfg.password = config.get("mysql.password", "");
    dbCfg.database = config.get("mysql.database", "muduo_im");
    dbCfg.minSize  = config.getInt("mysql.pool_min", 3);
    dbCfg.maxSize  = config.getInt("mysql.pool_max", 8);
    auto db = std::make_shared<MySQLPool>(dbCfg);

    // Kafka 配置
    std::string brokers = "localhost:9092";
    if (const char* e = std::getenv("MUDUO_IM_KAFKA_BROKERS")) brokers = e;
    std::string group   = "persister";
    if (const char* e = std::getenv("MUDUO_IM_GROUP")) group = e;

    KafkaConsumer consumer({brokers}, group, {"im.messages"});

    std::atomic<uint64_t> seenPrivate{0}, seenGroup{0}, errors{0};
    bool ok = consumer.start(
        [&](const std::string& /*topic*/, int32_t /*part*/, int64_t /*offset*/,
            const std::string& /*key*/, const std::string& value) -> bool {
            json j = json::parse(value, nullptr, false);
            if (j.is_discarded()) {
                std::cerr << "[persister] bad json, skipping\n";
                errors.fetch_add(1);
                return true;  // commit 跳过坏消息（避免无限重投）
            }
            std::string type = j.value("type", "");
            bool persisted = false;
            if (type == "private") {
                persisted = persistPrivate(*db, j);
                if (persisted) seenPrivate.fetch_add(1);
            } else if (type == "group") {
                persisted = persistGroup(*db, j);
                if (persisted) seenGroup.fetch_add(1);
            } else {
                std::cerr << "[persister] unknown type=" << type
                          << " msgId=" << j.value("msgId", "") << " — skip\n";
                return true;
            }
            if (!persisted) {
                std::cerr << "[persister] persist failed msgId="
                          << j.value("msgId", "") << " — will retry\n";
                errors.fetch_add(1);
                return false;  // 不 commit；下次再试（DB 抖动恢复后能拉回）
            }
            return true;
        });

    if (!ok) {
        std::cerr << "[persister] consumer start failed\n";
        return 1;
    }
    std::cerr << "[persister] running, broker=" << brokers
              << " group=" << group << " topic=im.messages\n";

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::cerr << "[persister] private=" << seenPrivate.load()
                  << " group=" << seenGroup.load()
                  << " errors=" << errors.load()
                  << " consumed=" << consumer.consumed()
                  << " retries=" << consumer.retries() << "\n";
    }
    consumer.stop();
    std::cerr << "[persister] stopped\n";
    return 0;
}
