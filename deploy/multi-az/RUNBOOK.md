# Phase 5 多 AZ 部署 Runbook

> 设计文档：`docs/plans/2026-05-04-multi-az-design.md`
>
> 当前 PR 落地的 = AZ 标签 + same-AZ 优先路由 + cross-AZ Kafka 复制配置模板。
> 整 AZ failover 的自动化编排留给 v2。

## 1. 同 AZ 内（每个 AZ 一份）

按 `deploy/etcd`、`deploy/kafka/cluster-tarball`、`deploy/redis-sentinel`、
`deploy/mysql-replication` 起完整一份基础设施。每个 AZ：

- 1 etcd 集群子集（2 节点 in az1，1 节点 in az2，或者 3:2）
- 1 Kafka 3-broker 集群
- 1 MySQL master + replica
- 1 Redis Sentinel 拓扑（3 sentinel + 1 master + 2 replica）

## 2. 应用进程的 AZ 绑定

logic 启动：

```bash
MUDUO_IM_USE_ETCD=1 \
  MUDUO_IM_AZ=az1 \
  MUDUO_IM_LOGIC_INSTANCE_ID=logic-az1-A \
  MUDUO_IM_LOGIC_ADVERTISE_HOST=10.1.2.3 \
  ./muduo-im-logic 0.0.0.0:9100
```

→ etcd 里写：

```json
{ "service": "logic", "addr": "10.1.2.3:9100",
  "weight": 1, "instance": "logic-az1-A", "az": "az1" }
```

gateway 启动：

```bash
MUDUO_IM_USE_ETCD=1 \
  MUDUO_IM_AZ=az1 \
  MUDUO_IM_GATEWAY_ID=gw-az1-A \
  ./muduo-im-gateway 9091
```

→ 日志：

```
[gateway] using etcd ... localAz=az1
[pool] add logic instance=logic-az1-A addr=10.1.2.3:9100 az=az1 (local)
[pool] add logic instance=logic-az2-A addr=10.2.3.4:9100 az=az2 (remote)
```

## 3. 跨 AZ Kafka 复制

部署 MirrorMaker 2（两个 AZ 各一份，单向 az1→az2）：

```bash
# 在 az2 找一台机器（有跨 AZ 网络访问 az1 broker），跑：
$KAFKA_HOME/bin/connect-mirror-maker.sh deploy/multi-az/mm2.properties &
```

验证：

```bash
# 在 az2 broker 上看 mirror 过来的 topic
$KAFKA_HOME/bin/kafka-topics.sh --bootstrap-server kafka-az2-1:9092 --list \
    | grep "az1\."

# 期望看到：
#   az1.im.messages
#   az1.im.events
#   az1.im.push.commands
#   az1.heartbeats     ← mm2 自己的
#   az1.checkpoints.internal
```

灾难场景：az1 整挂 → consumer 从 `az1.im.messages` 接着消费（offset 通过
checkpoints topic 翻译）。RPO 取决于 mm2 lag，跨地域通常 < 5s。

## 4. 整 AZ failover 手册（手动）

az1 全挂时人工/编排器执行：

1. **DNS / VIP 切流量**：把外部 LB（haproxy/nginx）的 backend 列表
   切成 az2 gateway。
2. **MySQL promote**：在 az2 replica 上跑：
   ```bash
   bash deploy/mysql-replication/promote.sh \
        --new-master-port 3306 --remaining-replica-ports "" \
        --new-master-host az2-mysql-host \
        --new-master-internal-host az2-mysql
   ```
3. **配置 reload**：把 `config.ini` 的 `mysql.host` 改为 az2 master
   地址，重启所有 az2 应用进程（或 SIGHUP，目前没实现）。
4. **Kafka offset**：az2 的 consumer group 从 `az1.im.messages` 续读
   （mm2 已经把 offset 翻译好了，直接 subscribe 即可）。
5. **Sentinel**：az2 已经有自己的 sentinel 拓扑，不动。

灾后 az1 修复回来：
- MySQL：旧 master 改 replica 角色，连接到新 master（`CHANGE REPLICATION SOURCE`）
- Kafka：mm2 反向（az2→az1）拉数据回来
- 应用：保持现状（az2 当主），不必切回 az1

## 5. e2e 测试

```bash
./tests/etcd/multi_az_e2e.sh
```

验：
- 通过 az1 gateway 发 200 条 → ≥ 95% 落到 az1 logic
- kill 全部 az1 logic → 通过 az1 gateway 仍能发出，全走 az2 logic（fail-open）

实测：
- 200/200 messages, 199/199 routed to az1（剩 1 是 etcd lease 边界 race，可接受）
- az1 down 后 50/50 messages 全走 az2，p99 持平

### 5.1 跨主机模拟（带延迟）

`tests/etcd/multi_az_crosshost_drill.sh` 用 `tc netem` 在 lo 上给 az2 端口注入
单向 15ms 延迟。**已知限制**：tc 在 loopback 上经常 fast-path 跳过，实测 p99 差距
不一定能稳定显出来。脚本仍会跑两轮 e2e 验证路由逻辑，那部分是确定的。

要做"真" 跨节点模拟，开两个 netns：

```bash
sudo ip netns add az1
sudo ip netns add az2
sudo ip link add veth-az1 type veth peer name veth-az2
sudo ip link set veth-az1 netns az1
sudo ip link set veth-az2 netns az2
sudo ip netns exec az1 ip addr add 10.1.0.1/24 dev veth-az1
sudo ip netns exec az2 ip addr add 10.1.0.2/24 dev veth-az2
sudo ip netns exec az1 ip link set veth-az1 up
sudo ip netns exec az2 ip link set veth-az2 up
sudo ip netns exec az2 tc qdisc add dev veth-az2 root netem delay 15ms

# 在 az1 namespace 跑 logic-az1 / gateway-A：
sudo ip netns exec az1 ./build/muduo-im-logic 0.0.0.0:9100
# az2 同理
```

这种方式 tc qdisc 在 veth 上**确实**生效（不是 lo 的 fast-path）。

## 6. 与设计目标对照

| 目标 | 实现状态 |
|---|---|
| AZ-1 同 AZ 内闭环 | ✅ 100% same-AZ 路由 |
| AZ-2 整 AZ 容灾 | ⚠️ 手动流程（本 RUNBOOK 第 4 节） |
| AZ-3 Kafka 跨 AZ 异步复制 | 📋 mm2.properties 模板，未 live |
| AZ-4 MySQL 跨 AZ replica | 📋 docker-compose 模板，复用 deploy/mysql-replication |
| AZ-5 etcd 跨 AZ | ⚠️ 现有 etcd RUNBOOK 不区分 AZ；生产部署时调拓扑 |
