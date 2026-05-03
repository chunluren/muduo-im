# Phase 4.4 MySQL 主从复制 + 手动 Failover Runbook

> 设计文档原计划接 Orchestrator 自动 failover；学习项目下用**手动 promote 脚本**
> 等价覆盖。脚本可作为日后接 Orchestrator hook 的参考实现。

## 1. 拓扑

```
                     ┌────────────────┐
        write/read   │  mysql-master  │  3306, semi-sync ON
       ─────────────▶│   server_id=1  │
                     └────────┬───────┘
                              │ GTID + row binlog
              ┌───────────────┴────────────────┐
              ▼                                ▼
     ┌────────────────┐               ┌────────────────┐
     │ mysql-replica-1│ 3307          │ mysql-replica-2│ 3308
     │   server_id=2  │ read-only     │   server_id=3  │ read-only
     └────────────────┘               └────────────────┘
```

- 同步策略：`rpl_semi_sync` —— master commit 必须等至少 1 个 replica ack；
  超时 1s 退化为 async（避免脑裂阻塞业务）
- 复制定位：`SOURCE_AUTO_POSITION=1` (GTID-based)，failover 时不用算 binlog 位点
- 安全网：`super_read_only=ON` 在所有 replica 上，避免误写

## 2. 第一次起步

```bash
# 起 1 master + 2 replica
docker compose -f deploy/mysql-replication/docker-compose.yml up -d

# 配复制（创建 repl 用户 + CHANGE REPLICATION SOURCE）
bash deploy/mysql-replication/init-replication.sh

# 验证 catch-up
mysql -h 127.0.0.1 -P 3307 -uroot -e "SHOW REPLICA STATUS\G" | grep Running
# 期望：Replica_IO_Running: Yes; Replica_SQL_Running: Yes
```

## 3. 应用接入

`config.ini`：
```ini
[mysql]
host=127.0.0.1
port=3306                # 写 → 主
pool_min=3
pool_max=10

# Phase 2A 已有 acquireRead / acquireWrite 拆分；下面是只读副本列表
[mysql.replicas]
addr=127.0.0.1:3307
addr=127.0.0.1:3308
```

读路径走 `MySQLPool::acquireRead()`（轮询副本），写路径走 `acquireWrite()`（主）。

## 4. Failover 演练

完整流程脚本：

```bash
bash deploy/mysql-replication/failover_drill.sh
```

干的事：
1. 写 100 行业务数据到 master（写到 `drill_kv` 表）
2. 杀 master 容器（`docker stop mysql-master`）
3. 调 `promote.sh` 把 replica-1 提升为新 master：
   - `STOP REPLICA + RESET REPLICA ALL`
   - `SET @@global.read_only = OFF`
4. 把 replica-2 改指向新 master（`CHANGE REPLICATION SOURCE TO ...`）
5. 在新 master 写 50 条 → 验证 replica-2 看得到

期望：< 30s 完成 failover，零数据丢失（半同步保证）。

## 5. promote.sh 单独使用

不跑全套 drill，只想做提升动作：

```bash
bash deploy/mysql-replication/promote.sh \
    --new-master-port 3307 \
    --remaining-replica-ports "3308" \
    --new-master-internal-host mysql-replica-1
```

提升完成后**必须**：
- 改应用 `config.ini` 的 `mysql.host/port`
- 重启应用进程（或者写一个 SIGHUP reload 的接口，目前没有）

## 6. 反向：旧 master 拉回来当 replica

灾后旧 master 修好了想拉回来：

```sql
# 在旧 master 上跑
STOP REPLICA;            -- 防御性
RESET REPLICA ALL;
SET @@global.super_read_only = ON;
CHANGE REPLICATION SOURCE TO
  SOURCE_HOST='mysql-replica-1',
  SOURCE_PORT=3306,
  SOURCE_USER='repl',
  SOURCE_PASSWORD='repl',
  SOURCE_AUTO_POSITION=1,
  GET_SOURCE_PUBLIC_KEY=1;
START REPLICA;
```

GTID 让这步免于位点对齐；如果旧 master 比新 master 多写了（脑裂）会
报 `Could not find first log file`，手动决断丢/留。

## 7. 常见问题

### Replica_IO_Running: Connecting

通常是网络/认证问题：

```bash
# 在 replica 看错误
mysql -P 3307 -uroot -e "SHOW REPLICA STATUS\G" | grep -i error
```

常见：
- `Last_IO_Error: ... access denied` → repl 用户密码不一致
- `Last_IO_Error: ... no master found` → master 容器没起或网络隔离

### Seconds_Behind_Source 持续增长

写量超过 replica 应用 binlog 的速度。短期：等。长期：
- `slave_parallel_workers > 0` 开多线程 apply
- 业务写减负 / 拆库

### promote 后旧 master 拉回时报错

90% 是脑裂：旧 master 在被 stop 之前还接受了一些写，新 master promote
后这些写没有同步到新 master。修法：
- 接受丢失，`RESET MASTER` 旧 master 重新同步
- 或人工 dump 出差异、apply 到新 master、再重连

## 8. 与 Orchestrator 的差距

| 特性 | 本 RUNBOOK / promote.sh | Orchestrator |
|---|---|---|
| 检测 master 挂 | 手动（kill / 监控告警） | 自动（多级探测）|
| 选新 master | 脚本指定 | 自动选 GTID 最新 + 优先级 |
| 切其他 replica | 脚本一把 | 自动 |
| 应用配置切换 | 手动改 config.ini | 通过 hook 调用 LB / 服务发现 |
| 反向同步老主 | 手动 SQL | 自动 |

后续接 Orchestrator 时，把 promote.sh 的逻辑搬到 Orchestrator hook 里就行。
