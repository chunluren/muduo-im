# ACID 与持久性改进 实施计划

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 为 muduo-im 补全事务、外键约束、Redis 持久化三块缺失的 ACID 保证。

**Architecture:** 在 MySQLConnection 上加事务辅助方法；Service 层对多表操作用事务包装；init.sql 加外键 + 级联删除；部署文档加 Redis AOF 配置说明。

**Tech Stack:** MySQL InnoDB、Redis AOF、C++17 RAII

**影响范围:** muduo-im 服务端 4 个 Service、init.sql、部署文档、Redis 配置

---

## Task 1: MySQLConnection 增加事务 RAII 辅助类

**Files:**
- Modify: `third_party/mymuduo-http/src/pool/MySQLPool.h` (add TransactionGuard)
- Test: `third_party/mymuduo-http/tests/test_mysql_pool.cpp` (add compile-time check)

### Step 1: 在 MySQLConnection 里加事务方法

在 `MySQLConnection` 类的 public 方法区域（`escape()` 之后、`raw()` 之前），添加：

```cpp
    /// 开始事务
    bool beginTransaction() {
        if (!conn_) return false;
        return mysql_query(conn_, "BEGIN") == 0;
    }

    /// 提交事务
    bool commit() {
        if (!conn_) return false;
        return mysql_query(conn_, "COMMIT") == 0;
    }

    /// 回滚事务
    bool rollback() {
        if (!conn_) return false;
        return mysql_query(conn_, "ROLLBACK") == 0;
    }
```

### Step 2: 在 MySQLConnection 类下方新增 TransactionGuard 类

RAII 风格，出错自动回滚：

```cpp
/**
 * @class TransactionGuard
 * @brief RAII 事务守卫 — 析构时若未 commit 则自动 rollback
 *
 * 使用示例：
 *   {
 *       TransactionGuard tx(conn);
 *       conn->execute("UPDATE ...");
 *       conn->execute("INSERT ...");
 *       tx.commit();  // 成功则提交
 *   }  // 若抛异常或没 commit，析构时自动回滚
 */
class TransactionGuard {
public:
    explicit TransactionGuard(MySQLConnection::Ptr conn)
        : conn_(conn), committed_(false), began_(false)
    {
        if (conn_ && conn_->valid()) {
            began_ = conn_->beginTransaction();
        }
    }

    ~TransactionGuard() {
        if (began_ && !committed_ && conn_ && conn_->valid()) {
            conn_->rollback();
        }
    }

    TransactionGuard(const TransactionGuard&) = delete;
    TransactionGuard& operator=(const TransactionGuard&) = delete;

    /// 显式提交（成功返回 true）
    bool commit() {
        if (!began_ || committed_ || !conn_) return false;
        committed_ = conn_->commit();
        return committed_;
    }

    /// 事务是否成功开启
    bool active() const { return began_; }

private:
    MySQLConnection::Ptr conn_;
    bool committed_;
    bool began_;
};
```

### Step 3: 编译验证

```bash
cd /home/ly/workspaces/im/muduo-im/build && make muduo-im -j$(nproc) 2>&1 | tail -3
```

Expected: `[100%] Built target muduo-im`

### Step 4: Commit

```bash
cd /home/ly/workspaces/im/mymuduo-http && git add src/pool/MySQLPool.h && \
  git commit -m "feat: add TransactionGuard RAII helper to MySQLPool"
```

---

## Task 2: FriendService::handleRequest 使用事务

**Files:**
- Modify: `src/server/FriendService.h` line 165-195

### Step 1: 用事务包装 accept 分支的 3 条 SQL

替换现有的 `handleRequest` 方法：

