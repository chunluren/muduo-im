# muduo-im 技术实现细节

## JWT 实现

位置: `src/common/JWT.h`

### 生成流程

1. 构造 header: `{"alg":"HS256","typ":"JWT"}`
2. 构造 payload: `{"userId": N, "iat": now_seconds, "exp": now + 86400}`
3. 分别 Base64URL 编码 header 和 payload
4. 拼接 `headerB64.payloadB64` 作为签名输入
5. 使用 OpenSSL HMAC-SHA256 + 密钥计算签名
6. 签名结果再 Base64URL 编码
7. 最终 token = `headerB64.payloadB64.signatureB64`

### 验证流程

1. 按 `.` 分割为三部分
2. 重新计算 `header.payload` 的 HMAC-SHA256 签名
3. 对比计算签名与 token 中的签名（字符串比较）
4. 解码 payload，检查 `exp` 是否过期
5. 返回 `userId`，失败返回 -1

### Base64URL 编码

标准 Base64 替换: `+` -> `-`, `/` -> `_`, 去掉 `=` 填充。

## 密码存储

位置: `src/common/Protocol.h`

```
hashPassword(password) = SHA256("muduo-im-salt-v1" + password)
```

- 使用 OpenSSL EVP API 计算 SHA256
- 固定 salt: `muduo-im-salt-v1`
- 输出为 64 字符 hex 字符串
- 验证时重新计算哈希并比较

## WebSocket 消息协议

所有消息均为 JSON 文本帧，通过 `type` 字段区分类型。

### 客户端 -> 服务端

| type | 字段 | 说明 |
|------|------|------|
| `msg` | to, content | 私聊消息，to 为目标用户 ID |
| `group_msg` | to, content | 群聊消息，to 为群组 ID |
| `file_msg` | to, url, filename, fileSize | 文件消息通知 |
| `recall` | msgId | 消息撤回请求 |
| `read_ack` | to, lastMsgId | 已读回执，to 为消息原发送者 |

### 服务端 -> 客户端

| type | 字段 | 说明 |
|------|------|------|
| `msg` | from, to, content, msgId, timestamp | 私聊消息转发 |
| `group_msg` | from, to, content, msgId, timestamp | 群聊消息转发 |
| `file_msg` | from, to, url, filename, fileSize, msgId, timestamp | 文件消息转发 |
| `ack` | msgId | 消息发送确认 |
| `recall` | msgId, from | 撤回通知 |
| `read_ack` | from, to, lastMsgId | 已读回执转发 |
| `online` | userId | 好友上线通知 |
| `offline` | userId | 好友离线通知 |
| `error` | message | 错误消息 |

### 消息 ID 生成

使用 UUID v4 格式: `xxxxxxxx-xxxx-4xxx-Nxxx-xxxxxxxxxxxx`，通过 `std::random_device` + `mt19937` 生成随机数，设置 version=4 和 variant bits。

## 消息撤回（软删除）

位置: `MessageService::recallMessage()`

### 规则

- 只能撤回自己发送的消息（WHERE from_user = userId）
- 2 分钟时间窗口（WHERE timestamp > now - 120000ms）
- 同时检查 private_messages 和 group_messages 两张表

### 实现

消息撤回采用软删除策略，设置 `recalled = 1` 而非物理删除:

```sql
UPDATE private_messages SET recalled = 1
WHERE msg_id='xxx' AND from_user=123 AND timestamp > (now - 120000)
```

如果 private_messages 未命中，继续尝试 group_messages。查询历史消息时通过 `recalled` 字段过滤已撤回消息。

### 通知

撤回成功后，向发送者本人 + 所有在线好友广播 recall 消息。客户端收到后从界面移除该消息。

## 已读回执

位置: `ChatServer::handleReadAck()`

### 流程

1. 用户 A 阅读了用户 B 的消息
2. A 发送: `{type:"read_ack", to:"B_id", lastMsgId:"xxx"}`
3. 服务端查找 B 的在线 session
4. 转发给 B: `{type:"read_ack", from:"A_id", to:"B_id", lastMsgId:"xxx"}`
5. B 的客户端更新消息已读状态

已读回执持久化到 Redis: 服务端收到 read_ack 后，将已读位置写入 Redis（key: `read:{userId}:{peerId}`, value: lastMsgId + timestamp）。查询接口 `GET /api/messages/read-status?peerId=NNN` 从 Redis 读取。如果 B 在线则实时转发，不在线时已读状态仍可通过 REST API 查询。

## 好友关系

位置: `FriendService`

### 好友申请流程

好友添加已从直接添加改为申请制:

1. 用户 A 发送好友申请: `POST /api/friends/request {friendId: B}`
2. 申请记录写入 `friend_requests` 表，状态为 `pending`
3. 用户 B 查看申请列表: `GET /api/friends/requests`
4. 用户 B 处理申请: `POST /api/friends/handle {requestId: N, action: "accept"}` 或 `"reject"`
5. 同意后，自动双向写入 `friends` 表

### 双向存储

同意好友申请后插入两条记录:
```sql
INSERT IGNORE INTO friends (user_id, friend_id) VALUES (1, 2)
INSERT IGNORE INTO friends (user_id, friend_id) VALUES (2, 1)
```

删除好友同样删除两条记录。使用 `INSERT IGNORE` 防止重复添加报错。

