/**
 * @file test_saga_coordinator.cpp
 * @brief Phase 6.1 Saga 编排器 — 单元 + 端到端
 *
 * 前置：MySQL @ 127.0.0.1:3306, db=muduo_im，saga_log 表已建（sql/init.sql）。
 * 跳过：SKIP_SAGA_TEST=1
 *
 * 用一个合成 saga：
 *   step1 INSERT saga_test_table (k=ctx.k1, v='a')
 *   step2 INSERT saga_test_table (k=ctx.k2, v='b')
 *   step3 INSERT saga_test_table (k=ctx.k3, v='c')   // optional fail
 *
 * 各 case：
 *   1. 全 forward 成功 → 表里 3 行 + saga state=done
 *   2. step3 主动返回 false → step1+step2 被 compensate（DELETE）→ 0 行 + state=failed
 *   3. recoverIncomplete：先 INSERT 一个 state='running' current_step=1 的假 saga
 *      → 调 recover → 从 step1 续跑 → 全部完成 + state=done
 */
#include "server/SagaCoordinator.h"
#include "pool/MySQLPool.h"
#include "util/Snowflake.h"
#include <cassert>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

#define TEST(name) static void name()
#define RUN_TEST(name) do { std::cerr << "[run] " #name "\n"; name(); std::cerr << "[ok]  " #name "\n"; } while (0)

static std::shared_ptr<MySQLPool> mkPool() {
    MySQLPoolConfig cfg;
    cfg.host = "127.0.0.1"; cfg.port = 3306;
    cfg.user = "root"; cfg.password = "";
    cfg.database = "muduo_im";
    cfg.minSize = 2; cfg.maxSize = 5;
    return std::make_shared<MySQLPool>(cfg);
}

static void ensureTestTable(MySQLPool& db) {
    auto c = db.acquireWrite();
    c->execute("CREATE TABLE IF NOT EXISTS saga_test_table ("
               "k VARCHAR(64) PRIMARY KEY, v VARCHAR(64) NOT NULL, "
               "created_at DATETIME(3) DEFAULT CURRENT_TIMESTAMP(3)"
               ") ENGINE=InnoDB");
    c->execute("DELETE FROM saga_test_table WHERE k LIKE 'saga-%'");
    c->execute("DELETE FROM saga_log WHERE saga_type LIKE 'saga_test_%'");
    db.release(std::move(c));
}

static int countRows(MySQLPool& db, const std::string& sagaId) {
    auto c = db.acquireRead();
    auto rs = c->query("SELECT COUNT(*) FROM saga_test_table WHERE k LIKE 'saga-"
                       + sagaId + "-%'");
    int n = 0;
    if (rs) {
        MYSQL_ROW row = mysql_fetch_row(rs.get());
        if (row && row[0]) n = std::atoi(row[0]);
    }
    db.release(std::move(c));
    return n;
}

static std::string sagaState(MySQLPool& db, int64_t sid) {
    auto c = db.acquireRead();
    auto rs = c->query("SELECT state FROM saga_log WHERE saga_id = "
                       + std::to_string(sid));
    std::string s;
    if (rs) {
        MYSQL_ROW row = mysql_fetch_row(rs.get());
        if (row && row[0]) s = row[0];
    }
    db.release(std::move(c));
    return s;
}

