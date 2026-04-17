#pragma once

#include "pool/MySQLPool.h"
#include <memory>
#include <iostream>
#include <cassert>

#define ASSERT_TRUE(x) do { if (!(x)) { std::cerr << "ASSERT FAILED: " #x " at " << __FILE__ << ":" << __LINE__ << std::endl; assert(false); } } while(0)
#define ASSERT_EQ(a, b) do { if (!((a) == (b))) { std::cerr << "ASSERT_EQ FAILED: " << (a) << " != " << (b) << " at " << __FILE__ << ":" << __LINE__ << std::endl; assert(false); } } while(0)

inline std::shared_ptr<MySQLPool> getTestDb() {
    MySQLPoolConfig cfg;
    cfg.host = "127.0.0.1";
    cfg.port = 3306;
    cfg.user = "root";
    cfg.password = "";
    cfg.database = "muduo_im_test";
    cfg.minSize = 2;
    cfg.maxSize = 5;
    return std::make_shared<MySQLPool>(cfg);
}

/// 清理所有测试表（保留 schema）
inline void cleanTestDb() {
    auto db = getTestDb();
    auto conn = db->acquire(2000);
    if (!conn || !conn->valid()) return;
    conn->execute("SET FOREIGN_KEY_CHECKS=0");
    conn->execute("TRUNCATE TABLE friend_requests");
    conn->execute("TRUNCATE TABLE friends");
    conn->execute("TRUNCATE TABLE group_members");
    conn->execute("TRUNCATE TABLE `groups`");
    conn->execute("TRUNCATE TABLE private_messages");
    conn->execute("TRUNCATE TABLE group_messages");
    conn->execute("TRUNCATE TABLE users");
    conn->execute("SET FOREIGN_KEY_CHECKS=1");
    db->release(std::move(conn));
}