```cpp
    json handleRequest(int64_t userId, int64_t requestId, bool accept) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {{"success", false}, {"message", "db error"}};

        // 查找申请
        auto result = conn->query("SELECT from_user, to_user FROM friend_requests WHERE id="
            + std::to_string(requestId) + " AND to_user=" + std::to_string(userId) + " AND status=0");
        if (!result || mysql_num_rows(result.get()) == 0) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "request not found"}};
        }

        MYSQL_ROW row = mysql_fetch_row(result.get());
        int64_t fromUser = std::stoll(row[0]);
        int64_t toUser = std::stoll(row[1]);

        bool success = false;
        if (accept) {
            // 事务：更新状态 + 双向添加好友（原子操作）
            TransactionGuard tx(conn);
            if (!tx.active()) {
                db_->release(std::move(conn));
                return {{"success", false}, {"message", "failed to start transaction"}};
            }

            int r1 = conn->execute("UPDATE friend_requests SET status=1 WHERE id=" + std::to_string(requestId));
            int r2 = conn->execute("INSERT IGNORE INTO friends (user_id, friend_id) VALUES ("
                + std::to_string(fromUser) + "," + std::to_string(toUser) + ")");
            int r3 = conn->execute("INSERT IGNORE INTO friends (user_id, friend_id) VALUES ("
                + std::to_string(toUser) + "," + std::to_string(fromUser) + ")");

            if (r1 >= 0 && r2 >= 0 && r3 >= 0) {
                success = tx.commit();
            }
            // 若未 commit，tx 析构时自动 rollback
        } else {
            // 拒绝：单条更新，无需事务
            int r = conn->execute("UPDATE friend_requests SET status=2 WHERE id=" + std::to_string(requestId));
            success = (r > 0);
        }

        db_->release(std::move(conn));
        if (!success) return {{"success", false}, {"message", "operation failed"}};
        return {{"success", true}, {"accepted", accept}};
    }
```

### Step 2: 编译

```bash
cd /home/ly/workspaces/im/muduo-im/build && make muduo-im -j$(nproc) 2>&1 | grep -E "error:" | head -3
```

Expected: 无 error

### Step 3: 跑 E2E 测试验证功能未破坏

```bash
bash stop.sh 2>/dev/null; bash start.sh
sleep 2
bash tests/e2e_test.sh 2>&1 | tail -5
bash stop.sh
```

Expected: `结果: 21 通过, 0 失败`

### Step 4: Commit

```bash
git add src/server/FriendService.h && \
  git commit -m "feat: wrap friend request acceptance in transaction"
```

---

## Task 3: GroupService::deleteGroup 使用事务

**Files:**
- Modify: `src/server/GroupService.h` (找到 `deleteGroup` 方法)

### Step 1: 事务包装 2 条 DELETE

找到 `deleteGroup` 方法，替换成：

```cpp
    json deleteGroup(int64_t userId, int64_t groupId) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {{"success", false}, {"message", "db error"}};

        // 验证群主
        auto result = conn->query("SELECT owner_id FROM `groups` WHERE id=" + std::to_string(groupId));
        if (!result || mysql_num_rows(result.get()) == 0) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "group not found"}};
        }
        MYSQL_ROW row = mysql_fetch_row(result.get());
        if (std::stoll(row[0]) != userId) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "only owner can delete group"}};
        }

        // 事务：删成员 + 删群组
        TransactionGuard tx(conn);
        if (!tx.active()) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "failed to start transaction"}};
        }

        int r1 = conn->execute("DELETE FROM group_members WHERE group_id=" + std::to_string(groupId));
        int r2 = conn->execute("DELETE FROM `groups` WHERE id=" + std::to_string(groupId));

        bool success = false;
        if (r1 >= 0 && r2 >= 0) success = tx.commit();

        db_->release(std::move(conn));
        if (!success) return {{"success", false}, {"message", "delete failed"}};
        return {{"success", true}};
    }
```

### Step 2: 编译 + commit

```bash
cd /home/ly/workspaces/im/muduo-im/build && make muduo-im -j$(nproc) 2>&1 | grep "error:" | head -3
cd /home/ly/workspaces/im/muduo-im && git add src/server/GroupService.h && \
  git commit -m "feat: wrap group deletion in transaction"
```

---

## Task 4: UserService::deleteAccount 使用事务

**Files:**
- Modify: `src/server/UserService.h` (找到 `deleteAccount` 方法)

### Step 1: 事务包装 4 条 DELETE

找到 `deleteAccount` 方法。在验证密码通过之后的那段 DELETE，用事务包装：