// 共用 saga 定义工厂：可控 step3 的 forward 是 OK 还是 FAIL
static SagaCoordinator::Definition makeTestSaga(bool step3WillFail) {
    SagaCoordinator::Definition def;
    def.name = "saga_test_basic";
    auto mkInsert = [](int idx) {
        return [idx](MySQLConnection::Ptr& c, SagaCoordinator::Context& ctx) -> bool {
            std::string k = "saga-" + std::to_string(ctx.saga_id) + "-" + std::to_string(idx);
            std::string v = "v" + std::to_string(idx);
            std::string sql = "INSERT INTO saga_test_table (k, v) VALUES ('"
                + c->escape(k) + "', '" + c->escape(v) + "')";
            return c->execute(sql) == 1;
        };
    };
    auto mkDelete = [](int idx) {
        return [idx](MySQLConnection::Ptr& c, SagaCoordinator::Context& ctx) -> bool {
            std::string k = "saga-" + std::to_string(ctx.saga_id) + "-" + std::to_string(idx);
            std::string sql = "DELETE FROM saga_test_table WHERE k = '"
                + c->escape(k) + "'";
            return c->execute(sql) >= 0;  // 删 0 行也算成功（compensate 幂等）
        };
    };
    def.steps.push_back({"step1_insert", mkInsert(1), mkDelete(1)});
    def.steps.push_back({"step2_insert", mkInsert(2), mkDelete(2)});
    if (step3WillFail) {
        def.steps.push_back({
            "step3_fail",
            [](MySQLConnection::Ptr&, SagaCoordinator::Context& ctx) {
                ctx.last_error = "intentional fail";
                return false;
            },
            nullptr   // 不可补偿（也用不上）
        });
    } else {
        def.steps.push_back({"step3_insert", mkInsert(3), mkDelete(3)});
    }
    return def;
}

static std::shared_ptr<MySQLPool> g_db;

TEST(saga_all_forward_success) {
    SagaCoordinator coord(g_db);
    coord.registerSaga(makeTestSaga(false));
    int64_t sid = coord.start("saga_test_basic", {{"k1", "test1"}});
    assert(sid > 0);
    bool ok = coord.execute(sid);
    assert(ok);
    assert(countRows(*g_db, std::to_string(sid)) == 3);
    assert(sagaState(*g_db, sid) == "done");
}

TEST(saga_compensate_on_step3_failure) {
    SagaCoordinator coord(g_db);
    coord.registerSaga(makeTestSaga(true));
    int64_t sid = coord.start("saga_test_basic", {{"k1", "test2"}});
    assert(sid > 0);
    bool ok = coord.execute(sid);
    assert(!ok);
    // step3 失败 → step1+step2 compensate → 0 行
    assert(countRows(*g_db, std::to_string(sid)) == 0);
    assert(sagaState(*g_db, sid) == "failed");
}

TEST(saga_recover_resumes_at_current_step) {
    SagaCoordinator coord(g_db);
    coord.registerSaga(makeTestSaga(false));

    // 模拟"step1 已 commit、进程在 step2 之前崩溃"的场景：
    //   手动 INSERT saga_log + saga_test_table（step1 的产物）
    int64_t sid = mymuduo::Snowflake::instance().nextId();
    auto c = g_db->acquireWrite();
    nlohmann::json p = {{"resume", "true"}};
    c->execute("INSERT INTO saga_log (saga_id, saga_type, state, current_step, payload) "
               "VALUES (" + std::to_string(sid) + ", 'saga_test_basic', "
               "'running', 1, '" + c->escape(p.dump()) + "')");
    c->execute("INSERT INTO saga_test_table (k, v) VALUES ('saga-"
               + std::to_string(sid) + "-1', 'v1')");
    g_db->release(std::move(c));

    // 现在调 recover —— 应该从 step2 开始
    int handled = coord.recoverIncomplete();
    assert(handled >= 1);
    assert(countRows(*g_db, std::to_string(sid)) == 3);
    assert(sagaState(*g_db, sid) == "done");
}

int main() {
    if (std::getenv("SKIP_SAGA_TEST")) { std::cerr << "SKIP\n"; return 0; }
    try { mymuduo::Snowflake::instance().initFromEnv("MUDUO_IM_WORKER_ID"); } catch (...) {}
    g_db = mkPool();
    ensureTestTable(*g_db);

    RUN_TEST(saga_all_forward_success);
    RUN_TEST(saga_compensate_on_step3_failure);
    RUN_TEST(saga_recover_resumes_at_current_step);
    std::cerr << "ALL OK\n";
    return 0;
}
