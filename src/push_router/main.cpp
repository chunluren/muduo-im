/**
 * @file push_router/main.cpp
 * @brief muduo-im-push-router — Kafka 消息推送路由器（Phase 1.4 + 6.3）
 *
 * 角色：消费 im.messages topic，把每条消息**重新打包成 ws 协议帧**，
 *       produce 到 im.push.commands（key = 收件人 uid）。所有 gateway /
 *       ChatServer 实例订阅 im.push.commands，只对本地有 session 的 uid
 *       做投递。
 *
 * 两种模式：
 *
 *   1. **at-least-once**（默认）：KafkaConsumer 每处理完一条 commit offset
 *      故障情况下下游可能见到重复事件（~1% 实测重复率）；下游 ChatServer
 *      用 msgId + Redis seen_push 兜底幂等。
 *
 *   2. **transactional EOS**（Phase 6.3，env MUDUO_IM_PUSH_ROUTER_TRANSACTIONAL=1）：
 *      read-process-write 三步原子：
 *        - producer.beginTransaction
 *        - 处理这一批 records 全部 produce
 *        - producer.sendOffsetsToTransaction(consumer.position, groupMeta)
 *        - producer.commitTransaction（消费者 offset 跟 produce 一起原子提交）
 *      下游配 isolation.level=read_committed 跳过 abort 掉的事务。
 *      重复率应降到 0.01% 以下（仅取决于产端到产端的 idempotence guard）。
 *
 * 启动:
 *   MUDUO_IM_KAFKA_BROKERS=localhost:9092 \
 *   MUDUO_IM_GROUP=push-router \
 *   [MUDUO_IM_PUSH_ROUTER_TRANSACTIONAL=1] \
 *   [MUDUO_IM_PUSH_ROUTER_TX_ID=push-router-1] \
 *   ./muduo-im-push-router
 */
#include "util/KafkaConsumer.h"
#include "util/KafkaProducer.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <signal.h>
#include <thread>
#include <vector>

using json = nlohmann::json;

static std::atomic<bool> g_running{true};
static void onSignal(int sig) {
    std::cerr << "[push-router] signal " << sig << ", stopping\n";
    g_running.store(false);
}

/// 处理单条 im.messages：返回要 produce 的 (key, value) 列表
/// （提取出来给两种模式共用）
struct PushOut { std::string key; std::string value; };

static std::vector<PushOut> route(const std::string& payload, std::atomic<uint64_t>& badJson) {
    std::vector<PushOut> out;
    json j = json::parse(payload, nullptr, false);
    if (j.is_discarded()) {
        badJson.fetch_add(1);
        return out;
    }
    std::string type = j.value("type", "");
    std::string msgId = j.value("msgId", "");

    json wsFrame;
    wsFrame["msgId"] = msgId;
    wsFrame["timestamp"] = j.value("timestamp", (int64_t)0);
    wsFrame["content"] = j.value("content", "");

    std::vector<int64_t> recipients;
    if (type == "private") {
        int64_t to = j.value("to", (int64_t)0);
        if (to == 0) return out;
        recipients.push_back(to);
        wsFrame["type"] = "msg";
        wsFrame["from"] = std::to_string(j.value("from", (int64_t)0));
        wsFrame["to"]   = std::to_string(to);
    } else {
        return out;  // 群消息暂不接此路径
    }

    for (int64_t uid : recipients) {
        json cmd;
        cmd["type"]      = "push";
        cmd["targetUid"] = std::to_string(uid);
        cmd["payload"]   = wsFrame;
        out.push_back({std::to_string(uid), cmd.dump()});
    }
    return out;
}

// ────────────── at-least-once 模式（默认） ──────────────
static int runAtLeastOnce(const std::string& brokers, const std::string& group) {
    auto producer = std::make_shared<KafkaProducer>(
        std::vector<std::string>{brokers}, "push-router");
    if (!producer->start()) {
        std::cerr << "[push-router] producer start fail\n";
        return 1;
    }

    KafkaConsumer consumer({brokers}, group, {"im.messages"});
    std::atomic<uint64_t> producedCmd{0}, badJson{0};

    bool ok = consumer.start(
        [&](const std::string&, int32_t, int64_t,
            const std::string&, const std::string& value) -> bool {
            for (auto& o : route(value, badJson)) {
                if (producer->produce("im.push.commands", o.key, o.value)) {
                    producedCmd.fetch_add(1);
                }
            }
            return true;
        });

    if (!ok) { std::cerr << "[push-router] consumer start fail\n"; return 1; }
    std::cerr << "[push-router] mode=at-least-once broker=" << brokers
              << " group=" << group << "\n";

    while (g_running.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::cerr << "[push-router] consumed=" << consumer.consumed()
                  << " produced_cmds=" << producedCmd.load()
                  << " bad_json=" << badJson.load()
                  << " kafka_dropped=" << producer->droppedCount() << "\n";
    }
    consumer.stop();
    producer->stop();
    return 0;
}