```cpp
        // 事务：级联删除用户相关数据
        TransactionGuard tx(conn);
        if (!tx.active()) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "failed to start transaction"}};
        }

        std::string uid = std::to_string(userId);
        int r1 = conn->execute("DELETE FROM friends WHERE user_id=" + uid + " OR friend_id=" + uid);
        int r2 = conn->execute("DELETE FROM group_members WHERE user_id=" + uid);
        int r3 = conn->execute("DELETE FROM friend_requests WHERE from_user=" + uid + " OR to_user=" + uid);
        int r4 = conn->execute("DELETE FROM users WHERE id=" + uid);

        bool success = false;
        if (r1 >= 0 && r2 >= 0 && r3 >= 0 && r4 >= 0) {
            success = tx.commit();
        }

        db_->release(std::move(conn));
        if (!success) return {{"success", false}, {"message", "delete failed"}};
        return {{"success", true}};
```

### Step 2: 编译 + commit

```bash
cd /home/ly/workspaces/im/muduo-im/build && make muduo-im -j$(nproc) 2>&1 | grep "error:" | head -3
cd /home/ly/workspaces/im/muduo-im && git add src/server/UserService.h && \
  git commit -m "feat: wrap account deletion in transaction"
```

---

## Task 5: FriendService::addFriend 使用事务（兼容老接口）

**Files:**
- Modify: `src/server/FriendService.h` (`addFriend` 方法)

虽然 `/api/friends/add` 已从 ChatServer 移除，但 `addFriend` 还被 `handleRequest` 间接用到（现已改事务），但老方法本身仍建议加事务。

### Step 1: 替换 addFriend

```cpp
    json addFriend(int64_t userId, int64_t friendId) {
        if (userId == friendId) return {{"success", false}, {"message", "cannot add self"}};

        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {{"success", false}, {"message", "db error"}};

        TransactionGuard tx(conn);
        if (!tx.active()) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "failed to start transaction"}};
        }

        int r1 = conn->execute("INSERT IGNORE INTO friends (user_id, friend_id) VALUES ("
            + std::to_string(userId) + "," + std::to_string(friendId) + ")");
        int r2 = conn->execute("INSERT IGNORE INTO friends (user_id, friend_id) VALUES ("
            + std::to_string(friendId) + "," + std::to_string(userId) + ")");

        bool success = (r1 >= 0 && r2 >= 0) && tx.commit();
        db_->release(std::move(conn));
        if (!success) return {{"success", false}, {"message", "add failed"}};
        return {{"success", true}};
    }
```

### Step 2: 同样处理 deleteFriend（2 条 DELETE）

```cpp
    json deleteFriend(int64_t userId, int64_t friendId) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {{"success", false}, {"message", "db error"}};

        TransactionGuard tx(conn);
        if (!tx.active()) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "failed to start transaction"}};
        }

        int r1 = conn->execute("DELETE FROM friends WHERE user_id=" + std::to_string(userId)
                      + " AND friend_id=" + std::to_string(friendId));
        int r2 = conn->execute("DELETE FROM friends WHERE user_id=" + std::to_string(friendId)
                      + " AND friend_id=" + std::to_string(userId));

        bool success = (r1 >= 0 && r2 >= 0) && tx.commit();
        db_->release(std::move(conn));
        if (!success) return {{"success", false}, {"message", "delete failed"}};
        return {{"success", true}};
    }
```

### Step 3: 编译 + commit

```bash
make muduo-im -j$(nproc) 2>&1 | grep "error:" | head -3
git add src/server/FriendService.h && \
  git commit -m "feat: wrap addFriend/deleteFriend in transactions"
```

---

## Task 6: init.sql 添加外键约束和级联删除

**Files:**
- Modify: `sql/init.sql`

### Step 1: 重写 init.sql 添加外键

READ 当前的 init.sql。然后重写 CREATE TABLE 语句，对 friends/group_members/friend_requests/messages 添加外键约束：

