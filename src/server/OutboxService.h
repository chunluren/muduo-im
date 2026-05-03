/**
 * @file OutboxService.h
 * @brief Phase 1.2 Transactional Outbox：解决"DB 写 + Kafka 发"双写不一致
 *
 * 关键不变量：
 *   - 业务表 INSERT + outbox INSERT **必须在同一个 MySQL 事务里**
 *   - 业务事务 COMMIT 之后，消息一定能被异步 relay 出去（Kafka 重试 + 死信）
 *   - 业务事务 ROLLBACK 之后，outbox 行也回滚 → 永远不会"业务失败但消息发了"
 *
 * 用法：
 * @code
 *   auto conn = mysqlPool->acquire(...);
 *   TransactionGuard tx(conn);
 *   messageService_.savePrivateMessageTx(conn, ...);
 *   outboxService_.insertOutbox(conn, "im.messages", convKey, payloadJson);
 *   tx.commit();
 *
 *   // OutboxRelay 后台线程会拉 pending → produce Kafka → 标 sent
 * @endcode
 *
 * Relay 周期：默认 200ms，批量 100 行。生产场景按 lag 监控调。
 */
#pragma once

#include "pool/MySQLPool.h"
#include "util/KafkaProducer.h"
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class OutboxService {
public:
    OutboxService(std::shared_ptr<MySQLPool> db,
                  std::shared_ptr<KafkaProducer> kafka)
        : db_(std::move(db)), kafka_(std::move(kafka)) {}

    ~OutboxService() { stop(); }

    /**
     * @brief 在已有事务里插一行 outbox（**调用方负责事务边界**）
     *
     * @param conn   持有事务的连接（TransactionGuard 已 begin）
     * @param topic  Kafka topic
     * @param key    Kafka partition key（保序）
     * @param payload  完整业务消息 JSON
     * @return true 插入成功
     */
    bool insertOutbox(MySQLConnection::Ptr& conn,
                      const std::string& topic,
                      const std::string& key,
                      const std::string& payload) {
        if (!conn || !conn->valid()) return false;
        PreparedStatement stmt(conn,
            "INSERT INTO outbox (topic, msg_key, payload) VALUES (?, ?, ?)");
        if (!stmt.valid()) return false;
        stmt.bindString(1, topic);
        stmt.bindString(2, key);
        stmt.bindString(3, payload);
        return stmt.execute() && stmt.affectedRows() > 0;
    }

    /// 启动 relay 后台线程
    void start(int intervalMs = 200, int batchSize = 100) {
        if (running_.exchange(true)) return;
        intervalMs_ = intervalMs;
        batchSize_  = batchSize;
        thread_ = std::thread([this]() { relayLoop(); });
        std::cerr << "[outbox] relay started interval=" << intervalMs_
                  << "ms batch=" << batchSize_ << "\n";
    }

    void stop() {
        if (!running_.exchange(false)) return;
        if (thread_.joinable()) thread_.join();
    }

    // 监控用：累计计数
    uint64_t relayed()  const { return relayed_.load(); }
    uint64_t failed()   const { return failed_.load(); }
    uint64_t lastBatchSize() const { return lastBatchSize_.load(); }

private:
    void relayLoop() {
        while (running_.load()) {
            int n = drainOnce();
            lastBatchSize_.store(n);
            // 没拉到东西 → 睡 intervalMs；有的话立即下一轮（peak 期跟得上）
            if (n == 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs_));
            }
        }
    }

    /// 拉一批 pending → produce Kafka → 标 sent。返回本批处理行数（含失败）
    int drainOnce() {
        auto conn = db_->acquire(2000);
        if (!conn || !conn->valid()) return 0;

        // 1) 拉 pending（按 id 升序，保证 partition 内顺序）
        std::string sql = "SELECT id, topic, msg_key, payload FROM outbox "
                          "WHERE status='pending' ORDER BY id ASC LIMIT "
                          + std::to_string(batchSize_);
        auto result = conn->query(sql);
        if (!result || mysql_num_rows(result.get()) == 0) {
            db_->release(std::move(conn));
            return 0;
        }

        int processed = 0;
        std::vector<int64_t> sentIds;
        std::vector<std::pair<int64_t, std::string>> failedIds;
        sentIds.reserve(batchSize_);

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result.get()))) {
            int64_t id     = row[0] ? std::stoll(row[0]) : 0;
            std::string topic   = row[1] ? row[1] : "";
            std::string key     = row[2] ? row[2] : "";
            std::string payload = row[3] ? row[3] : "";
            ++processed;

            // 同步等 Kafka delivery 太慢；这里用异步 produce + 在 dr_cb 内更新 DB
            // 简化：每行用一个 promise/future 同步等
            // 但为吞吐：批量 produce，结束后用单独 UPDATE 标 sent
            auto* ctx = new RelayCtx{id, this};
            bool queued = kafka_->produce(topic, key, payload,
                [ctx](bool ok, const std::string& err) {
                    ctx->self->onDelivery(ctx->outboxId, ok, err);
                    delete ctx;
                });
            if (!queued) {
                // 入队失败 → 当作此次失败，下轮重试
                delete ctx;
                failedIds.emplace_back(id, "produce queue full");
            }
        }
        result.reset();

        // 不在这里直接 UPDATE — 让 dr_cb 异步走
        db_->release(std::move(conn));
        return processed;
    }

    struct RelayCtx {
        int64_t outboxId;
        OutboxService* self;
    };

    void onDelivery(int64_t id, bool ok, const std::string& err) {
        auto conn = db_->acquire(1500);
        if (!conn || !conn->valid()) {
            // 实际落库失败 → 下次 relay 还会拉到（pending 仍未变）
            return;
        }
        if (ok) {
            PreparedStatement stmt(conn,
                "UPDATE outbox SET status='sent', sent_at=NOW(3) WHERE id=?");
            if (stmt.valid()) {
                stmt.bindInt64(1, id);
                stmt.execute();
            }
            relayed_.fetch_add(1);
        } else {
            PreparedStatement stmt(conn,
                "UPDATE outbox SET retry_count=retry_count+1, last_error=? WHERE id=?");
            if (stmt.valid()) {
                stmt.bindString(1, err.substr(0, 500));
                stmt.bindInt64(2, id);
                stmt.execute();
            }
            failed_.fetch_add(1);
        }
        db_->release(std::move(conn));
    }

    std::shared_ptr<MySQLPool> db_;
    std::shared_ptr<KafkaProducer> kafka_;
    std::atomic<bool> running_{false};
    std::thread thread_;
    int intervalMs_ = 200;
    int batchSize_  = 100;
    std::atomic<uint64_t> relayed_{0};
    std::atomic<uint64_t> failed_{0};
    std::atomic<uint64_t> lastBatchSize_{0};
};
