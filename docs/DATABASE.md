# muduo-im 数据库设计

## 测试数据库

单元测试使用独立的 `muduo_im_test` 数据库，避免污染开发数据。

初始化：
```bash
sudo mysql -e "CREATE DATABASE IF NOT EXISTS muduo_im_test DEFAULT CHARACTER SET utf8mb4;"
sudo mysql muduo_im_test < sql/init.sql
```

每个测试用例开始时调用 `cleanTestDb()`（在 `tests/test_helper.h`）TRUNCATE 所有表。

## ER 图

```
┌──────────────┐       ┌──────────────┐       ┌──────────────┐
│    users     │       │   friends    │       │   groups     │
├──────────────┤       ├──────────────┤       ├──────────────┤
│ id       PK  │◀──┐   │ user_id   PK │───────▶│ id       PK  │
│ username UNI │   ├───│ friend_id PK │       │ name         │
│ password     │   │   │ created_at   │       │ owner_id  FK │──┐
│ nickname     │   │   └──────────────┘       │ created_at   │  │
│ avatar       │   │                          └──────────────┘  │
│ created_at   │   │                                 │          │
└──────────────┘   │                                 │          │
       │           │   ┌──────────────────┐
       │           │   │ friend_requests  │
       │           │   ├──────────────────┤
       │           ├───│ from_user    FK  │
       │           ├───│ to_user      FK  │
       │           │   │ id           PK  │
       │           │   │ status           │
       │           │   │ created_at       │
       │           │   └──────────────────┘
       │           │
       │           │   ┌──────────────────┐          │          │
       │           │   │  group_members   │          │          │
       │           │   ├──────────────────┤          │          │
       │           ├───│ user_id      PK  │          │          │
       │           │   │ group_id     PK  │──────────┘          │
       │           │   │ joined_at        │                     │
       │           │   └──────────────────┘                     │
       │           │                                            │
       │           │   ┌──────────────────┐                     │
       │           │   │private_messages  │                     │
       │           │   ├──────────────────┤                     │
       │           ├───│ from_user        │                     │
       │           ├───│ to_user          │                     │
       │           │   │ id           PK  │                     │
       │           │   │ msg_id      UNI  │                     │
       │           │   │ content          │                     │
       │           │   │ msg_type         │                     │
       │           │   │ timestamp        │                     │
       │           │   └──────────────────┘                     │
       │           │                                            │
       │           │   ┌──────────────────┐                     │
       │           │   │ group_messages   │                     │
       │           │   ├──────────────────┤                     │
       │           ├───│ from_user        │                     │
       │               │ id           PK  │                     │
       │               │ msg_id      UNI  │                     │
       │               │ group_id     FK  │─────────────────────┘
       │               │ content          │
       │               │ msg_type         │
       │               │ timestamp        │
       └───────────────└──────────────────┘
```

## 表结构详解

### users -- 用户表

| 字段 | 类型 | 约束 | 说明 |
|------|------|------|------|
| id | BIGINT | PK, AUTO_INCREMENT | 用户 ID |
| username | VARCHAR(64) | UNIQUE, NOT NULL | 登录用户名 |
| password | VARCHAR(128) | NOT NULL | SHA256 哈希后的密码 |
| nickname | VARCHAR(64) | | 昵称，默认同 username |
| avatar | VARCHAR(256) | DEFAULT '' | 头像 URL |
| created_at | TIMESTAMP | DEFAULT CURRENT_TIMESTAMP | 注册时间 |

### friends -- 好友关系表

| 字段 | 类型 | 约束 | 说明 |
|------|------|------|------|
| user_id | BIGINT | PK (联合) | 用户 ID |
| friend_id | BIGINT | PK (联合) | 好友 ID |
| created_at | TIMESTAMP | DEFAULT CURRENT_TIMESTAMP | 添加时间 |

索引: `idx_user (user_id)` -- 加速查询某用户的好友列表。

设计: 双向存储，添加好友时写入 (A,B) 和 (B,A) 两条记录，删除时同样删除两条。

### friend_requests -- 好友申请表

| 字段 | 类型 | 约束 | 说明 |
|------|------|------|------|
| id | BIGINT | PK, AUTO_INCREMENT | 申请 ID |
| from_user | BIGINT | NOT NULL | 申请发起者 ID |
| to_user | BIGINT | NOT NULL | 申请目标用户 ID |
| status | VARCHAR(16) | DEFAULT 'pending' | 状态: pending / accepted / rejected |
| created_at | TIMESTAMP | DEFAULT CURRENT_TIMESTAMP | 申请时间 |

索引: `idx_to_user (to_user)` -- 加速查询某用户收到的好友申请。

设计: 用户 A 向 B 发送好友申请，B 同意后自动在 friends 表中双向插入记录。

### groups -- 群组表

| 字段 | 类型 | 约束 | 说明 |
|------|------|------|------|
| id | BIGINT | PK, AUTO_INCREMENT | 群组 ID |
| name | VARCHAR(128) | NOT NULL | 群名称 |
| owner_id | BIGINT | NOT NULL | 群主用户 ID |
| created_at | TIMESTAMP | DEFAULT CURRENT_TIMESTAMP | 创建时间 |

