# muduo-im 部署索引

> 8 份 RUNBOOK 的统一入口。每个子目录代表一个组件 / 演练场景。
>
> 设计文档：`docs/plans/2026-05-03-kafka-distributed-architecture.md`
> 多 AZ 设计：`docs/plans/2026-05-04-multi-az-design.md`

## 1. 总拓扑

```
                   ┌─────────────────────────────────────────────────┐
ws clients ──HTTP──▶│  haproxy :9090  ──http-check──▶ /health :9081/9182
                   └────────┬───────────────┬────────────────────────┘
                            │               │
                       gateway-A         gateway-B
                       :9091             :9192
                       /health :9081     /health :9182
                       /metrics ↑        /metrics ↑
                            │               │
                            └───────┬───────┘
                                    │ gRPC unary + bidi stream
                                    │ (same-AZ 优先, fail-open)
                            ┌───────┴───────┐
                            │ etcd discovery│
                            │ services/logic│
                            └───────┬───────┘
                                    │
                  ┌─────────────────┼─────────────────┐
                  │                 │                 │
              logic-az1-A       logic-az1-B       logic-az2-A
              :9100             :9101             :9102
                  │                 │                 │
                  └─────────────────┼─────────────────┘
                                    │
              ┌─────────────────────┼─────────────────────┐
              │                     │                     │
        ┌─────▼──────┐       ┌─────▼──────┐       ┌─────▼──────┐
        │   MySQL    │       │   Redis    │       │   Kafka    │
        │ master+2   │       │ Sentinel   │       │ 3 brokers  │
        │ replica    │       │ 1m+2r+3sen │       │ RF=3 isr=2 │
        │ (semi-sync)│       │            │       │            │
        └────────────┘       └────────────┘       └────────────┘

   ┌───────────────── observability ─────────────────┐
   │  Prometheus :9090 ◀── 8 exporter ◀── 业务 + 基础 │
   │     │                                           │
   │     ▼                                           │
   │  Grafana :3000  6 dashboards (muduo-im folder)  │
   └─────────────────────────────────────────────────┘
```

## 2. 子目录索引

| 目录 | 角色 | RUNBOOK | 一键起 |
|---|---|---|---|
| `etcd/` | 服务发现路径文档 | [RUNBOOK.md](etcd/RUNBOOK.md) | apt + 二进制 |
| `grpc/` | RegistryService 单点路径（旧） | [RUNBOOK.md](grpc/RUNBOOK.md) | docker compose up |
| `kafka/` | Kafka 单 broker（dev） + 3-broker 集群 | — | `cluster-tarball/up.sh` |
| `kafka/cluster-tarball/` | KRaft 3-broker 本机集群 | — | `up.sh` + `init-topics.sh` |
| `lb/` | haproxy 前置 ws + http-check | — | `up.sh` |
| `mysql-replication/` | 主从 + 半同步 + GTID + manual promote | [RUNBOOK.md](mysql-replication/RUNBOOK.md) | docker compose |
| `redis-sentinel/` | 1 master + 2 replica + 3 sentinel | — | `up.sh` |
| `multi-az/` | 跨 AZ 路由 + MirrorMaker 模板 | [RUNBOOK.md](multi-az/RUNBOOK.md) | 见 RUNBOOK §2.1 |
| `distributed/` | MySQL semi-sync + Redis Cluster cookbook | [README.md](distributed/README.md) | （文档）|
| `observability/` | Prometheus + 5 exporter + Grafana 6 dashboard | [RUNBOOK.md](observability/RUNBOOK.md) | `up.sh` |

## 3. 端到端起套（开发机）

按下面顺序，每一步 30s ~ 2min：

```bash
# 1. etcd（已通过 apt 装好）
sudo systemctl start etcd

# 2. MySQL（系统 / 单实例 / 主从拓扑选一）
sudo systemctl start mysql              # 单实例
# 或：docker compose -f deploy/mysql-replication/docker-compose.yml up -d

# 3. Redis（系统 / sentinel 拓扑选一）
sudo systemctl start redis-server       # 单实例
# 或：bash deploy/redis-sentinel/up.sh

# 4. Kafka 集群
bash deploy/kafka/cluster-tarball/up.sh
bash deploy/kafka/cluster-tarball/init-topics.sh

# 5. 应用（按 etcd discovery 模式）
cd build
MUDUO_IM_USE_ETCD=1 MUDUO_IM_AZ=az1 \
  ./muduo-im-logic 0.0.0.0:9110 &      # 注：避开 node_exporter:9100
MUDUO_IM_USE_ETCD=1 MUDUO_IM_AZ=az1 \
  MUDUO_IM_GATEWAY_HEALTH_PORT=9081 \
  ./muduo-im-gateway 9091 &
cd ..

# 6. 前置 LB
bash deploy/lb/up.sh

# 7. 观测
bash deploy/observability/up.sh

# 8. 浏览
xdg-open http://127.0.0.1:9090   # Prometheus
xdg-open http://127.0.0.1:3000   # Grafana
xdg-open http://127.0.0.1:7777   # haproxy stats
```

