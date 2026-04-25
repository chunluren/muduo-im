# P1.2 Gateway/Logic gRPC 拆分蓝图

> Issue #3 / 2026-04-25
> 当前状态：**proto + CMake 骨架已落地，二进制实现待开发机装 `libgrpc++-dev` 后再启动。**

## 1. 背景

ChatServer 现在是单进程：WS 接入 + 业务路由 + DB/Redis 操作全在一个二进制里。
Plan B § 1.2 要求拆成：

```
Client ─ws─▶ muduo-im-gateway (无状态接入层) ─gRPC─▶ muduo-im-logic (业务层)
                                                     │
                                                ┌────┴────┐
                                                ▼         ▼
                                              MySQL    Redis
```

拆完之后：
- gateway 只关心连接 + 协议 + 简单鉴权，可水平扩到几十台
- logic 拿走所有业务逻辑，可独立扩缩容
- gateway↔logic 之间 bidi-streaming，logic 通过反向流主动推送下行

## 2. 当前已交付

| 文件 | 内容 |
|------|------|
| `proto/im/common.proto`   | Envelope / ClientIdentity / Status |
| `proto/im/logic.proto`    | LogicService（HandleMessage、HandleAuth、RegisterGateway 双向流）|
| `proto/im/registry.proto` | RegistryService（Register / KeepAlive / Watch）|
| `CMakeLists.txt`          | `option(WITH_GRPC OFF)` + protoc 生成规则 + gateway/logic 可执行文件占位 |

`protoc --cpp_out` 已通过 smoke test，`im_proto` 静态库目标已建（开 `-DWITH_GRPC=ON` 即编）。

## 3. 待实施（W1–W3，对应 issue #3 任务勾选）

### W1.D1–D2 ✅ proto + 代码生成 + CMake
本次提交完成。

### W1.D3–D5 muduo-im-logic 独立二进制
**新增目录：** `src/logic/`
**入口：** `src/logic/main.cpp`
- 复用现有 `MessageService` / `FriendService` / `GroupService` / `OnlineManager` / `ContentModerationService`
- 启动 gRPC server 监听 `logic.host:logic.port`（默认 0.0.0.0:9100）
- 实现 `LogicServiceImpl`：
  - `HandleMessage`：解 payload JSON → 走原 `handlePrivateMessage` / `handleGroupMessage` 等
  - `HandleAuth`：调 JwtUtils 验签返 ClientIdentity
  - `RegisterGateway`：维护 `gateway_id → grpc::ServerWriter<PushCommand>*` map；下行推送从这里写
- 在 `Register` 给 RegistryService（同进程或独立进程，初期同进程）

**风险：** `OnlineManager` 当前持有 `WebSocketSession*`，在 logic 里没有 ws 句柄。
**修法：** logic 侧 OnlineManager 只保存 `(uid, device_id) → gateway_id`；下行通过 RegisterGateway 反向流送 PushToConn。

### W2.D1–D3 muduo-im-gateway 独立二进制
**新增目录：** `src/gateway/`
- 入口 `src/gateway/main.cpp`：复用 mymuduo 的 WebSocketServer
- 持有 `LogicClientPool`：每个 logic 一个 `RegisterGateway` 长连接 stream
- WS onMessage：parse JSON → 选 logic（一致性 hash by uid）→ stream.Write(GatewayEvent::up_msg)
- LogicClient 收 PushCommand：根据 conn_id / uid 查本地连接表 → ws.send

### W2.D4–D5 双向流 + 自动重连
- gRPC keepalive 30s
- stream 断了：5 个 backoff（1s/2s/4s/8s/16s 上限 30s）
- 重连成功后立刻补发 ConnectionOpened 事件给 logic（让 logic 重建路由表）

### W3.D1–D2 Registry + 一致性 hash
- 单进程内存版 RegistryServiceImpl（`std::unordered_map<lease_id, Endpoint>` + lease TTL 线程）
- ConsistentHashRing：每个 endpoint 200 个虚拟节点
- gateway 启动时 List + Watch；hash by `routing_key = uid`

### W3.D3–D4 多实例集成测试
- compose 起 2 gateway + 2 logic + redis + mysql
- 跑 `tests/e2e_test.sh` 23 case 全绿
- ws_bench 压测：1k 并发、100 msg/s/conn，看 P99
- 验收：gRPC 单跳 P99 ≤ 5ms（issue #3 验收项）

### W3.D5 部署
- `deploy/grpc/docker-compose.yml`
- `deploy/grpc/RUNBOOK.md`：滚动重启 logic / gateway 步骤

## 4. 与 #4 跨实例路由的关系

#4（已合 main）走 Redis Pub/Sub，**不会被 #3 替换**：
- gRPC 拆分后 logic 之间仍可能需要互推（gateway A 上的 user → gateway B 上的 user，但落到不同 logic shard）
- 此时 Redis Pub/Sub 仍是兜底通道
- 等 #3 完成后可评估是否把 Pub/Sub 改成 logic→logic 直接 gRPC，但不是 P0

## 5. 本地启用 gRPC

```bash
sudo apt-get install libgrpc++-dev libprotobuf-dev protobuf-compiler-grpc
cd build && cmake -DWITH_GRPC=ON .. && make -j
```

## 6. 风险与决策

| 风险 | 缓解 |
|------|------|
| gRPC C++ 依赖较重，CI 构建时间 +5min | 仅在 release 流水线开 WITH_GRPC；PR 流水线保持 OFF |
| 双进程后调试链路变长 | 强制 `trace_id` 贯穿 Envelope，logic 日志带 trace 落 ELK |
| Logic 单点故障期间消息丢失 | gateway 端 5s 缓存最近上行消息，logic 重连后 replay；超时降级走 Redis Pub/Sub |
| 一致性 hash 加新 logic 时部分 user 路由迁移 | 用虚拟节点 200，迁移面积 ≤ 1/N；不做 hot user reroute（让其自然在新会话路由） |

## 7. 下一步

按 W1.D3 起步：在 `src/logic/main.cpp` 写 LogicServiceImpl 骨架，先让 `HandleMessage` unary 通起来（不先做 stream）。