### 上线/下线通知

用户连接 WebSocket 时，查询其好友列表，向所有在线好友发送 `online` 通知。断开连接时发送 `offline` 通知。

## 群消息广播

位置: `ChatServer::handleGroupMessage()`

### 流程

1. 保存消息到 group_messages 表
2. 发送 ACK 给发送者
3. 查询 `group_members` 获取所有成员 ID
4. 遍历成员列表，跳过发送者自己
5. 对每个成员，查询 OnlineManager 是否在线
6. 在线则通过 WsSession 发送消息

### 扩散策略

- 群消息采用**读扩散**: 消息只存一份到 group_messages，成员查询时按 group_id 读取
- 私聊消息采用**写扩散**: 消息存一份，查询时用 `(from, to)` 双向匹配

## OnlineManager 线程安全

位置: `src/server/OnlineManager.h`

- 内部维护 `unordered_map<int64_t, WsSessionPtr>`
- 所有方法（addUser, removeUser, getSession, isOnline, getOnlineUsers, getUserId）均使用 `std::lock_guard<std::mutex>` 保护
- getSession 额外检查 `session->isOpen()` 防止返回已关闭的连接

## Redis 降级策略

位置: `MessageService::queueMessage()` / `MessageService::directSaveMessage()`

当 Redis 不可用时（连接失败、超时等），消息队列操作会自动降级为直接写入 MySQL:

1. 正常路径: 消息先写入 Redis 队列，后台消费者批量写入 MySQL
2. 降级路径: Redis 操作失败时，调用 `directSaveMessage()` 直接 INSERT 到 MySQL
3. 降级对用户透明，消息不会丢失，仅写入延迟略有变化

## 文件上传校验

位置: `ChatServer::handleUpload()`

### 校验逻辑

1. 文件大小限制: 最大 50MB，超出返回 413 错误
2. 文件类型白名单: 仅允许安全的文件扩展名（图片、文档、压缩包等）
3. 文件名清理: 防止路径穿越攻击（过滤 `..` 和 `/`）
4. 存储路径: 文件保存到 `uploads/` 目录，使用 UUID 重命名防止冲突

## CORS 支持

ChatServer 启动时调用 `httpServer_.enableCors()`，允许前端跨域访问 REST API。

## 消息幂等投递

位置: `ChatServer::handleMessage()` / `ChatServer::handleGroupMessage()`

### 机制

客户端为每条消息分配唯一 `msgId`（UUID v4），服务端维护一个 in-memory 去重集合:

1. 收到消息时，检查 `msgId` 是否已存在于去重集合中
2. 若已存在，直接返回 ACK，不重复写入数据库也不重复广播
3. 若不存在，将 `msgId` 加入集合，正常处理消息

### 容量限制

去重集合使用 `std::unordered_set<std::string>` 实现，上限 100,000 条。当集合满时，清空整个集合重新开始。这是一个简单的有界策略，适用于单实例部署场景。

### 适用场景

- 客户端因网络抖动重发相同消息
- WebSocket 断线重连后重发未确认消息

## 配置文件系统

位置: `src/server/main.cpp`

### 格式

采用简单的 key=value INI 格式:

```ini
# MySQL
mysql_host=127.0.0.1
mysql_port=3306
mysql_user=root
mysql_password=
mysql_database=muduo_im
mysql_min_conn=5
mysql_max_conn=20

# Redis
redis_host=127.0.0.1
redis_port=6379
redis_min_conn=3
redis_max_conn=10

# JWT
jwt_secret=muduo-im-jwt-secret-key

# Server
http_port=8080
ws_port=9090
```

### 加载优先级

1. 命令行参数指定路径: `./muduo-im ../config.ini`
2. 当前目录下的 `config.ini`（自动检测）
3. 无配置文件时使用代码中的默认值

### 解析实现

逐行读取文件，忽略空行和 `#` 开头的注释行，按第一个 `=` 分割 key 和 value，去除前后空白。解析结果存入 `std::unordered_map<std::string, std::string>`，启动时按 key 覆盖默认配置。

## 消息搜索实现

位置: `MessageService::searchMessages()`

### 查询逻辑

消息搜索同时检索私聊和群聊两张表，使用 SQL LIKE 进行关键词匹配:

**私聊消息搜索:**
```sql
SELECT * FROM private_messages
WHERE (from_user = ? OR to_user = ?) AND content LIKE '%keyword%' AND recalled = 0
ORDER BY timestamp DESC LIMIT 50
```

**群聊消息搜索:**
```sql
SELECT gm.* FROM group_messages gm
JOIN group_members mem ON gm.group_id = mem.group_id
WHERE mem.user_id = ? AND gm.content LIKE '%keyword%' AND gm.recalled = 0
ORDER BY gm.timestamp DESC LIMIT 50
```

### 权限控制

- 私聊: 只搜索当前用户参与的会话（from_user 或 to_user 为自己）
- 群聊: 通过 JOIN group_members 确保只搜索用户已加入的群组消息
- 已撤回消息（recalled=1）不出现在搜索结果中

### 可选过滤

- `peerId` 参数: 限定与某用户的私聊记录
- `groupId` 参数: 限定某群组的聊天记录
- 两者均为可选，不传则搜索所有会话