## 4. 演练脚本

每个 component 都有自动化 drill：

| 演练 | 路径 | 实测结果 |
|---|---|---|
| 拓扑 e2e | `tests/etcd/topology_e2e.sh` | 200/200 messages |
| 故障转移 e2e | `tests/etcd/failover_e2e.sh` | 19/20 pairs (95%) 跨 SIGTERM 存活 |
| 多 AZ e2e | `tests/etcd/multi_az_e2e.sh` | 100% same-AZ + 100% fail-open |
| 跨 AZ 延迟（best-effort） | `tests/etcd/multi_az_crosshost_drill.sh` | 路由 PASS（latency 受 lo fast-path 限制） |
| Kafka 滚动重启 | `deploy/kafka/cluster-tarball/rolling_restart_drill.sh` | 1000/1000 投递 0 errors |
| Redis Sentinel failover | `deploy/redis-sentinel/failover_drill.sh` | 6.0s, 0 数据丢失 |
| haproxy fail-over | `deploy/lb/lb_e2e.sh` | gw-A SIGTERM 后 6s 摘掉 |
| MySQL promote | `deploy/mysql-replication/failover_drill.sh` | 文档 + 脚本（docker registry 受限未 live） |

## 5. 端口对照表

| 端口 | 服务 |
|---|---|
| 8080 | chat server HTTP（含 /metrics） |
| 8404 | haproxy /metrics |
| 9081 / 9182 | gateway-A / gw-B /health + /metrics |
| 9091 / 9192 | gateway-A / gw-B WS |
| 9090 | haproxy WS frontend ⚠️ 跟 Prometheus 默认相同 |
| 7777 | haproxy stats UI |
| 9090 | **Prometheus**（与 chat ws_port、haproxy frontend 同号 — RUNBOOK §7） |
| 9100 | **node_exporter**（与 logic 默认 :9100 同号 — RUNBOOK §7） |
| 9104 | mysqld_exporter |
| 9121 | redis_exporter |
| 9308 | kafka_exporter |
| 3000 | Grafana |
| 9092/9192/9292 | Kafka brokers 1/2/3（与 gateway-B :9192 ⚠️ 冲突，选 etcd 路径不同时跑）|
| 9093/9193/9293 | Kafka KRaft controllers |
| 6379/6380/6381 | Redis master / replica1 / replica2 |
| 26379/26380/26381 | Redis sentinel 1/2/3 |
| 2379/2380 | etcd client / peer |
| 3306/3307/3308 | MySQL master / replica1 / replica2 |

## 6. 设计文档总目

```
docs/plans/
├── 2026-04-11-phase3-muduo-im-server.md       初版服务端
├── 2026-04-11-phase4-frontend-integration.md  初版前端
├── 2026-04-17-acid-durability-improvements.md
├── 2026-04-22-plan-b-execution-roadmap.md     PlanB 执行路线
├── 2026-04-25-p1-2-grpc-split-blueprint.md    gateway/logic 拆分
├── 2026-05-03-capacity-baseline.md            Phase 0 容量基线
├── 2026-05-03-kafka-distributed-architecture.md  ★ 主设计
└── 2026-05-04-multi-az-design.md              多 AZ 延伸
```

## 7. 已知冲突一览

| A | B | 解决 |
|---|---|---|
| chat server `ws_port=9090` | Prometheus :9090 | 改 `config.ini` 把 ws_port 移到 9095 |
| logic 默认 :9100 | node_exporter :9100 | logic 启动参数显式 `0.0.0.0:9110`，或多机部署 |
| gateway-B :9192 | Kafka broker-2 :9192 | LB 演练和 Kafka 集群不同时跑 |
| gateway 默认 health :9081 | 多 gateway 同主机 | 第二个 gateway 设 `MUDUO_IM_GATEWAY_HEALTH_PORT=9182` |

## 8. 故障转移手册速查

| 故障 | 自动恢复 | RTO |
|---|---|---|
| 单 logic 挂 | etcd lease + gateway pool 自摘 | ≤ 16s（lease TTL）|
| 单 gateway 挂 | haproxy http-check 摘 | ≤ 6s（fall=3 × inter=2s） |
| Redis master 挂 | Sentinel 自动 failover | ≤ 6s |
| 单 Kafka broker 挂 | RF=3 自动负载均衡 | 0（不中断）|
| MySQL master 挂 | 手动 promote | ≤ 30s（半同步零丢失）|
| 整 AZ 挂 | DNS/LB 切流量 + 手动 promote MySQL | ≤ 60s（manual） |