```sql
CREATE DATABASE IF NOT EXISTS muduo_im DEFAULT CHARACTER SET utf8mb4;
USE muduo_im;

CREATE TABLE IF NOT EXISTS users (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    username VARCHAR(64) UNIQUE NOT NULL,
    password VARCHAR(128) NOT NULL,
    nickname VARCHAR(64),
    avatar VARCHAR(256) DEFAULT '',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS friends (
    user_id BIGINT NOT NULL,
    friend_id BIGINT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (user_id, friend_id),
    INDEX idx_user (user_id),
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE,
    FOREIGN KEY (friend_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS `groups` (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(128) NOT NULL,
    owner_id BIGINT NOT NULL,
    announcement TEXT DEFAULT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_groups_owner (owner_id),
    FOREIGN KEY (owner_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS group_members (
    group_id BIGINT NOT NULL,
    user_id BIGINT NOT NULL,
    joined_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (group_id, user_id),
    INDEX idx_user_groups (user_id),
    FOREIGN KEY (group_id) REFERENCES `groups`(id) ON DELETE CASCADE,
    FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS private_messages (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    msg_id VARCHAR(36) UNIQUE,
    from_user BIGINT NOT NULL,
    to_user BIGINT NOT NULL,
    content TEXT,
    msg_type TINYINT DEFAULT 0,
    recalled TINYINT DEFAULT 0,
    timestamp BIGINT NOT NULL,
    INDEX idx_chat (from_user, to_user, timestamp),
    INDEX idx_inbox (to_user, timestamp)
    -- 消息不加外键：保留历史消息即使用户删了
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS group_messages (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    msg_id VARCHAR(36) UNIQUE,
    group_id BIGINT NOT NULL,
    from_user BIGINT NOT NULL,
    content TEXT,
    msg_type TINYINT DEFAULT 0,
    recalled TINYINT DEFAULT 0,
    timestamp BIGINT NOT NULL,
    INDEX idx_group_time (group_id, timestamp)
    -- 不加外键：保留历史
) ENGINE=InnoDB;

CREATE TABLE IF NOT EXISTS friend_requests (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    from_user BIGINT NOT NULL,
    to_user BIGINT NOT NULL,
    status TINYINT DEFAULT 0 COMMENT '0=pending, 1=accepted, 2=rejected',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    INDEX idx_to_user (to_user, status),
    UNIQUE KEY uk_pair (from_user, to_user),
    FOREIGN KEY (from_user) REFERENCES users(id) ON DELETE CASCADE,
    FOREIGN KEY (to_user) REFERENCES users(id) ON DELETE CASCADE
) ENGINE=InnoDB;
```

**注意：** 既有数据库若已有悬挂数据，加外键会失败。提供数据清理脚本。

### Step 2: 清理脚本 sql/migrate_fk.sql

```sql
-- 清理悬挂引用后再加外键
USE muduo_im;

DELETE FROM friends WHERE user_id NOT IN (SELECT id FROM users);
DELETE FROM friends WHERE friend_id NOT IN (SELECT id FROM users);
DELETE FROM group_members WHERE user_id NOT IN (SELECT id FROM users);
DELETE FROM group_members WHERE group_id NOT IN (SELECT id FROM `groups`);
DELETE FROM `groups` WHERE owner_id NOT IN (SELECT id FROM users);
DELETE FROM friend_requests WHERE from_user NOT IN (SELECT id FROM users);
DELETE FROM friend_requests WHERE to_user NOT IN (SELECT id FROM users);

-- 然后应用外键
ALTER TABLE friends ADD CONSTRAINT fk_friends_user FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE;
ALTER TABLE friends ADD CONSTRAINT fk_friends_friend FOREIGN KEY (friend_id) REFERENCES users(id) ON DELETE CASCADE;
ALTER TABLE `groups` ADD CONSTRAINT fk_groups_owner FOREIGN KEY (owner_id) REFERENCES users(id) ON DELETE CASCADE;
ALTER TABLE group_members ADD CONSTRAINT fk_gm_group FOREIGN KEY (group_id) REFERENCES `groups`(id) ON DELETE CASCADE;
ALTER TABLE group_members ADD CONSTRAINT fk_gm_user FOREIGN KEY (user_id) REFERENCES users(id) ON DELETE CASCADE;
ALTER TABLE friend_requests ADD CONSTRAINT fk_fr_from FOREIGN KEY (from_user) REFERENCES users(id) ON DELETE CASCADE;
ALTER TABLE friend_requests ADD CONSTRAINT fk_fr_to FOREIGN KEY (to_user) REFERENCES users(id) ON DELETE CASCADE;
```

### Step 3: 应用到 dev 数据库验证

```bash
sudo mysql muduo_im -e "source sql/migrate_fk.sql" 2>&1 | head -10
sudo mysql muduo_im -e "SELECT CONSTRAINT_NAME FROM information_schema.KEY_COLUMN_USAGE WHERE TABLE_SCHEMA='muduo_im' AND REFERENCED_TABLE_NAME IS NOT NULL;" 2>&1
```

Expected: 7 个外键约束

### Step 4: 跑 E2E 测试验证

