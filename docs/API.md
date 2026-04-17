# muduo-im API 文档

## REST API

基础地址: `http://localhost:8080`

认证方式: 除注册和登录外，所有接口需要 `Authorization: Bearer <token>` 请求头。

### POST /api/register

注册新用户。

**Request:**
```json
{
  "username": "alice",
  "password": "secret123",
  "nickname": "Alice"       // 可选，默认同 username
}
```

**Response (200):**
```json
{"success": true, "userId": 1, "message": "registered"}
```

**Error:**
```json
{"success": false, "message": "username already exists"}
{"success": false, "message": "username and password required"}
{"success": false, "message": "username or password too long"}
```

限制: username/password 最长 64 字符。

---

### POST /api/login

用户登录，获取 JWT token。

**Request:**
```json
{
  "username": "alice",
  "password": "secret123"
}
```

**Response (200):**
```json
{
  "success": true,
  "token": "eyJhbGci...",
  "userId": 1,
  "nickname": "Alice"
}
```

**Error:**
```json
{"success": false, "message": "user not found"}
{"success": false, "message": "wrong password"}
```

Token 有效期: 24 小时。

---

### GET /api/friends

获取当前用户的好友列表。需要认证。

**Response (200):**
```json
[
  {"userId": 2, "username": "bob", "nickname": "Bob", "avatar": ""}
]
```

**Error (401):** `unauthorized`

---

### POST /api/friends/add

添加好友（双向）。需要认证。

**Request:**
```json
{"friendId": 2}
```

**Response (200):**
```json
{"success": true}
```

**Error:**
```json
{"success": false, "message": "cannot add self"}
```

---

### POST /api/friends/delete

删除好友（双向）。需要认证。

**Request:**
```json
{"friendId": 2}
```

**Response (200):**
```json
{"success": true}
```

---

### GET /api/groups

获取当前用户加入的群组列表。需要认证。

**Response (200):**
```json
[
  {"groupId": 1, "name": "开发组", "ownerId": 1}
]
```

---

### POST /api/groups/create

创建群组（创建者自动加入）。需要认证。

**Request:**
```json
{"name": "开发组"}
```

**Response (200):**
```json
{"success": true, "groupId": 1}
```

**Error:**
```json
{"success": false, "message": "name required"}
```

---

### POST /api/groups/join

加入群组。需要认证。

**Request:**
```json
{"groupId": 1}
```

**Response (200):**
```json
{"success": true}
```

---

### GET /api/groups/members?groupId=1

获取群成员列表。需要认证。

**Response (200):**
```json
[
  {"userId": 1, "username": "alice", "nickname": "Alice"},
  {"userId": 2, "username": "bob", "nickname": "Bob"}
]
```

---

### GET /api/messages/history

获取聊天历史记录。需要认证。

**Query 参数:**
- `peerId` -- 对方用户 ID（私聊时使用）
- `groupId` -- 群组 ID（群聊时使用，优先级高于 peerId）
- `before` -- 毫秒时间戳，获取此时间之前的消息（分页用）

返回最多 50 条消息，按时间倒序排列。

**Response -- 私聊 (200):**
```json
[
  {"msgId": "uuid", "from": 1, "to": 2, "content": "hello", "timestamp": 1712880000000}
]
```

**Response -- 群聊 (200):**
```json
[
  {"msgId": "uuid", "from": 1, "content": "hello", "timestamp": 1712880000000}
]
```

---

### POST /api/upload

上传文件（multipart/form-data）。需要认证。

**Request:** Content-Type: multipart/form-data，包含文件 part。

**Response (200):**
```json
{
  "success": true,
  "url": "/uploads/xxxxxxxx-xxxx-4xxx-xxxx-xxxxxxxxxxxx.jpg",
  "filename": "photo.jpg",
  "size": 102400
}
```

**Error:**
```json
{"message": "missing boundary"}
{"message": "no file uploaded"}
{"message": "no file found in upload"}
```

上传后的文件可通过 `GET /uploads/<filename>` 下载。

---

## WebSocket 协议

### 连接

```
ws://localhost:9090/ws?token=<JWT_TOKEN>
```

握手时服务端验证 token，无效则拒绝连接（关闭码 1008）。

连接成功后，服务端自动向用户的在线好友发送 `online` 通知。

### 客户端发送

#### 私聊消息
```json
{"type": "msg", "to": "2", "content": "你好"}
```

#### 群聊消息
```json
{"type": "group_msg", "to": "100", "content": "大家好"}
```

#### 文件消息（先上传文件再发送通知）
```json
{
  "type": "file_msg",
  "to": "2",
  "url": "/uploads/xxx.jpg",
  "filename": "photo.jpg",
  "fileSize": 102400
}
```

#### 消息撤回
```json
{"type": "recall", "msgId": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"}
```

#### 已读回执
```json
{"type": "read_ack", "to": "2", "lastMsgId": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"}
```

### 服务端推送

#### 收到私聊消息
```json
{
  "type": "msg",
  "from": "1",
  "to": "2",
  "content": "你好",
  "msgId": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
  "timestamp": 1712880000000
}
```

