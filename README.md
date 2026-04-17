# muduo-im

基于 mymuduo-http 网络框架的即时通信系统。

## 功能

- 用户注册/登录（JWT 认证）
- 查看/修改个人资料、查看他人资料
- 修改密码、注销账号
- 好友申请流程（申请/同意/拒绝/双向添加/删除/列表）
- 群组管理（创建/加入/退出/解散/成员列表）
- 实时私聊 + 群聊（WebSocket）
- 正在输入提示（typing indicator）
- 消息持久化 + 历史查询（MySQL）
- 消息长度校验（最大 10000 字符）
- 在线状态管理 + 未读消息计数
- 文件传输（HTTP 上传 + WebSocket 通知，50MB 限制 + 类型白名单）
- 消息撤回（2 分钟内，软删除 recalled 字段）
- 已读回执
- Redis 降级（Redis 不可用时直接写 MySQL 回退）
- 配置文件支持（config.ini 外部配置）
- 消息搜索（关键词搜索聊天记录）
- 表情包选择器
- 图片消息内联预览
- 消息引用/回复（replyTo 字段）
- 群公告（群主设置/获取）
- 群管理（踢人）
- 离线未读同步
- 消息幂等投递（去重）
- HTTP 请求限流
- C++ WebSocket 压测客户端
- Web 前端（单文件 HTML）
- ACID 事务保证（多表操作原子性）
- 外键约束 + 级联删除
- Redis AOF 持久化（文档：docs/REDIS_CONFIG.md）
- TransactionGuard RAII 事务守卫
- `/health` 健康检查端点
- `/metrics` Prometheus 监控指标
- WebSocket 心跳保活（30s ping，60s 空闲断开）
- 优雅关闭（SIGTERM 刷队列后退出）
- 单条消息隔离（消息队列刷写时单条失败不影响批次）

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
├── benchmark/
│   └── ws_bench.cpp           # C++ WebSocket 压测客户端
├── tests/
│   ├── e2e_test.sh             # 端到端集成测试
│   ├── test_helper.h           # 单元测试共享 MySQL 固件
│   ├── test_user_service.cpp   # UserService 单元测试
│   ├── test_friend_service.cpp # FriendService 单元测试
│   ├── test_group_service.cpp  # GroupService 单元测试
│   └── test_message_service.cpp # MessageService 单元测试
├── config.ini                 # 运行时配置文件（MySQL/Redis/JWT 等）
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

3. 配置（可选）
   ```
   cp config.ini.example config.ini   # 按需修改 MySQL/Redis/JWT 等参数
   ```

4. 运行
   ```
   ./muduo-im                          # 自动加载同目录下 config.ini（如存在）
   ```

5. 访问
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
| /api/user/info | GET | 查看他人资料 |
| /api/user/password | POST | 修改密码 |
| /api/user/delete | POST | 注销账号 |
| /api/user/profile | GET | 查看自己资料 |
| /api/user/profile | PUT | 修改资料 |
| /api/user/search | GET | 搜索用户 |
| /api/friends | GET | 好友列表 |
| /api/friends/request | POST | 发送好友申请 |
| /api/friends/requests | GET | 好友申请列表 |
| /api/friends/handle | POST | 处理好友申请 |
| /api/friends/delete | POST | 删除好友 |
| /api/groups | GET | 群列表 |
| /api/groups/create | POST | 创建群 |
| /api/groups/join | POST | 加入群 |
| /api/groups/leave | POST | 退出群组 |
| /api/groups/delete | POST | 解散群组（仅群主） |
| /api/groups/members | GET | 群成员 |
| /api/messages/history | GET | 历史消息 |
| /api/unread | GET | 未读消息计数 |
| /api/upload | POST | 文件上传（multipart） |
| /api/messages/search | GET | 搜索消息 |
| /api/groups/announcement | POST | 设置群公告 |
| /api/groups/announcement | GET | 获取群公告 |
| /api/groups/kick | POST | 踢出群成员 |
| /api/messages/read-status | GET | 已读状态查询 |

### WebSocket 协议

连接: `ws://localhost:9090/ws?token=JWT_TOKEN`

客户端发送:
```json
{"type":"msg", "to":"userId", "content":"hello", "msgId":"uuid"}
{"type":"group_msg", "to":"groupId", "content":"hello", "msgId":"uuid"}
{"type":"file_msg", "to":"userId", "url":"/uploads/xxx", "filename":"test.pdf", "fileSize":1024}
{"type":"recall", "msgId":"uuid"}
{"type":"read_ack", "to":"senderId", "lastMsgId":"uuid"}
{"type":"typing", "to":"userId"}
{"type":"unread_sync", "data":{"userId":count}}
```

消息可携带 `"replyTo":"msgId"` 字段实现引用/回复。

服务端推送:
```json
{"type":"msg", "from":"userId", "content":"hello", "msgId":"uuid", "timestamp":123}
{"type":"ack", "msgId":"uuid"}
{"type":"online", "userId":"xxx"}
{"type":"offline", "userId":"xxx"}
{"type":"file_msg", "from":"userId", "url":"/uploads/xxx", "filename":"test.pdf", "fileSize":1024, "msgId":"uuid"}
{"type":"recall", "msgId":"uuid", "from":"userId"}
{"type":"read_ack", "from":"readerId", "to":"senderId", "lastMsgId":"uuid"}
{"type":"typing", "from":"userId"}
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

### C++ WebSocket 压测客户端
```bash
cd build && ./ws_bench [并发数] [每客户端消息数]
```

### 单元测试

cd build && make test_user_service test_friend_service test_group_service test_message_service
./test_user_service && ./test_friend_service && ./test_group_service && ./test_message_service

需要先创建测试数据库:
sudo mysql -e "CREATE DATABASE IF NOT EXISTS muduo_im_test DEFAULT CHARACTER SET utf8mb4;"
sudo mysql muduo_im_test < sql/init.sql
