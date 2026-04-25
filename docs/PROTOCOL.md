# muduo-im WebSocket 协议契约

**版本**：v1（2026-04-22）
**状态**：Stable
**作者**：chunluren

---

## 0. 文档定位

本文档是 **muduo-im 客户端 ↔ 服务端 WebSocket 通信的唯一契约来源**。任何涉及 WS 协议字段的改动必须先更新本文档再改代码。

### 0.1 为什么要这份文档

历史教训（2026-04-21 P1.1 Snowflake 改造）：
- 后端把 msgId 从客户端 UUID 改为 Snowflake，但前端 `web/index.html` 仍用本地 UUID
- 协议字段语义无文档约束 → **撤回功能整个挂掉**
- Postmortem 见 commit `0aee39e`

**结论**：跨前后端的协议字段必须在此文档先冻结，再改代码两边对齐。

### 0.2 阅读对象

- 实现/修改后端 ChatServer 的人
- 维护前端 `web/index.html` 的人
- 写客户端 SDK（移动端 / 桌面端）的人
- Code Review 审计跨端字段一致性的人

### 0.3 变更流程

任何新增 / 修改 / 废弃字段都要：

1. **先更新此文档**：在本文档加新字段定义 + 改 §11 变更日志
2. **改后端 Protocol.h**
3. **改前端 / 客户端**
4. **写测试**：`tests/test_protocol_*.cpp` 或 E2E
5. **Code Review** 时审计本文档是否已同步

字段一旦发布，**不允许悄悄改语义**——只能：
- 加新字段（默认值兼容）
- 标记 deprecated → 一个 release 周期后删除

---

## 1. 通用约定

### 1.1 传输

- WebSocket 文本帧（opcode = 1）
- 单条消息一个 JSON 对象，不嵌套消息
- 编码 UTF-8

### 1.2 鉴权

WebSocket 握手 URL 必须带 `token` 参数：

```
ws://host:9090/ws?token=<JWT>
```

- token 是 `POST /api/login` 返回的 JWT
- 服务端在 onConnect 时验证 token；失败发 `error` 后断开
- token 过期 / 被 revoke 后服务端主动断连
- 未来扩展（P3.1 多端 session）：`?token=...&device_id=...&device_type=...`

### 1.3 消息分类

| 方向 | 含义 |
|------|------|
| **C→S** | 客户端主动发送 |
| **S→C** | 服务端主动推送 |
| **C↔S** | 双方都可发起，对方原样回应 |

### 1.4 字段命名规范

- 所有 JSON 字段用 `camelCase`
- ID 字段：服务端权威 ID 用 `xxxId`（如 `msgId` `userId`）
- 时间戳：`timestamp` 字段，毫秒级 Unix 时间戳，`int64`
- 用户 ID 在 from / to 里**用字符串**（避免 JS 整数精度问题）
- 其他 ID 字段（msgId / groupId）也用字符串
- 内部数值字段（如时间戳）保持 `int64`

### 1.5 错误处理

任何字段缺失 / 类型错误，服务端发 `error` 类型消息：

```json
{"type":"error","message":"missing 'to' or 'content'"}
```

不静默忽略。

### 1.6 协议版本

- 当前版本 v1，不在消息体内显式带版本号
- 未来 v2 协议如果与 v1 冲突，将通过 WebSocket 子协议（Sec-WebSocket-Protocol）协商

---

## 2. 消息类型总览

### 2.1 当前已实现（v1）