### group_members -- 群成员表

| 字段 | 类型 | 约束 | 说明 |
|------|------|------|------|
| group_id | BIGINT | PK (联合) | 群组 ID |
| user_id | BIGINT | PK (联合) | 成员用户 ID |
| joined_at | TIMESTAMP | DEFAULT CURRENT_TIMESTAMP | 加入时间 |

索引: `idx_user_groups (user_id)` -- 加速查询某用户加入的群列表。

### private_messages -- 私聊消息表

| 字段 | 类型 | 约束 | 说明 |
|------|------|------|------|
| id | BIGINT | PK, AUTO_INCREMENT | 自增 ID |
| msg_id | VARCHAR(36) | UNIQUE | UUID 消息 ID |
| from_user | BIGINT | NOT NULL | 发送者 ID |
| to_user | BIGINT | NOT NULL | 接收者 ID |
| content | TEXT | | 消息内容（文件消息为 JSON） |
| msg_type | TINYINT | DEFAULT 0 | 消息类型（预留） |
| timestamp | BIGINT | NOT NULL | 毫秒时间戳 |
| recalled | TINYINT | DEFAULT 0 | 是否已撤回（0=正常, 1=已撤回） |

索引:
- `idx_chat (from_user, to_user, timestamp)` -- 查询两人之间的聊天记录
- `idx_inbox (to_user, timestamp)` -- 查询某用户收到的消息

### group_messages -- 群聊消息表

| 字段 | 类型 | 约束 | 说明 |
|------|------|------|------|
| id | BIGINT | PK, AUTO_INCREMENT | 自增 ID |
| msg_id | VARCHAR(36) | UNIQUE | UUID 消息 ID |
| group_id | BIGINT | NOT NULL | 群组 ID |
| from_user | BIGINT | NOT NULL | 发送者 ID |
| content | TEXT | | 消息内容 |
| msg_type | TINYINT | DEFAULT 0 | 消息类型（预留） |
| timestamp | BIGINT | NOT NULL | 毫秒时间戳 |
| recalled | TINYINT | DEFAULT 0 | 是否已撤回（0=正常, 1=已撤回） |

索引: `idx_group_time (group_id, timestamp)` -- 按群和时间查询历史消息。

## 查询模式分析

### 私聊历史查询

```sql
SELECT ... FROM private_messages
WHERE ((from_user=A AND to_user=B) OR (from_user=B AND to_user=A))
  AND timestamp < ?
ORDER BY timestamp DESC LIMIT 50
```

使用 `idx_chat (from_user, to_user, timestamp)` 覆盖 `(A->B)` 方向查询。`(B->A)` 方向同样命中该索引。两个方向结果合并后按 timestamp 排序。

### 收件箱查询

`idx_inbox (to_user, timestamp)` 用于查询某用户收到的所有消息（目前代码未直接使用，为未来功能预留）。

### 群聊历史查询

```sql
SELECT ... FROM group_messages
WHERE group_id=? AND timestamp < ?
ORDER BY timestamp DESC LIMIT 50
```

直接命中 `idx_group_time (group_id, timestamp)` 组合索引。

## 写扩散 vs 读扩散

| | 私聊 | 群聊 |
|--|------|------|
| 策略 | 写扩散 | 读扩散 |
| 存储 | 每条消息存 1 条记录 | 每条消息存 1 条记录 |
| 查询 | 按 (from, to) 双向查 | 按 group_id 查 |
| 优点 | 查询直接定位两人对话 | 写入只需 1 次 INSERT |
| 缺点 | 如需收件箱需额外索引 | 大群查询时需过滤 |

私聊消息只存一份但包含 from_user 和 to_user，查询时双向匹配，本质上是写扩散的简化形式。群聊消息按 group_id 存储，所有成员读取同一份数据，是典型的读扩散模型。

## 外键约束

所有非消息表添加了 FOREIGN KEY + ON DELETE CASCADE 级联删除：

| 表 | 约束 | 级联行为 |
|----|------|---------|
| friends.user_id → users.id | CASCADE | 用户删除时清理好友关系 |
| friends.friend_id → users.id | CASCADE | 同上（另一方向） |
| groups.owner_id → users.id | CASCADE | 群主删号时删群 |
| group_members.group_id → groups.id | CASCADE | 群解散时清理成员 |
| group_members.user_id → users.id | CASCADE | 用户删号时退出所有群 |
| friend_requests.from_user → users.id | CASCADE | 清理申请 |
| friend_requests.to_user → users.id | CASCADE | 清理申请 |

**消息表无外键**：保留历史消息即使用户已删号。

### 迁移脚本

`sql/migrate_fk.sql` 用于为已有数据库添加外键：
1. 先清理悬挂引用
2. 再 ALTER TABLE ADD CONSTRAINT

新部署直接用 `sql/init.sql`（外键已内置）。