```bash
bash tests/e2e_test.sh 2>&1 | tail -5
```

Expected: `21 通过, 0 失败`

### Step 5: Commit

```bash
git add sql/init.sql sql/migrate_fk.sql && \
  git commit -m "feat: add foreign keys and ON DELETE CASCADE"
```

---

## Task 7: Redis AOF 持久化 + 文档说明

**Files:**
- Create: `docs/REDIS_CONFIG.md`
- Modify: `docs/DEPLOYMENT.md` (加引用)

### Step 1: 写 REDIS_CONFIG.md

```markdown
# Redis 持久化配置

## 为什么需要 AOF

muduo-im 使用 Redis 作为消息队列缓冲（msg_queue），每 2 秒批量刷到 MySQL。
若 Redis 崩溃且只用默认 RDB，最多可能丢失 **最近 15 分钟的消息**。

启用 AOF 可将丢失窗口缩短到 **最多 1 秒**。

## 配置方法

### 方法 1：修改 /etc/redis/redis.conf

```conf
appendonly yes
appendfsync everysec
appendfilename "appendonly.aof"
dir /var/lib/redis

# 自动重写（避免 AOF 文件过大）
auto-aof-rewrite-percentage 100
auto-aof-rewrite-min-size 64mb
```

重启 Redis：
```bash
sudo systemctl restart redis-server
redis-cli config get appendonly  # 确认: 1) "appendonly"  2) "yes"
```

### 方法 2：运行时启用（无需重启）

```bash
redis-cli CONFIG SET appendonly yes
redis-cli CONFIG SET appendfsync everysec
redis-cli CONFIG REWRITE  # 持久化配置到磁盘
```

## 验证

```bash
ls -la /var/lib/redis/appendonly.aof  # 应该存在且持续增长
```

## fsync 策略选择

| 策略 | 丢失窗口 | 性能影响 |
|------|---------|---------|
| `always` | 0（每条写都 fsync） | 严重（QPS 降 10x） |
| `everysec` | 1 秒 | 轻微（推荐） |
| `no` | 操作系统控制（通常 30s） | 无 |

muduo-im 推荐 `everysec`。
```

### Step 2: 在 DEPLOYMENT.md 加引用

找到现有的 Redis 配置段落，在下面添加：

```markdown
### Redis 持久化

生产环境必须启用 AOF 持久化避免消息丢失。详见 [REDIS_CONFIG.md](REDIS_CONFIG.md)。
```

### Step 3: Commit

```bash
git add docs/REDIS_CONFIG.md docs/DEPLOYMENT.md && \
  git commit -m "docs: add Redis AOF configuration guide"
```

---

## Task 8: 最终验证 + 推送

### Step 1: 全量编译

```bash
cd build && cmake .. && make muduo-im -j$(nproc) 2>&1 | grep "error:" | head -5
```

### Step 2: E2E 测试

```bash
bash stop.sh 2>/dev/null
bash start.sh
sleep 3
bash tests/e2e_test.sh 2>&1 | tail -10
bash stop.sh
```

Expected: 21/21 通过

### Step 3: 推送

```bash
# mymuduo-http (Task 1 的 TransactionGuard)
cd /home/ly/workspaces/im/mymuduo-http && git push origin main

# muduo-im (Task 2-7)
cd /home/ly/workspaces/im/muduo-im && git push origin main
```

---

## 总结

| Task | 改动 | 风险 | 回滚 |
|------|------|------|------|
| 1 | MySQLPool TransactionGuard | 低 | 删除新增类 |
| 2 | FriendService.handleRequest | 低 | git revert |
| 3 | GroupService.deleteGroup | 低 | git revert |
| 4 | UserService.deleteAccount | 低 | git revert |
| 5 | FriendService.addFriend/deleteFriend | 低 | git revert |
| 6 | 外键约束 | **中**（可能破坏既有数据） | migrate_fk.sql 加 DROP FOREIGN KEY |
| 7 | Redis AOF 文档 | 零 | — |
| 8 | 编译测试推送 | 零 | — |

**预计耗时：** 1-2 小时

**不在本计划内的（未来可做）：**
- 跨实例消息路由（Redis Pub/Sub）— 单机部署不需要
- 分布式事务（Seata 等）— 当前规模不需要
- 双写模式（MySQL + Redis 同步）— 会降低吞吐
