/**
 * @file push_router/main.cpp
 * @brief muduo-im-push-router — Kafka 消息推送路由器（Phase 1.4）
 *
 * 角色：消费 im.messages topic，把每条消息**重新打包成 ws 协议帧**，
 *       produce 到 im.push.commands（key = 收件人 uid）。所有 gateway /
 *       ChatServer 实例订阅 im.push.commands，只对本地有 session 的 uid
 *       做投递，没有就跳过。这条路径替代以前的 InstanceRouter Redis Pub/Sub。
 *
 * 当前阶段（与 ChatServer 共存，不淘汰 InstanceRouter）：
 *   - push-router 只产出 push.commands，不改变 ChatServer 已有的内联 broadcast
 *   - 这阶段 push.commands 主要用来**验证数据正确性**（kafka-console-consumer 看）
 *   - Phase 1.5 起 ChatServer 引入 push.commands 订阅，并在配置开关下停掉
 *     InstanceRouter，完成切换
 *
 * 启动:
 *   MUDUO_IM_KAFKA_BROKERS=localhost:9092 \
 *   MUDUO_IM_GROUP=push-router \
 *   ./muduo-im-push-router
 */
#include "util/KafkaConsumer.h"
#include "util/KafkaProducer.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <signal.h>

using json = nlohmann::json;

static std::atomic<bool> g_running{true};
static void onSignal(int sig) {
    std::cerr << "[push-router] signal " << sig << ", stopping\n";
    g_running.store(false);
}

int main() {
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    std::string brokers = "localhost:9092";
    if (const char* e = std::getenv("MUDUO_IM_KAFKA_BROKERS")) brokers = e;
    std::string group = "push-router";
    if (const char* e = std::getenv("MUDUO_IM_GROUP")) group = e;

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
            const std::string& /*key*/, const std::string& value) -> bool {
            json j = json::parse(value, nullptr, false);
            if (j.is_discarded()) {
                badJson.fetch_add(1);
                return true;  // commit 跳过坏 JSON
            }
            std::string type = j.value("type", "");
            std::string msgId = j.value("msgId", "");

            // 决定推送给谁 + 构造 ws 帧
            std::vector<int64_t> recipients;
            json wsFrame;
            wsFrame["msgId"] = msgId;
            wsFrame["timestamp"] = j.value("timestamp", (int64_t)0);
            wsFrame["content"] = j.value("content", "");

            if (type == "private") {
                int64_t to = j.value("to", (int64_t)0);
                if (to == 0) return true;
                recipients.push_back(to);
                wsFrame["type"] = "msg";  // 与 ChatServer Protocol::MSG 对齐
                wsFrame["from"] = std::to_string(j.value("from", (int64_t)0));
                wsFrame["to"]   = std::to_string(to);
            } else if (type == "group") {
                // 群消息：没有显式 recipient 列表，下游 gateway 按本地 sessions
                // 决定。这里 push-router 只负责"广播给可能在线的成员"——简化做法
                // 用 group_id 作 key，所有 gateway 都拉到这条 cmd，自己看本地
                // 哪些 session 是这个群的成员（需要 gateway 维护群成员缓存）。
                // 当前先不接群推路径，跳过。
                return true;
            } else {
                return true;  // unknown type，commit 跳过
            }

            // 给每个 recipient produce 一条 push command
            for (int64_t uid : recipients) {
                json cmd;
                cmd["type"]     = "push";
                cmd["targetUid"]= std::to_string(uid);
                cmd["payload"]  = wsFrame;
                std::string key = std::to_string(uid);
                bool queued = producer->produce("im.push.commands", key,
                                                  cmd.dump());
                if (queued) producedCmd.fetch_add(1);
            }
            return true;
        });

    if (!ok) {
        std::cerr << "[push-router] consumer start fail\n";
        return 1;
    }
    std::cerr << "[push-router] running, broker=" << brokers
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
    std::cerr << "[push-router] stopped\n";
    return 0;
}
