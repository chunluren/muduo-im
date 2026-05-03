# Multi-AZ + 容灾设计（Phase 5）

> 作者：claude · 2026-05-04
> 状态：设计 + 部分落地（AZ 标签 + 同 AZ 优先路由）。MirrorMaker / 跨 AZ
> MySQL 链路只给 docker-compose 模板和 RUNBOOK，未做 live 演练。

设计文档原 Phase 4 写"多 AZ 不在本设计范围"。这里把它拉出来。学习
项目下不真买异地机房，本地用容器/网络命名空间模拟同样的拓扑就能
验证逻辑分支。

## 1. 目标

| 编号 | 目标 | 落地状态 |
|---|---|---|
| AZ-1 | 同 AZ 内 ws → gateway → logic 走完不跨 AZ | ✅ 路由实现 |
| AZ-2 | 整个 AZ 挂掉时另一个 AZ 自然接管（≤ 1min RTO）| ⚠️ 设计 + 配置 |
| AZ-3 | Kafka 数据跨 AZ 异步复制（RPO ≈ 1s）| 📋 配置模板 |
| AZ-4 | MySQL 主在 az1，az2 是热备 replica | 📋 配置模板 |
| AZ-5 | etcd 跨 AZ 部署，本身有 AZ tolerance | 📋 配置模板 |

## 2. 拓扑

```
┌──────────────────── AZ1 ───────────────────────────┐  ┌──────── AZ2 ──────────┐
│                                                     │  │                       │
│  ws clients                                         │  │  ws clients           │
│       │                                             │  │       │               │
│       ▼                                             │  │       ▼               │
│  ┌─────────────┐     ┌─────────────┐                │  │  ┌─────────────┐     │
│  │ gateway-az1 │────▶│  logic-az1  │  (prefer same AZ)│  │ gateway-az2 │     │
│  └──────┬──────┘     └──────┬──────┘                │  │  └──────┬──────┘     │
│         │                   │                       │  │         │             │
│         │            ┌──────┴──────┐                │  │         │             │
│         │            │ MySQL-prim  │ ◀──semi-sync── │  │  ┌──────┴──────┐     │
│         │            │ az1 master  │                │  │  │ logic-az2   │     │
│         │            └──────┬──────┘                │  │  └──────┬──────┘     │
│         │                   │ async                 │  │         │             │
│         │                   ▼                       │  │         ▼             │
│  ┌─────────────────────────────────────┐            │  │  ┌──────────────┐    │
│  │ Redis Sentinel (master+1 replica)   │            │  │  │ MySQL replica│    │
│  └──────────────┬──────────────────────┘            │  │  │ async slave  │    │
│                 ▼                                   │  │  └──────────────┘    │
│  ┌─────────────────────────────────────┐            │  │                       │
│  │ Kafka brokers az1 (3 节点 RF=3)     │  ─MM2───▶  │  │ Kafka brokers az2    │
│  └─────────────────────────────────────┘            │  │ (RF=3, mirror only)  │
│                                                     │  │                       │
│  ┌─────────────────────────────────────┐            │  │                       │
│  │ etcd-1 (voter)   etcd-2 (voter)     │            │  │  etcd-3 (voter)      │
│  └─────────────────────────────────────┘            │  │                       │
│                                                     │  │                       │
└─────────────────────────────────────────────────────┘  └───────────────────────┘
```

**关键决策**：
- **etcd 跨 AZ 5 节点**：3 in az1, 2 in az2 → 任 1 AZ 挂仍有多数（quorum）
  - 学习项目下：3 节点（2:1 分布）足够展示 AZ-1 quorum loss 的边界条件
- **MySQL 主在 az1**：cross-AZ 写延迟通常 5-50ms，不接受同步主从。az2 是 async replica
  - 灾难场景：az1 整挂 → 手动 promote az2 replica → 改 etcd 配置 → 应用重连
  - RPO：取决于 binlog 同步 lag，通常 < 1s
- **Kafka MirrorMaker 2 单向**：az1 → az2，az2 不接收业务写。
  - 灾难恢复时：consumer offset 丢一些（acceptable for IM；最坏 push.commands
    重复推送几条，外层 idempotent msgId 兜底）
- **Redis Sentinel 同 AZ**：跨 AZ 失败转移代价大于收益（IM 在线状态可重建）

## 3. 服务发现的 AZ 标签

logic 注册时把 `az` 写进 etcd KV 的 JSON：

```json
{
  "service": "logic",
  "addr": "10.1.2.3:9100",
  "weight": 1,
  "instance": "logic-az1-A",
  "az": "az1"
}
```

env：`MUDUO_IM_AZ=az1`（不设默认空字符串 = AZ-blind 兼容老路径）。

gateway 启动时读自己的 `MUDUO_IM_AZ`，在 `LogicClientPool` 里维护
**两层环**：

```cpp
ConsistentHashRing ringAll_;                                     // 所有 logic
std::unordered_map<std::string, ConsistentHashRing> ringByAz_;   // 按 AZ 分组
std::string localAz_;
```

路由：
1. 如果 `localAz_` 非空且 `ringByAz_[localAz_]` 非空 → 用本 AZ 环挑
2. 否则 fallback 到 `ringAll_`（跨 AZ）

