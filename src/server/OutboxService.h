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
        // 先回收上次崩溃留下的 'sending' 卡死行
        reclaimStuckSending(60);
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

    /// 拉一批 pending → 抢占式标 'sending' → produce Kafka → 标 sent
    /// 不再拿同一行重复投递（关键修复：以前异步 dr_cb 没回来时下一轮 drainOnce
    /// 会再读到 status='pending' 的同一行，导致重复 produce）
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

        struct Row { int64_t id; std::string topic, key, payload; };
        std::vector<Row> rows;
        rows.reserve(batchSize_);
        MYSQL_ROW r;
        while ((r = mysql_fetch_row(result.get()))) {
            rows.push_back({r[0] ? std::stoll(r[0]) : 0,
                            r[1] ? r[1] : "",
                            r[2] ? r[2] : "",
                            r[3] ? r[3] : ""});
        }
        result.reset();

        // 2) 抢占式标 sending（CAS：仅当当前 status='pending' 才改）。
        //    如果其它 relay 实例同时跑，就会有竞争——只有 affectedRows=1 的行
        //    本进程才负责投递，避免双倍投递。本项目目前单 relay，影响小，但语
        //    义保持正确。
        std::vector<Row> claimed;
        claimed.reserve(rows.size());
        for (auto& row : rows) {
            PreparedStatement upd(conn,
                "UPDATE outbox SET status='sending' "
                "WHERE id=? AND status='pending'");
            if (!upd.valid()) continue;
            upd.bindInt64(1, row.id);
            if (upd.execute() && upd.affectedRows() == 1) {
                claimed.push_back(row);
            }
        }
        db_->release(std::move(conn));

        // 3) 异步 produce 已抢占的行
        for (auto& row : claimed) {
            auto* ctx = new RelayCtx{row.id, this};
            bool queued = kafka_->produce(row.topic, row.key, row.payload,
                [ctx](bool ok, const std::string& err) {
                    ctx->self->onDelivery(ctx->outboxId, ok, err);
                    delete ctx;
                });
            if (!queued) {
                delete ctx;
                // 入队失败 → 立刻把状态打回 pending，下轮重试
                onDelivery(row.id, false, "local queue full");
            }
        }
        return static_cast<int>(claimed.size());
    }

    struct RelayCtx {
        int64_t outboxId;
        OutboxService* self;
    };

    void onDelivery(int64_t id, bool ok, const std::string& err) {
        auto conn = db_->acquire(1500);
        if (!conn || !conn->valid()) {
            // 极端情况：连不上 DB。'sending' 行会卡着 → 启动时由 reclaimStuck 回收
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
            // 失败 → 状态回 pending，下次 drainOnce 重新抢占重试
            PreparedStatement stmt(conn,
                "UPDATE outbox SET status='pending', "
                "retry_count=retry_count+1, last_error=? WHERE id=?");
            if (stmt.valid()) {
                stmt.bindString(1, err.substr(0, 500));
                stmt.bindInt64(2, id);
                stmt.execute();
            }
            failed_.fetch_add(1);
        }
        db_->release(std::move(conn));
    }

    /// 启动时回收"sending"卡死的行（前次进程崩溃 / OOM 留下的）。
    /// 调用时机：start() 第一次 drainOnce 之前。把 status='sending' 且
    /// 时间超过阈值的打回 'pending'。
    void reclaimStuckSending(int olderThanSec = 60) {
        auto conn = db_->acquire(1500);
        if (!conn || !conn->valid()) return;
        PreparedStatement stmt(conn,
            "UPDATE outbox SET status='pending' "
            "WHERE status='sending' AND created_at < NOW(3) - INTERVAL ? SECOND");
        if (stmt.valid()) {
            stmt.bindInt64(1, olderThanSec);
            stmt.execute();
            if (stmt.affectedRows() > 0) {
                std::cerr << "[outbox] reclaimed " << stmt.affectedRows()
                          << " rows stuck in 'sending'\n";
            }
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
