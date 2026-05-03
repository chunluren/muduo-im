/**
 * @file SagaCoordinator.h
 * @brief Phase 6.1 Saga 编排器 — 多步业务流的最终一致性
 *
 * 用法：
 *   1. 启动时 register saga 类型（forward + compensate 函数对）
 *   2. 业务代码调 start() 拿一个 saga_id
 *   3. 调 execute() 串行跑 forward；任何一步 false → 反向跑 compensation
 *   4. 进程崩溃后 recoverIncomplete() 扫 saga_log 续跑
 *
 * forward 步骤里对业务表的写**和 saga_log 行的 current_step 更新必须同事务**，
 * 这样进程崩溃后 saga_log 反映的就是真实进度（leverage Outbox 思路）。
 *
 * 设计取舍：
 *   - 串行执行（不并发跑步骤），简单可推理
 *   - 同步阻塞（在调用线程跑），不引入新线程；调用方自己切到 worker pool 即可
 *   - compensation 里 best-effort：失败只记 log，不再无限重试（防死循环）
 *   - 不引入 Seata 等外部协调器，纯 MySQL 驱动
 */
#pragma once

#include "common/Logging.h"
#include "pool/MySQLPool.h"
#include "util/Metrics.h"
#include "util/Snowflake.h"
#include <atomic>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

class SagaCoordinator {
public:
    /// 步骤上下文：跨步骤传递的"工作集"，每步可读写 ctx 字段
    struct Context {
        int64_t saga_id = 0;
        std::string saga_type;
        nlohmann::json payload;          // 用户业务数据（输入/输出共享）
        int current_step = 0;
        std::string last_error;
    };

    /// 单步函数签名：true = 成功，false = 失败（forward 失败触发 compensate）
    using StepFn = std::function<bool(MySQLConnection::Ptr&, Context&)>;

    struct Step {
        std::string name;
        StepFn forward;
        StepFn compensate;   // 可为空（不可补偿步骤；通常放最后）
    };

    struct Definition {
        std::string name;
        std::vector<Step> steps;
    };

    explicit SagaCoordinator(std::shared_ptr<MySQLPool> db) : db_(std::move(db)) {}

    /// 启动期：注册一个 saga 类型
    void registerSaga(Definition def) {
        std::lock_guard<std::mutex> lk(defMu_);
        defs_[def.name] = std::move(def);
    }

    /// 启动一个新 saga：在 saga_log 写一行 running，返回 saga_id
    /// 失败返回 0
    int64_t start(const std::string& sagaType, const nlohmann::json& payload) {
        Definition def;
        {
            std::lock_guard<std::mutex> lk(defMu_);
            auto it = defs_.find(sagaType);
            if (it == defs_.end()) {
                std::cerr << "[saga] unknown type: " << sagaType << "\n";
                return 0;
            }
            def = it->second;
        }
        int64_t sid = mymuduo::Snowflake::instance().nextId();
        auto conn = db_->acquireWrite(2000);
        if (!conn || !conn->valid()) return 0;
        std::string p = payload.dump();
        std::string sql =
            "INSERT INTO saga_log (saga_id, saga_type, state, current_step, payload) "
            "VALUES (" + std::to_string(sid) + ", '" + conn->escape(sagaType) +
            "', 'running', 0, '" + conn->escape(p) + "')";
        int rows = conn->execute(sql);
        db_->release(std::move(conn));
        if (rows < 1) {
            std::cerr << "[saga] start INSERT failed sid=" << sid << "\n";
            return 0;
        }
        Metrics::instance().increment("saga_started_total");
        return sid;
    }

