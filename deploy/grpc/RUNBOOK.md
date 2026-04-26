# Phase 1.2 gRPC 拓扑 Runbook

> Issue #3 W3.D5 — gateway/logic/registry 拓扑的部署、运维、故障排查速查

## 1. 拓扑图

```
┌──────────────┐
│  ws clients  │
└──────┬───────┘
       │ ws://...:9091   ws://...:9092
       ▼                   ▼
┌────────────┐       ┌────────────┐
│ gateway-a  │       │ gateway-b  │
└──────┬─────┘       └──────┬─────┘
       │ gRPC unary + bidi-stream (RegisterGateway)
       ├───────────────────┐
       ▼                   ▼
┌────────────┐       ┌────────────┐
│  logic-a   │       │  logic-b   │
└──────┬─────┘       └──────┬─────┘
       │ MySQL / Redis       │
       └─────────┬───────────┘
                 ▼
        ┌──────────────────┐
        │  mysql + redis   │
        └──────────────────┘

      ┌───────────────────────┐
      │  registry (single)    │  ← logic Register/KeepAlive；gateway List/Watch
      └───────────────────────┘
```

## 2. 起步

```bash
# 在仓库根
docker compose -f deploy/grpc/docker-compose.yml up --build -d
docker compose -f deploy/grpc/docker-compose.yml ps
```

健康验证：

```bash
# registry 看到两个 logic
docker compose -f deploy/grpc/docker-compose.yml logs registry | grep register
# 期望：register logic/logic-A、register logic/logic-B

# gateway 加载到两个 logic
docker compose -f deploy/grpc/docker-compose.yml logs gateway-a | grep '\[pool\]'
# 期望：[pool] add logic instance=logic-A ... 和 logic-B
```

E2E：

```bash
# 把 build/ 目录的客户端测试脚本指向 docker 端口
GW_A=ws://127.0.0.1:9091/ws GW_B=ws://127.0.0.1:9092/ws \
  python3 tests/grpc/topology_e2e.py
```

## 3. 滚动操作

### 3.1 替换/重启某个 logic

```bash
# 新加 logic-c
docker compose -f deploy/grpc/docker-compose.yml up -d --scale logic-a=1 logic-c
# 此时 registry 收到 ADDED 事件 → gateway pool 自动 addEndpoint，
# 一致性 hash 环加入 logic-c，~1/(N+1) 的 routing key 迁到 logic-c。
# 客户端连接不断（gateway 持流），下行流通过 broadcast(ConnOpen) 让 logic-c
# 也能 push。

# 优雅退役 logic-a
docker compose -f deploy/grpc/docker-compose.yml stop logic-a
# logic main.cpp 退出前会向 registry Deregister → REMOVED 事件 → gateway
# 删 client + ring 节点。当前正在 logic-a 上的 unary HandleMessage 会以
# UNAVAILABLE 失败一次（gateway 回 client {"type":"error","code":"logic_unavailable"}）；
# 客户端重发即可命中其他 logic。
```

### 3.2 替换/重启某个 gateway

```bash
docker compose -f deploy/grpc/docker-compose.yml restart gateway-b
# RegisterGateway 流被 reset → logic 端 GatewayRegistry 在流结束时 deregisterGateway
# 清理这条 gateway 上所有 uid 的 presence。
# gateway-b 重启后会重新 RegisterGateway，client 重新连接 ws 后 ConnOpen 重建 presence。
```

## 4. 故障排查

| 症状 | 原因 | 处理 |
|------|------|------|
| `[pool] no logic instance` 频繁 | registry 起得比 logic 慢 | 等 1-2s；或调大 `depends_on` 顺序 |
| logic 日志反复 `register failed, retry` | registry 不可达 | 检查 `MUDUO_IM_REGISTRY_ADDR` 与 service 名一致；`docker compose ps` 看 registry 状态 |
| client 收得到 ack，收不到对端的 msg | gateway 与 logic 的 RegisterGateway 流断了 | gateway 日志找 `stream Read loop ended`；logic 重启或 OOM 后 1-30s 自动重连 |
| 全部失败 + `incompatible with your Protocol Buffer headers` | `/usr/local` 还有 protobuf 3.11 残留 | 重 build 镜像；运行时镜像是干净 22.04，应当不会出现 |
| 部分 uid 被持续路由到挂掉的 logic | gateway 的 watch 流断了，没收到 REMOVED | 检查 `[pool] registry Watch ended` 日志；正常会 2s 后重连 |

## 5. 已知简化项（生产前要补）

- registry 是单点 + 内存（断电丢全表）。生产替换 etcd。
- KeepAlive 不强校验 TTL，logic 崩溃后 registry 不会自动剔除（需要 logic 进程退出时主动 Deregister，OOM kill 场景会留遗骸）。补：在 registry 上加一个 lease 过期 GC。
- gateway → logic 上行 RPC 没有重试/熔断；UNAVAILABLE 直接回 client error。补：加 1 次幂等重试（看 gRPC `retry_policy`）。
- 没有任何 TLS。生产前两件事：（1）gateway↔logic 用 mTLS；（2）ws 升级 wss（已有 TLS 中间件，迁过来即可）。
- 没有度量。建议：在 LogicClient / LogicClientPool 各方法加 prometheus 计数和直方图。