| `type` 值 | 方向 | 用途 | 实现状态 |
|-----------|------|------|----------|
| `msg` | C↔S | 私聊文本消息 | ✅ |
| `group_msg` | C↔S | 群聊文本消息 | ✅ |
| `file_msg` | C↔S | 文件 / 图片消息 | ✅ |
| `ack` | S→C | 服务端收到客户端消息的确认 | ✅ |
| `recall` | C↔S | 撤回消息 | ✅ |
| `read_ack` | C↔S | 已读回执 | ✅ |
| `typing` | C↔S | "正在输入"状态 | ✅ |
| `online` | S→C | 好友上线通知 | ✅ |
| `offline` | S→C | 好友离线通知 | ✅ |
| `unread_sync` | S→C | 用户上线时推送各对话未读数 | ✅ |
| `edit` | C↔S | 消息编辑（15 分钟内） | ✅ |
| `reaction` | C→S | 切换 emoji 表情反应 | ✅ |
| `reaction_update` | S→C | 反应字典变化推送 | ✅ |
| `error` | S→C | 错误响应 | ✅ |

### 2.2 计划中（Phase 2-4）

| `type` 值 | 方向 | 用途 | 关联任务 |
|-----------|------|------|----------|
| `delivered` | S→C | 消息送达对端的回执（双向 ACK） | #6 P2.1 |
| `client_ack` | C→S | 接收方确认消息已送达本地 | #6 P2.1 |
| `read_sync` | S→C | 多端已读同步 | #10 P3.2 |
| `reaction_update` | S→C | 表情反应变化推送 | #16 P4.5 |
| `device_kicked` | S→C | 被管理端 / 自己其他端踢下线 | #9 P3.1 |

### 2.3 ID 体系

| ID 字段 | 类型 | 含义 | 生成 |
|---------|------|------|------|
| `msgId` | string | **服务端权威消息 ID** | Snowflake，19 位数字字符串 |
| `clientMsgId` | string | 客户端临时 ID（幂等 + DOM 关联） | 客户端生成 UUID v4 |
| `userId` | string | 用户 ID | DB AUTO_INCREMENT，字符串化 |
| `groupId` | string | 群组 ID | DB AUTO_INCREMENT，字符串化 |
| `jti` | string | JWT 唯一 ID（黑名单用，不在 WS 消息体内） | UUID v4 |

**关键约定**：
- 客户端发送消息时**自己生成 `clientMsgId`**（UUID）作为本地标识
- 服务端收到后生成 Snowflake 作为 `msgId`，在 `ack` 里把两者都返回
- 客户端收到 ack 后，**用服务端 `msgId` 替换本地缓存和 DOM 节点的 `clientMsgId`**
- 所有后续操作（撤回 / 引用 / 编辑 / 反应）必须用服务端 `msgId`

---

## 3. 私聊消息（msg）

### 3.1 客户端发送（C→S）