// ────────────── transactional EOS 模式（Phase 6.3） ──────────────
static int runTransactional(const std::string& brokers, const std::string& group,
                            const std::string& txId) {
    auto producer = std::make_shared<KafkaProducer>(
        std::vector<std::string>{brokers}, "push-router");
    producer->enableTransactions(txId, /*timeoutMs=*/10000);
    if (!producer->start()) {
        std::cerr << "[push-router] tx producer start fail\n";
        return 1;
    }

    KafkaConsumer consumer({brokers}, group, {"im.messages"});
    consumer.enableReadCommitted();
    if (!consumer.startManual()) {
        std::cerr << "[push-router] consumer startManual fail\n";
        return 1;
    }

    std::cerr << "[push-router] mode=transactional broker=" << brokers
              << " group=" << group << " tx_id=" << txId << "\n";

    std::atomic<uint64_t> processedTx{0}, abortedTx{0}, producedCmd{0}, badJson{0};
    auto lastReport = std::chrono::steady_clock::now();

    // 简单的 batch-by-batch loop：累 N 条 / 50ms 触发一次 commit
    constexpr int BATCH_MAX = 100;
    constexpr int BATCH_TIMEOUT_MS = 50;
    int inBatch = 0;
    bool inTx = false;

    auto endTx = [&](bool ok) {
        if (!inTx) return;
        if (ok) {
            // sendOffsetsToTransaction 必须在 commit 之前
            auto positions = consumer.currentPositions();
            std::unique_ptr<RdKafka::ConsumerGroupMetadata> meta(consumer.groupMetadata());
            if (!positions.empty() && meta) {
                producer->sendOffsetsToTransaction(positions, meta.get());
            }
            RdKafka::TopicPartition::destroy(positions);
            if (!producer->commitTransaction()) {
                producer->abortTransaction();
                abortedTx.fetch_add(1);
            } else {
                processedTx.fetch_add(1);
            }
        } else {
            producer->abortTransaction();
            abortedTx.fetch_add(1);
        }
        inTx = false;
        inBatch = 0;
    };

    while (g_running.load()) {
        if (!inTx) {
            if (!producer->beginTransaction()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            inTx = true;
            inBatch = 0;
        }

        auto msg = consumer.consumeOnce(BATCH_TIMEOUT_MS);
        if (!msg) {
            // timeout → 把当前 batch 关掉
            if (inBatch > 0) endTx(true);
            else { producer->abortTransaction(); inTx = false; }
            continue;
        }
        if (msg->err() == RdKafka::ERR__TIMED_OUT
            || msg->err() == RdKafka::ERR__PARTITION_EOF) {
            if (inBatch > 0) endTx(true);
            else { producer->abortTransaction(); inTx = false; }
            continue;
        }
        if (msg->err() != RdKafka::ERR_NO_ERROR) {
            std::cerr << "[push-router] consume err: " << msg->errstr() << "\n";
            endTx(false);
            continue;
        }

        std::string value(static_cast<const char*>(msg->payload()), msg->len());
        bool batchOk = true;
        for (auto& o : route(value, badJson)) {
            if (!producer->produce("im.push.commands", o.key, o.value)) {
                batchOk = false;
                break;
            }
            producedCmd.fetch_add(1);
        }
        if (!batchOk) {
            endTx(false);
            continue;
        }
        ++inBatch;
        if (inBatch >= BATCH_MAX) {
            endTx(true);
        }

        auto now = std::chrono::steady_clock::now();
        if (now - lastReport > std::chrono::seconds(5)) {
            lastReport = now;
            std::cerr << "[push-router] tx_committed=" << processedTx.load()
                      << " tx_aborted=" << abortedTx.load()
                      << " produced_cmds=" << producedCmd.load()
                      << " bad_json=" << badJson.load() << "\n";
        }
    }
    endTx(false);  // 退出前 abort 当前
    consumer.stop();
    producer->stop();
    return 0;
}

int main() {
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    std::string brokers = "localhost:9092";
    if (const char* e = std::getenv("MUDUO_IM_KAFKA_BROKERS")) brokers = e;
    std::string group = "push-router";
    if (const char* e = std::getenv("MUDUO_IM_GROUP")) group = e;

    bool tx = false;
    if (const char* e = std::getenv("MUDUO_IM_PUSH_ROUTER_TRANSACTIONAL")) {
        tx = (std::string(e) == "1" || std::string(e) == "true");
    }
    if (tx) {
        std::string txId = "push-router-1";
        if (const char* e = std::getenv("MUDUO_IM_PUSH_ROUTER_TX_ID")) txId = e;
        return runTransactional(brokers, group, txId);
    }
    return runAtLeastOnce(brokers, group);
}