    /// 串行跑 forward 步骤；某步失败 → 反向跑 compensation
    /// 返回 true = 全部 forward 成功；false = 部分回滚或彻底失败
    bool execute(int64_t sagaId) {
        Context ctx;
        Definition def;
        if (!loadContextAndDef(sagaId, ctx, def)) {
            std::cerr << "[saga] execute: cannot load sid=" << sagaId << "\n";
            return false;
        }

        // forward 阶段（从 ctx.current_step 续跑）
        bool forwardOk = true;
        for (size_t i = static_cast<size_t>(ctx.current_step); i < def.steps.size(); ++i) {
            auto& step = def.steps[i];
            ctx.current_step = static_cast<int>(i);
            std::cerr << "[saga] sid=" << sagaId << " forward[" << i << "] "
                      << step.name << "\n";

            // 每步在自己的事务里跑：业务表 INSERT/UPDATE + saga_log step++
            auto conn = db_->acquireWrite(3000);
            if (!conn || !conn->valid()) {
                ctx.last_error = "acquire conn failed";
                forwardOk = false;
                break;
            }
            TransactionGuard tx(conn);
            if (!tx.active()) {
                ctx.last_error = "begin tx failed";
                db_->release(std::move(conn));
                forwardOk = false;
                break;
            }
            bool stepOk = false;
            try {
                stepOk = step.forward(conn, ctx);
            } catch (const std::exception& e) {
                ctx.last_error = std::string("exception: ") + e.what();
            }
            if (!stepOk) {
                // 不 commit，让 RAII 回滚
                db_->release(std::move(conn));
                Metrics::instance().increment("saga_step_failed_total");
                forwardOk = false;
                break;
            }
            // 同事务把 saga_log 推进一步
            std::string p = ctx.payload.dump();
            std::string updSql =
                "UPDATE saga_log SET current_step = " + std::to_string(i + 1) +
                ", payload = '" + conn->escape(p) +
                "' WHERE saga_id = " + std::to_string(sagaId);
            int updRows = conn->execute(updSql);
            if (updRows < 1 || !tx.commit()) {
                ctx.last_error = "saga_log update or commit failed";
                db_->release(std::move(conn));
                forwardOk = false;
                break;
            }
            db_->release(std::move(conn));
        }

        if (forwardOk) {
            markDone(sagaId);
            Metrics::instance().increment("saga_done_total");
            return true;
        }

        // compensate 阶段（反向，从 current_step-1 退回到 0）
        std::cerr << "[saga] sid=" << sagaId << " forward failed at step "
                  << ctx.current_step << ": " << ctx.last_error << "\n";
        markCompensating(sagaId, ctx.last_error);
        for (int i = ctx.current_step - 1; i >= 0; --i) {
            auto& step = def.steps[static_cast<size_t>(i)];
            if (!step.compensate) {
                std::cerr << "[saga] sid=" << sagaId << " step[" << i << "] "
                          << step.name << " no compensate, skip\n";
                continue;
            }
            std::cerr << "[saga] sid=" << sagaId << " compensate[" << i << "] "
                      << step.name << "\n";
            auto conn = db_->acquireWrite(3000);
            if (!conn || !conn->valid()) {
                std::cerr << "[saga] sid=" << sagaId << " compensate conn failed\n";
                continue;
            }
            TransactionGuard tx(conn);
            bool ok = false;
            try { ok = step.compensate(conn, ctx); } catch (...) { ok = false; }
            if (ok) tx.commit();
            db_->release(std::move(conn));
            if (!ok) {
                std::cerr << "[saga] sid=" << sagaId << " compensate FAILED step "
                          << i << " (best-effort, continuing)\n";
                Metrics::instance().increment("saga_compensate_failed_total");
            }
        }
        markFailed(sagaId, ctx.last_error);
        Metrics::instance().increment("saga_failed_total");
        return false;
    }

    /// 启动期扫 saga_log 中 state IN ('running','compensating') 的 saga 续跑
    /// 一般在 ChatServer 启动后调一次；返回处理的 saga 数
    int recoverIncomplete() {
        auto conn = db_->acquireRead(3000);
        if (!conn || !conn->valid()) return 0;
        auto rs = conn->query(
            "SELECT saga_id, state, current_step "
            "  FROM saga_log "
            " WHERE state IN ('running', 'compensating') "
            " ORDER BY created_at ASC");
        std::vector<int64_t> ids;
        if (rs) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(rs.get()))) {
                if (row[0]) {
                    int64_t sid = std::stoll(row[0]);
                    std::cerr << "[saga] recover sid=" << sid
                              << " state=" << (row[1] ? row[1] : "?")
                              << " step=" << (row[2] ? row[2] : "?") << "\n";
                    ids.push_back(sid);
                }
            }
        }
        db_->release(std::move(conn));

        for (int64_t sid : ids) execute(sid);
        if (!ids.empty()) {
            Metrics::instance().gauge("saga_recovered_total",
                                      static_cast<int64_t>(ids.size()));
        }
        return static_cast<int>(ids.size());
    }

private:
    bool loadContextAndDef(int64_t sagaId, Context& ctx, Definition& outDef) {
        auto conn = db_->acquireRead(3000);
        if (!conn || !conn->valid()) return false;
        auto rs = conn->query(
            "SELECT saga_type, current_step, payload FROM saga_log WHERE saga_id = "
            + std::to_string(sagaId));
        bool found = false;
        if (rs) {
            MYSQL_ROW row = mysql_fetch_row(rs.get());
            if (row && row[0]) {
                ctx.saga_id = sagaId;
                ctx.saga_type = row[0];
                ctx.current_step = row[1] ? std::stoi(row[1]) : 0;
                if (row[2]) {
                    try { ctx.payload = nlohmann::json::parse(row[2]); }
                    catch (...) { ctx.payload = nlohmann::json::object(); }
                } else {
                    ctx.payload = nlohmann::json::object();
                }
                found = true;
            }
        }
        db_->release(std::move(conn));
        if (!found) return false;

        std::lock_guard<std::mutex> lk(defMu_);
        auto it = defs_.find(ctx.saga_type);
        if (it == defs_.end()) return false;
        outDef = it->second;
        return true;
    }

    void markDone(int64_t sagaId) {
        auto conn = db_->acquireWrite(2000);
        if (!conn) return;
        conn->execute("UPDATE saga_log SET state='done' WHERE saga_id = "
                      + std::to_string(sagaId));
        db_->release(std::move(conn));
    }
    void markCompensating(int64_t sagaId, const std::string& err) {
        auto conn = db_->acquireWrite(2000);
        if (!conn) return;
        conn->execute(
            "UPDATE saga_log SET state='compensating', error_msg='"
            + conn->escape(err) + "' WHERE saga_id = " + std::to_string(sagaId));
        db_->release(std::move(conn));
    }
    void markFailed(int64_t sagaId, const std::string& err) {
        auto conn = db_->acquireWrite(2000);
        if (!conn) return;
        conn->execute(
            "UPDATE saga_log SET state='failed', error_msg='"
            + conn->escape(err) + "' WHERE saga_id = " + std::to_string(sagaId));
        db_->release(std::move(conn));
    }

    std::shared_ptr<MySQLPool> db_;
    std::mutex defMu_;
    std::map<std::string, Definition> defs_;
};
