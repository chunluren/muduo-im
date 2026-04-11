# muduo-im

基于 mymuduo-http 网络框架的即时通信系统。

## 功能

- 用户注册/登录（JWT 认证）
- 好友管理（添加/删除/列表）
- 群组管理（创建/加入/成员列表）
- 实时私聊 + 群聊（WebSocket）
- 消息持久化 + 历史查询（MySQL）
- 在线状态管理
- 文件传输（HTTP 上传 + WebSocket 通知）
- 消息撤回（2 分钟内）
- 已读回执
- Web 前端（单文件 HTML）

## 技术栈

| 层 | 技术 |
|----|------|
| 接入层 | mymuduo-http (HTTP + WebSocket) |
| 业务层 | C++17 (UserService / ChatServer / MessageService) |
| 存储 | MySQL (用户/消息/好友/群组) |
| 缓存 | Redis (在线状态/消息队列) |
| 认证 | JWT (HMAC-SHA256) |
| 前端 | HTML + CSS + JS (原生 WebSocket + fetch) |

## 项目结构

```
muduo-im/
├── third_party/mymuduo-http/  # 网络框架 (submodule)
├── src/
│   ├── common/
│   │   ├── JWT.h              # JWT 生成/验证
│   │   └── Protocol.h         # 消息协议 + 密码哈希
│   └── server/
│       ├── ChatServer.h       # 主服务 (HTTP API + WebSocket)
│       ├── UserService.h      # 注册/登录
│       ├── MessageService.h   # 消息存储/历史
│       ├── FriendService.h    # 好友管理
│       ├── GroupService.h     # 群组管理
│       ├── OnlineManager.h    # 在线状态
│       └── main.cpp
├── web/
│   └── index.html             # Web 前端
├── sql/
│   └── init.sql               # 建表脚本
└── CMakeLists.txt
```

## 构建 & 运行

### 前提

- MySQL 服务运行中
- Redis 服务运行中
- 依赖: libmysqlclient-dev, libhiredis-dev, OpenSSL, zlib, Protobuf

### 步骤

1. 建表
   ```
   mysql -u root < sql/init.sql
   ```

2. 编译
   ```
   mkdir build && cd build
   cmake .. && make -j$(nproc)
   ```

3. 运行
   ```
   ./muduo-im
   ```

4. 访问
   ```
   浏览器打开 http://localhost:8080/index.html
   ```

### 端口

- HTTP API: 8080
- WebSocket: 9090

## API

### REST API

| 接口 | 方法 | 说明 |
|------|------|------|
| /api/register | POST | 注册 |
| /api/login | POST | 登录 → JWT |
| /api/friends | GET | 好友列表 |
| /api/friends/add | POST | 添加好友 |
| /api/friends/delete | POST | 删除好友 |
| /api/groups | GET | 群列表 |
| /api/groups/create | POST | 创建群 |
| /api/groups/join | POST | 加入群 |
| /api/groups/members | GET | 群成员 |
| /api/messages/history | GET | 历史消息 |
| /api/upload | POST | 文件上传（multipart） |

### WebSocket 协议

连接: `ws://localhost:9090/ws?token=JWT_TOKEN`

客户端发送:
```json
{"type":"msg", "to":"userId", "content":"hello", "msgId":"uuid"}
{"type":"group_msg", "to":"groupId", "content":"hello", "msgId":"uuid"}
{"type":"file_msg", "to":"userId", "url":"/uploads/xxx", "filename":"test.pdf", "fileSize":1024}
{"type":"recall", "msgId":"uuid"}
{"type":"read_ack", "to":"senderId", "lastMsgId":"uuid"}
```

服务端推送:
```json
{"type":"msg", "from":"userId", "content":"hello", "msgId":"uuid", "timestamp":123}
{"type":"ack", "msgId":"uuid"}
{"type":"online", "userId":"xxx"}
{"type":"offline", "userId":"xxx"}
{"type":"file_msg", "from":"userId", "url":"/uploads/xxx", "filename":"test.pdf", "fileSize":1024, "msgId":"uuid"}
{"type":"recall", "msgId":"uuid", "from":"userId"}
{"type":"read_ack", "from":"readerId", "to":"senderId", "lastMsgId":"uuid"}
```

## 测试

### 端到端测试
```bash
./tests/e2e_test.sh
```

### WebSocket 压力测试
```bash
python3 benchmark/ws_benchmark.py [客户端数] [每客户端消息数]
python3 benchmark/ws_benchmark.py 10 100   # 10客户端 × 100消息
```
