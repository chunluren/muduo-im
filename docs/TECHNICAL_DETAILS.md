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

## 消息撤回

位置: `MessageService::recallMessage()`

### 规则

- 只能撤回自己发送的消息（WHERE from_user = userId）
- 2 分钟时间窗口（WHERE timestamp > now - 120000ms）
- 同时检查 private_messages 和 group_messages 两张表

### 实现

```sql
DELETE FROM private_messages
WHERE msg_id='xxx' AND from_user=123 AND timestamp > (now - 120000)
```

如果 private_messages 未命中，继续尝试 group_messages。

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

注意: 已读回执不持久化到数据库，仅在线转发。如果 B 不在线则丢弃。

## 好友关系

位置: `FriendService`

### 双向存储

添加好友时插入两条记录:
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

## CORS 支持

ChatServer 启动时调用 `httpServer_.enableCors()`，允许前端跨域访问 REST API。
