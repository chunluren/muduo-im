# Phase 3 etcd 服务发现 Runbook

> Phase 3E — gateway/logic 用 etcd 替代单点 RegistryService 后的部署、运维、故障排查速查。
>
> 与 `deploy/grpc/RUNBOOK.md`（基于 RegistryService gRPC）完全互斥但功能等价：
> 加 `MUDUO_IM_USE_ETCD=1` 切到本路径；不加就保持老行为。

## 1. 拓扑

```
┌──────────────┐
│  ws clients  │
└──────┬───────┘
       │ ws://...:9091   ws://...:9192
       ▼                    ▼
┌────────────┐        ┌────────────┐
│ gateway-a  │ ─pollOnce(1s)─▶ etcd ◀─keepAlive(5s)─ logic-a
└──────┬─────┘        └──────┬─────┘                logic-b
       │ gRPC unary +        │
       │ bidi-stream (RegisterGateway)
       ▼                     ▼
┌────────────┐        ┌────────────┐
│  logic-a   │        │  logic-b   │
└──────┬─────┘        └──────┬─────┘
       │ MySQL / Redis        │
       └─────────┬────────────┘
                 ▼
        ┌──────────────────┐
        │  mysql + redis   │
        └──────────────────┘

        ┌────────────────────┐
        │ etcd (3.3+ HTTP    │  ← logic put services/logic/{id} + lease 15s
        │ gateway, port      │     keepAlive 每 5s
        │ 2379)              │  ← gateway pollOnce prefix=services/logic/ 每 1s
        └────────────────────┘
```

**关键 KV 协议**（etcd v3 HTTP gateway，base64 字段值）：

```
key:    services/logic/{instance_id}
value:  {"service":"logic","addr":"127.0.0.1:9100","weight":1,"instance":"logic-A"}
lease:  15s（每 5s keepAlive，连续 3 次失败 → re-grant + put）
```

## 2. 起步

### 2.1 单机本地（开发）

需要 etcd 二进制（Ubuntu 22.04 自带 3.3.25：`apt install etcd`）：

```bash
# 起单节点 etcd（KRaft 模式，关 v2）
etcd --name node1 --data-dir /tmp/etcd-data \
     --listen-client-urls http://127.0.0.1:2379 \
     --advertise-client-urls http://127.0.0.1:2379 \
     --listen-peer-urls http://127.0.0.1:2380 \
     --initial-advertise-peer-urls http://127.0.0.1:2380 \
     --initial-cluster node1=http://127.0.0.1:2380 \
     --initial-cluster-state new --enable-v2=false &

# 起 logic + gateway（不需要 muduo-im-registry 进程）
MUDUO_IM_USE_ETCD=1 \
  ./build/muduo-im-logic 0.0.0.0:9100 &
MUDUO_IM_USE_ETCD=1 MUDUO_IM_LOGIC_INSTANCE_ID=logic-B \
  ./build/muduo-im-logic 0.0.0.0:9101 &

MUDUO_IM_USE_ETCD=1 \
  ./build/muduo-im-gateway 9091 &
MUDUO_IM_USE_ETCD=1 \
  ./build/muduo-im-gateway 9192 &
```

预期日志：

```
# logic
[logic] etcd registered key=services/logic/logic-A advertise=127.0.0.1:9100 lease=...
# gateway
[gateway] using etcd 127.0.0.1:2379 prefix=services/logic/
[pool] add logic instance=logic-A addr=127.0.0.1:9100 (size=1)
[pool] add logic instance=logic-B addr=127.0.0.1:9101 (size=2)
```

### 2.2 e2e 自动化

```bash
# 已编出 muduo-im-{logic,gateway}：
./tests/etcd/topology_e2e.sh
```

通过的输出（节选）：

```
[topology] etcd KV (services/logic/):
  services/logic/logic-A -> {"addr":"127.0.0.1:9100",...}
  services/logic/logic-B -> {"addr":"127.0.0.1:9101",...}
[e2e] OK: 200 messages in 0.66s (303 msg/s aggregate)
[e2e] ack-latency  ... p99=44.90 max=45.82
[e2e] recv-latency ... p99=44.94 max=45.85
```

## 3. 环境变量速查

| 变量 | 默认 | 作用 | 谁读 |
|---|---|---|---|
| `MUDUO_IM_USE_ETCD` | unset → 走 RegistryService | 设 `1` 切 etcd 路径 | logic + gateway |
| `MUDUO_IM_ETCD_HOST` | `127.0.0.1` | etcd 地址 | logic + gateway |
| `MUDUO_IM_ETCD_PORT` | `2379` | etcd 端口 | logic + gateway |
| `MUDUO_IM_ETCD_PREFIX` | `/v3beta` | API 前缀（v3.4+ 用 `/v3`） | logic + gateway |
| `MUDUO_IM_LOGIC_KEY_PREFIX` | `services/logic/` | etcd key 前缀 | gateway |
| `MUDUO_IM_LOGIC_INSTANCE_ID` | `logic-{pid}` | etcd key 后缀 + 注册体里的 instance | logic |
| `MUDUO_IM_LOGIC_ADVERTISE_HOST` | `127.0.0.1` | 替换监听 `0.0.0.0` 给别人看 | logic |