```json
{
  "type": "msg",
  "to": "12345",
  "content": "hello",
  "msgId": "550e8400-e29b-41d4-a716-446655440000",
  "replyTo": "172263581197272608"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `type` | string | 是 | 固定 `"msg"` |
| `to` | string | 是 | 接收方 userId（字符串化） |
| `content` | string | 是 | 消息正文，长度 ≤ 10000 字符 |
| `msgId` | string | 是 | **客户端 UUID v4**，作为 clientMsgId 用 |
| `replyTo` | string | 否 | 引用回复的目标 msgId（服务端权威 ID） |

### 3.2 服务端转发给接收方（S→C）

```json
{
  "type": "msg",
  "msgId": "172263581197272608",
  "from": "11111",
  "to": "12345",
  "content": "hello",
  "timestamp": 1745000000000,
  "replyTo": "172263581197272608"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `type` | string | 是 | `"msg"` |
| `msgId` | string | 是 | **服务端 Snowflake ID**（替换客户端 UUID） |
| `from` | string | 是 | 发送方 userId |
| `to` | string | 是 | 接收方 userId |
| `content` | string | 是 | 消息正文 |
| `timestamp` | int64 | 是 | 服务端落库时间戳，毫秒 |
| `replyTo` | string | 否 | 引用消息的服务端 msgId |

### 3.3 业务规则

- 接收方在线 → 立即推送
- 接收方离线 → 增加未读计数（Redis HASH `unread:{toUserId}`），消息存 MySQL `private_messages` 表
- 服务端按 (sender_id, clientMsgId) UNIQUE 去重（INSERT IGNORE）

---

## 4. 群聊消息（group_msg）

### 4.1 客户端发送（C→S）

```json
{
  "type": "group_msg",
  "to": "1001",
  "content": "@张三 帮忙看一下",
  "msgId": "uuid-v4-string",
  "replyTo": "172263581197272608",
  "mentions": ["12345", "67890"]
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `type` | string | 是 | `"group_msg"` |
| `to` | string | 是 | 群组 ID |
| `content` | string | 是 | 消息正文 |
| `msgId` | string | 是 | 客户端 UUID |
| `replyTo` | string | 否 | 引用 msgId |
| `mentions` | array<string> | 否 | **被 @ 的 userId 列表**（计划 #8 P2.3） |

### 4.2 服务端转发（S→C）

```json
{
  "type": "group_msg",
  "msgId": "172263581197272608",
  "from": "11111",
  "to": "1001",
  "content": "@张三 帮忙看一下",
  "timestamp": 1745000000000,
  "replyTo": null,
  "mentions": ["12345"],
  "mention": true
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| ...（同 §4.1） | | | |
| `mention` | bool | 否 | **当前接收者是否被 @**（仅推给被 @ 用户时为 true） |

### 4.3 业务规则

- 读扩散：所有群成员查同一份 group_messages 表（写一份）
- 在线成员遍历推送（未来在 #3 P1.2 后改为 Logic 路由）
- `mentions` 字段（#8 P2.3）：被 @ 用户额外计入 `unread_mentions:{uid}` HASH

---

## 5. 服务端 ACK（ack）

### 5.1 服务端 → 发送方（S→C）

```json
{
  "type": "ack",
  "msgId": "172263581197272608",
  "clientMsgId": "550e8400-e29b-41d4-a716-446655440000"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `type` | string | 是 | `"ack"` |
| `msgId` | string | 是 | **服务端 Snowflake ID** |
| `clientMsgId` | string | 否 | 客户端原始 UUID（用于前端 DOM 关联） |

### 5.2 客户端处理逻辑（**重要**）

收到 ack 后必须：

1. 在本地消息缓存找到 `clientMsgId === msg.msgId` 的记录
2. 把它的 `msgId` 替换为服务端 `data.msgId`
3. 同步更新 DOM 节点的 `data-msg-id` 属性
4. 同步更新该消息上的撤回 / 引用 / 编辑按钮的 onclick 中的 msgId

```javascript
// web/index.html ack handler 实现
case 'ack': {
  if (data.clientMsgId && data.msgId && data.clientMsgId !== data.msgId) {
    const tmpId = data.clientMsgId;
    const realId = data.msgId;
    // 更新缓存
    for (const key in messageCache) {
      const arr = messageCache[key];
      for (const m of arr) {
        if (m.msgId === tmpId) m.msgId = realId;
        if (m.replyTo === tmpId) m.replyTo = realId;
      }
    }
    // 更新 DOM
    const domNode = document.querySelector(`[data-msg-id="${tmpId}"]`);
    if (domNode) {
      domNode.setAttribute('data-msg-id', realId);
      const recallBtn = domNode.querySelector('.btn-recall');
      if (recallBtn) recallBtn.setAttribute('onclick', `recallMessage('${realId}')`);
      const replyBtn = domNode.querySelector('.btn-reply');
      if (replyBtn) {
        const oldOnclick = replyBtn.getAttribute('onclick') || '';
        replyBtn.setAttribute('onclick', oldOnclick.replace(`'${tmpId}'`, `'${realId}'`));
      }
    }
  }
  break;
}
```

**未来扩展（#6 P2.1）**：ACK 仅表示服务端收到。「送达对端」用单独的 `delivered` 类型。

---

## 6. 文件消息（file_msg）

### 6.1 客户端发送

文件先通过 `POST /api/upload` HTTP 上传（multipart），拿到 URL 后通过 WS 发：

```json
{
  "type": "file_msg",
  "to": "12345",
  "url": "/uploads/abc.jpg",
  "filename": "photo.jpg",
  "fileSize": 102400,
  "msgId": "client-uuid"
}
```

### 6.2 服务端转发

```json
{
  "type": "file_msg",
  "msgId": "172263581197272608",
  "from": "11111",
  "to": "12345",
  "url": "/uploads/abc.jpg",
  "filename": "photo.jpg",
  "fileSize": 102400,
  "timestamp": 1745000000000
}
```

### 6.3 限制

- 单文件 ≤ 50MB
- 类型白名单（jpg/png/gif/webp/mp4/pdf/zip/...，详见 ChatServer::handleUpload）
- 未来 P4.2 加缩略图字段：`fileThumb200`、`fileThumb600`
- 未来 P4.3 断点续传走独立的 `/api/upload/init|chunk|status|complete` 4 个 API

---

## 7. 撤回（recall）

### 7.1 客户端发起（C→S）

```json
{"type":"recall","msgId":"172263581197272608"}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `msgId` | string | 是 | **服务端 Snowflake ID**（不是 clientMsgId！） |

### 7.2 服务端通知发送方 + 接收方（S→C）

```json
{"type":"recall","msgId":"172263581197272608","from":"11111"}
```

### 7.3 业务规则

- 只能撤回自己发的
- 时间窗：2 分钟内（120000 ms）
- 软删除（`recalled=1` 字段），保留审计
- 调用前先 `flushMessageQueue()` 把 Redis 队列刷到 MySQL，避免新消息还没入库

---

## 7.5 消息编辑（edit · Phase 4.1）

### 7.5.1 客户端发起（C→S）

```json
{"type":"edit","msgId":"172263581197272608","newBody":"修改后的内容"}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `msgId` | string | 是 | **服务端 Snowflake msgId**（不是 clientMsgId） |
| `newBody` | string | 是 | 编辑后内容，长度 ≤ 10000 |

### 7.5.2 服务端推送（S→C，发给会话相关方）

```json
{
  "type": "edit",
  "msgId": "172263581197272608",
  "newBody": "修改后的内容",
  "editedAt": 1745000000000,
  "from": "11111",
  "convType": "private",
  "convId": "12345"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `editedAt` | int64 | 编辑时间戳（毫秒），客户端用作"已编辑"展示时间 |
| `convType` | string | "private" / "group" |
| `convId` | string | 私聊对方 userId / 群 ID |

### 7.5.3 业务规则

- **必须是发送者本人**（数据库 from_user 校验）
- **15 分钟时间窗**（900_000 ms），超时返回 error
- **已撤回的消息不可编辑**（recalled=1 检查）
- **首次编辑保留 original_body**，后续编辑 不再覆盖（保留首版）
- **每次编辑写入 message_edits 审计表**（含 old_body / new_body / edited_at）
- **事务保证**：UPDATE messages + INSERT message_edits 在同一事务（任一失败回滚）

### 7.5.4 实现位置

- 后端：`MessageService::editMessage`、`ChatServer::handleEdit`、`Protocol::makeEdit`
- 前端：`web/index.html` 内 `editMessage()`、`case 'edit':` handler、`.btn-edit` / `.edited-tag` CSS

---

## 7.6 表情反应（reaction / reaction_update · Phase 4.5）

### 7.6.1 客户端切换（C→S）

```json
{"type":"reaction","msgId":"172263581197272608","emoji":"👍"}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `msgId` | string | 是 | 服务端 Snowflake msgId |
| `emoji` | string | 是 | UTF-8 emoji（≤ 16 字节） |

服务端语义：(msgId, uid, emoji) UNIQUE，已存在则**删除**（取消反应），不存在则**插入**（添加反应）。同一用户对同一消息可同时使用多个不同 emoji。

### 7.6.2 服务端推送（S→C）

```json
{
  "type": "reaction_update",
  "msgId": "172263581197272608",
  "reactions": {
    "👍": ["11", "22"],
    "❤️": ["33"]
  },
  "convType": "private",
  "convId": "12345"
}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `reactions` | object | **完整字典**：emoji → 反应过的 uid 数组 |
| `convType` | string | "private" / "group" |
| `convId` | string | 私聊对方 userId / 群 ID |

**关键约定**：每次推送都是**完整字典**（不是增量），客户端整体替换显示。

### 7.6.3 业务规则

- **任何会话成员**都能 reaction（不限发送者）
- **emoji 任意选择**（前端预设 8 个常用：👍❤️😂😮😢🎉🔥👏，但服务端不限制内容）
- **(msgId, uid, emoji) UNIQUE**，重复请求等价于切换（取消）
- **反应不持久化时间戳的语义**——只关心当前是否反应（旧反应不展示历史）
- 推送给会话相关方：私聊推对方；群聊推所有在线成员（含发起者用于本端 UI 立即同步）
- 消息已撤回 / 已删除时，前端不显示 reaction 按钮；后端仍可处理（DB 行存在）

### 7.6.4 实现位置

- 后端：`MessageService::toggleReaction` / `getReactions`、`ChatServer::handleReaction`、`Protocol::makeReactionUpdate`
- 前端：`web/index.html` 内 `showReactionPicker`、`toggleReaction`、`case 'reaction_update':` handler、`.reactions-bar` / `.reaction-chip` CSS

---

## 8. 已读回执（read_ack）

### 8.1 客户端上报（C→S）

```json
{
  "type": "read_ack",
  "to": "12345",
  "lastMsgId": "172263581197272608"
}
```

| 字段 | 类型 | 必填 | 说明 |
|------|------|------|------|
| `to` | string | 是 | 对方 userId |
| `lastMsgId` | string | 是 | 已读到此 msgId 为止的所有消息 |

### 8.2 服务端转发给对方（S→C）

```json
{"type":"read_ack","from":"12345","lastMsgId":"172263581197272608"}
```

### 8.3 业务规则

- 服务端清空 `unread:{from}.{to}` 计数
- read_receipts 表持久化（Redis 缓存最近 100 条）
- **未来 P3.2**：同时推送 `read_sync` 给同 uid 其他设备

---

## 9. 其他消息类型

### 9.1 typing（输入提示）

C→S：
```json
{"type":"typing","to":"12345"}
```

S→C：
```json
{"type":"typing","from":"11111","to":"12345"}
```

不持久化，仅转发给对方。

### 9.2 online / offline（在线状态）

S→C：
```json
{"type":"online","userId":"11111"}
{"type":"offline","userId":"11111"}
```

服务端在用户上线 / 下线时推给该用户的所有好友。

### 9.3 unread_sync（未读同步）

S→C，仅在用户 WebSocket 连接建立时推送一次：
```json
{
  "type": "unread_sync",
  "unread": {
    "12345": 3,
    "67890": 1
  }
}
```

key = 对方 userId，value = 未读数。

### 9.4 error（错误）

S→C：
```json
{"type":"error","message":"unknown message type"}
```

任何处理失败都返回这种结构。客户端展示 `message` 字段给用户。

---

## 10. 计划中的协议（占位）

> 这些类型未实现，本节作为设计预留，实现时回填详细字段。

### 10.1 delivered（双向 ACK · 计划 #6 P2.1）

服务端确认对端已收到：

```json
{"type":"delivered","msgId":"...","deliveredAt":1745000000000}
```

接收方主动 ack：

```json
{"type":"client_ack","msgId":"..."}
```

### 10.2 read_sync（多端已读同步 · 计划 #10 P3.2）

```json
{
  "type": "read_sync",
  "convType": "private",
  "convId": "12345",
  "lastMsgId": "..."
}
```

### 10.3 edit（消息编辑 · #12 P4.1 已实现，见 §7.5）

### 10.4 reaction / reaction_update（已实现，见 §7.6）

### 10.5 device_kicked（多端踢下线 · 计划 #9 P3.1）

S→C：
```json
{"type":"device_kicked","reason":"replaced_by_other_device","deviceId":"mobile-abc"}
```

收到后客户端主动断开 WebSocket。

---

## 11. 变更日志

| 日期 | 变更 | 关联 PR |
|------|------|---------|
| 2026-04-25 | 新增 `reaction` / `reaction_update` 类型 — 表情反应 | #16 |
| 2026-04-25 | 新增 `edit` 类型（C↔S）— 消息编辑 15 分钟时间窗 | #12 |
| 2026-04-22 | 创建本文档；冻结 v1 协议（11 类型）；明确双 msgId 约定 | (W1 路线图) |
| 2026-04-21 | ack 加 `clientMsgId` 透传字段（修复 Recall bug） | `0aee39e` |
| 2026-04-21 | msgId 由 UUID 改为 Snowflake（仅服务端权威） | `74e06fa` |
| 2026-04-21 | 加 jti claim（不在 WS 体内但影响 token） | `925001f` |

---

## 12. 实现位置索引

| 协议构造 | 后端文件 |
|---------|---------|
| 所有消息类型常量 | `src/common/Protocol.h` |
| make* 构造函数 | `src/common/Protocol.h` |
| WS 入站分发 | `src/server/ChatServer.h::wsServer_.setMessageHandler()` |
| 私聊 handlePrivateMessage | `src/server/ChatServer.h::handlePrivateMessage()` |
| 群聊 handleGroupMessage | `src/server/ChatServer.h::handleGroupMessage()` |
| 撤回 handleRecall | `src/server/ChatServer.h::handleRecall()` |
| 已读 handleReadAck | `src/server/ChatServer.h::handleReadAck()` |

| 前端 | 文件 |
|------|------|
| WS 连接 + handleWsMessage | `web/index.html::connectWebSocket()` / `handleWsMessage()` |
| ack 处理 | `web/index.html` 内 `case 'ack':` 块 |
| 消息构造 | `web/index.html::sendMessage()` |

---

## 13. Code Review 协议字段 Checklist

接到涉及 WS 协议的 PR 时，按此 checklist 审查：

- [ ] **本文档已更新**？（最重要！没更新文档的协议 PR 一律 reject）
- [ ] 后端 `Protocol.h` 已加 / 改字段
- [ ] 前端 `web/index.html` 已加 / 改字段（如适用）
- [ ] 字段命名符合 §1.4（camelCase / 字符串 ID）
- [ ] 增加新字段时其他无关业务不受影响（向前兼容）
- [ ] 增加新消息类型时已加 `error` 路径（type 不识别时）
- [ ] 单元测试 / E2E 覆盖新字段
- [ ] §11 变更日志已记录
- [ ] §12 实现位置索引已更新

---

## 14. FAQ

**Q1：`msgId` 为什么是字符串不是数字？**
A：JavaScript 的 `Number` 是 IEEE 754 双精度浮点，安全整数上限 `2^53-1 ≈ 9e15`。Snowflake ID 是 64-bit 整数，超出此范围会丢失精度。所以传输时统一字符串化。

**Q2：客户端为什么要自己生成 clientMsgId？**
A：用于幂等（重传去重）+ 本地 DOM 关联（在 ack 到来前有个临时标识）。如果服务端单独生成，客户端没法在 ack 前定位本地消息。

**Q3：旧客户端不发 clientMsgId 会怎样？**
A：服务端兼容——`clientMsgId` 字段为空时，去重 key 退化为服务端 Snowflake msgId，前端如果不更新 DOM 也只是失去撤回能力（但消息收发正常）。

**Q4：错误消息为什么不带 errorCode？**
A：v1 简化设计，只有 message 字符串。未来 v2 可考虑加 code 字段做国际化 + 程序化错误处理。

**Q5：为什么用 JSON 不用 protobuf？**
A：v1 优先简单 + 可调试（curl / DevTools 直接读）。未来 #3 P1.2 拆 gRPC 时**服务端 ↔ 服务端**用 protobuf，但 **客户端 ↔ Gateway** 仍保持 JSON over WebSocket（浏览器原生支持）。

---

**END**