`handleMessage` 失败重试时，挑备用也优先 same-AZ；same-AZ 全挂才跨 AZ。

**为什么不直接弃 ringAll_**？灾难恢复阶段（本 AZ 全挂、流量被 LB 拽到
对面 AZ 但 gateway 没立刻意识到）需要兜底。ringAll_ 是 fail-open 的安全网。

## 4. 灾难恢复 RTO/RPO 约定

| 故障类型 | RTO 目标 | RPO 目标 | 触发动作 |
|---|---|---|---|
| AZ 内单 logic 挂 | ≤ 5s | 0 | etcd lease 过期 + gateway pool 摘掉（已实现）|
| AZ 内 Redis master 挂 | ≤ 6s | < 1s | Sentinel 自动 failover（已实现）|
| AZ 内单 Kafka broker 挂 | 0（不中断）| 0 | RF=3 容忍（已实现）|
| AZ 内 MySQL master 挂 | ≤ 30s | < 1s | promote.sh 提升同 AZ replica（已实现）|
| **整 AZ 挂掉** | **≤ 60s** | **< 5s（业务消息）** | DNS/LB 切流量到对面 AZ + 对面 AZ 应用重连本地 master |

整 AZ 切换的细节：
1. 监控发现 az1 健康检查全失败（haproxy 连不上 + etcd 看不到 az1 logic）
2. DNS / VIP 切到 az2 LB（可手动也可基于 DNS 健康检查的服务自动切）
3. az2 应用本来就连 az2 logic（local-AZ-first），不用动
4. az2 MySQL 是 az1 的 async replica → 手动 promote 为 master + ROLE 切换
5. az2 Kafka 是 az1 的 mirror → consumer 改连 az2 broker，offset 从 mirror lag 处续

**未做的部分**（生产要补）：
- 自动 failover 协调器（用 etcd lock 选 leader 来执行 promote）
- 全链路一键切换脚本（DNS + MySQL + 配置 reload）

## 5. 跨 AZ 数据复制：MirrorMaker 2

MirrorMaker 2 是 Kafka 自带的 MM 工具（基于 Kafka Connect）。配置：

```properties
# az1 → az2 单向
clusters = az1, az2
az1.bootstrap.servers = kafka-az1-1:9092,kafka-az1-2:9092,kafka-az1-3:9092
az2.bootstrap.servers = kafka-az2-1:9092,kafka-az2-2:9092,kafka-az2-3:9092

az1->az2.enabled = true
az1->az2.topics = im\..*
az2->az1.enabled = false   # 单向，az2 不写

replication.factor = 3
checkpoints.topic.replication.factor = 3
heartbeats.topic.replication.factor = 3
offset-syncs.topic.replication.factor = 3
```

详细配置 + 启动方式见 `deploy/multi-az/`。

跨 AZ 复制 lag 视带宽，本地 docker compose 模拟（同主机网络）lag < 100ms。

## 6. 与已有 Phase 的关系

| Phase | 状态 | 多 AZ 影响 |
|---|---|---|
| 1 (Kafka + Outbox) | ✅ | RF=3, min.isr=2 - 单 AZ 内已经容忍 1 broker 挂；跨 AZ 加 MM2 |
| 2 (R/W split + Redis hashtag) | ✅ | hashtag 让 Redis 跨 AZ 容易切 cluster；现在切 sentinel |
| 3 (etcd) | ✅ | etcd 本身可以跨 AZ 5 节点部署 |
| 4 (HA + LB + graceful) | ✅ | 都是 AZ 内的 HA；跨 AZ 由本 Phase 5 接 |
| 5 (multi-AZ) | 🚧 | 当前 PR：AZ 标签 + same-AZ 优先 + MM2 模板 |

## 7. 落地清单

本次 PR 只做"展示性"的 multi-AZ 路由 + 模板：

- [x] logic etcd 注册带 `az` 字段（env: `MUDUO_IM_AZ`）
- [x] LogicClientPool 维护 same-AZ 环 + same-AZ 优先路由
- [x] gateway 读 `MUDUO_IM_AZ` 设置 `localAz_`
- [x] 跨 AZ docker-compose 模板 (`deploy/multi-az/`)
- [x] MirrorMaker 2 配置模板
- [x] e2e 测试：AZ-A 的 gateway 优先路由到 AZ-A 的 logic

不做的（标记为 v2 / future work）：
- ❌ 自动整 AZ failover 编排器
- ❌ DNS/anycast 路由
- ❌ 跨 AZ MySQL 自动 promote
- ❌ 跨 AZ 心跳告警 / SLO 仪表盘

## 8. 验收

`tests/etcd/multi_az_e2e.sh` 验证：

1. 起 etcd
2. 起 4 logic：logic-A1 / logic-A2 在 az1，logic-B1 / logic-B2 在 az2
3. 起 2 gateway：gw-A 在 az1，gw-B 在 az2
4. 通过 gw-A 发 200 条消息 → logic 端日志验证：≥ 95% 落到 az1 logic
5. 杀掉 az1 所有 logic → gw-A 自动 fallback 到 az2 logic（fail-open）