> ⚠️ `MUDUO_IM_REGISTRY_ADDR` 和 `MUDUO_IM_USE_ETCD` 互斥：USE_ETCD 优先，
> 有 USE_ETCD 时 REGISTRY_ADDR 完全被忽略。

## 4. 故障排查

### 4.1 gateway 看不到任何 logic

```
[gateway] using etcd 127.0.0.1:2379 prefix=services/logic/
# 没有 [pool] add logic ...
```

逐项排查：

```bash
# 1. etcd 活着吗
curl -s -X POST http://127.0.0.1:2379/v3beta/maintenance/status -d '{}'

# 2. logic 真的注册了吗（看 KV）
curl -s -X POST http://127.0.0.1:2379/v3beta/kv/range \
  -d "{\"key\":\"$(printf 'services/logic/' | base64)\",\"range_end\":\"$(printf 'services/logic0' | base64)\"}" \
  | python3 -c 'import sys,json,base64
j=json.load(sys.stdin)
[print(base64.b64decode(kv["key"]).decode(),"->",base64.b64decode(kv["value"]).decode()) for kv in j.get("kvs",[])]'

# 3. logic 那边的注册日志
grep "etcd registered\|etcd register failed" logs/logic-*.log
```

常见原因：
- logic 没拿 `MUDUO_IM_USE_ETCD=1` → 走的还是老 gRPC 路径，看不到 `[logic] etcd registered` 行
- prefix 配置不一致：logic 注册到 `services/logic/...`，gateway 在看 `services/logical/...`
- etcd 版本太老（< 3.0）没有 v3 gateway

### 4.2 logic 自己挂了，gateway 1s 内没下线

正常应当 ≤ 1s（poll 间隔）。如果一直挂着，说明：

1. logic 进程是 graceful 退（SIGTERM）→ `etcd.del(key)` 应当立刻删；如果没删，看 logic 日志有没有 `etcd deregistered` 行。
2. logic 直接 SIGKILL → 走 lease 过期，最长 15s 后 etcd 自己会删。gateway 下次 pollOnce 看到。
3. 网络故障 logic 还活着但发不出 keepAlive：keepAlive 失败 3 次（约 15s）后 logic 会 re-grant，期间 etcd 那把 lease 会过期，gateway 把它下掉，然后再上来。

### 4.3 etcd 自己挂了

logic 端：keepAlive 持续失败 → 每次循环 re-grant + put，自然在 etcd 起来后恢复。期间 gateway 看到 lease 过期会把 logic 下掉，**整段时间路由到该 logic 的消息会失败**（gateway::handleMessage 返回 UNAVAILABLE，外层 ws 收到 `error`）。

修：起一个高可用 etcd 集群（3/5 节点）。本 runbook 单节点只为开发/调试。

### 4.4 gateway 重启后老 KV 残留？

不会。logic 退出时主动 `del`；即使没 del，lease 15s 也兜底过期。gateway 在 `etcdPollLoop` 第一次跑时一次性拉 prefix bootstrap 当前真实状态，然后再 diff。

## 5. 与 RegistryService 路径的对照

| 维度 | gRPC RegistryService | etcd（本路径） |
|---|---|---|
| 协议 | gRPC unary（List）+ stream（Watch）| HTTP/1.0 POST + 1s 轮询 diff |
| 进程 | 必须额外起 muduo-im-registry | 无 |
| 推送变更 | server-stream，~ms 级 | poll 1s（可调） |
| HA | 需要自己写主从 | etcd 集群天生 HA |
| 数据持久化 | 进程内存，重启丢 | etcd 持久化（lease 控制 TTL）|
| 单机依赖 | nlohmann/json | nlohmann/json + 系统 etcd 二进制 |

简单部署可用 RegistryService（更轻），上规模或要 HA 的切 etcd（本路径）。

## 6. 已知限制

1. **轮询而非 watch**：每 1s 一次 `getPrefix` —— 单 etcd 节点 + 几十 logic 实例没问题；千级实例 + 多 gateway 时考虑：
   - 把 `MUDUO_IM_LOGIC_KEY_PREFIX` 拆细（按机房/分片）让每个 gateway 只 watch 自己的子集
   - 升级到 grpc-cpp 真正的 etcd Watch（参见 `EtcdClient.h` 顶部注释）
2. **logic 退出延迟**：当前在有活动 bidi stream 的情况下，logic SIGTERM → 退出最长 ~10s（`Shutdown(deadline=5s)` + 注册线程 5s sleep）。无 stream 场景 < 1s。
3. **etcd v3 HTTP gateway**：用的是 `/v3beta`（兼容 3.3）；3.4+ 推荐显式 `MUDUO_IM_ETCD_PREFIX=/v3`。
4. **没启用 TLS**：当前裸 HTTP，生产环境上 mTLS 需要扩 `EtcdClient.h`。