#### 收到群聊消息
```json
{
  "type": "group_msg",
  "from": "1",
  "to": "100",
  "content": "大家好",
  "msgId": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
  "timestamp": 1712880000000
}
```

#### 文件消息通知
```json
{
  "type": "file_msg",
  "from": "1",
  "to": "2",
  "url": "/uploads/xxx.jpg",
  "filename": "photo.jpg",
  "fileSize": 102400,
  "msgId": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
  "timestamp": 1712880000000
}
```

#### 发送确认
```json
{"type": "ack", "msgId": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"}
```

#### 撤回通知
```json
{"type": "recall", "msgId": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx", "from": "1"}
```

#### 已读回执
```json
{"type": "read_ack", "from": "2", "to": "1", "lastMsgId": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"}
```

#### 好友上线
```json
{"type": "online", "userId": "2"}
```

#### 好友离线
```json
{"type": "offline", "userId": "2"}
```

#### 错误消息
```json
{"type": "error", "message": "invalid JSON"}
```

可能的错误信息: `invalid JSON`, `not authenticated`, `missing 'to' or 'content'`, `invalid 'to'`, `invalid group id`, `missing 'to' or 'url'`, `missing msgId`, `recall failed (timeout or not your message)`, `unknown message type`

---

### POST /api/groups/leave
退出群组（群主不可退出，需使用解散）

**Request:** `{"groupId": 1}`
**Response:** `{"success": true}`

### POST /api/groups/delete
解散群组（仅群主）

**Request:** `{"groupId": 1}`
**Response:** `{"success": true}`

### GET /api/user/info?userId=NNN
查看其他用户公开资料

**Response:** `{"success": true, "userId": 2, "username": "xxx", "nickname": "xxx", "avatar": ""}`

### POST /api/user/password
修改密码

**Request:** `{"oldPassword": "old", "newPassword": "new"}`
**Response:** `{"success": true}`

### POST /api/user/delete
注销账号（需要密码确认）

**Request:** `{"password": "xxx"}`
**Response:** `{"success": true}`

### WebSocket: typing
正在输入提示

**Client sends:** `{"type": "typing", "to": "userId"}`
**Server forwards:** `{"type": "typing", "from": "userId", "to": "userId"}`

---

### GET /api/messages/search?keyword=xxx

搜索聊天记录。需要认证。

**Query 参数:**
- `keyword` -- 搜索关键词（必填，SQL LIKE 匹配）
- `peerId` -- 限定与某用户的私聊记录（可选）
- `groupId` -- 限定某群组的聊天记录（可选）

返回最多 50 条匹配消息，按时间倒序排列。

**Response (200):**
```json
[
  {"msgId": "uuid", "from": 1, "to": 2, "content": "matched text", "timestamp": 1712880000000}
]
```

**Error:**
```json
{"success": false, "message": "keyword required"}
```

---

### POST /api/groups/announcement

设置群公告（仅群主）。需要认证。

**Request:**
```json
{
  "groupId": 1,
  "content": "公告内容"
}
```

**Response (200):**
```json
{"success": true}
```

**Error:**
```json
{"success": false, "message": "only owner can set announcement"}
{"success": false, "message": "groupId and content required"}
```

---

### GET /api/groups/announcement?groupId=NNN

获取群公告。需要认证。

**Query 参数:**
- `groupId` -- 群组 ID（必填）

**Response (200):**
```json
{"success": true, "groupId": 1, "content": "公告内容", "updatedAt": 1712880000000}
```

**Error:**
```json
{"success": false, "message": "groupId required"}
{"success": false, "message": "no announcement"}
```

---

### POST /api/groups/kick

踢出群成员（仅群主）。需要认证。

**Request:**
```json
{
  "groupId": 1,
  "userId": 2
}
```

**Response (200):**
```json
{"success": true}
```

**Error:**
```json
{"success": false, "message": "only owner can kick members"}
{"success": false, "message": "cannot kick yourself"}
{"success": false, "message": "groupId and userId required"}
```

---

### GET /api/messages/read-status?peerId=NNN

查询与某用户的消息已读状态。需要认证。

**Query 参数:**
- `peerId` -- 对方用户 ID（必填）

**Response (200):**
```json
{
  "success": true,
  "peerId": 2,
  "lastReadMsgId": "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx",
  "lastReadTime": 1712880000000
}
```

**Error:**
```json
{"success": false, "message": "peerId required"}
```

---

### GET /health
健康检查（无需鉴权，部署探针用）

**Response 200 OK 或 503 Service Unavailable:**
```json
{
  "status": "ok|degraded",
  "mysql": "ok|down",
  "redis": "ok|down",
  "online_users": 42,
  "mysql_breaker": "closed|open|half-open",
  "redis_breaker": "closed|open|half-open"
}
```

### GET /metrics
Prometheus 监控指标（无需鉴权）

返回 Prometheus 文本格式：
```
# TYPE http_requests_total counter
http_requests_total 1234
http_requests_get 800
http_requests_post 434
```
