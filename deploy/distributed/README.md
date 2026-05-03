# Phase 2 分布式存储部署指南

> 起草: 2026-05-03
> 范围: MySQL 1主N从 + Redis Cluster
> 前提: 代码已落地（commit 6c68ec0）—— pool 层支持 acquireRead/Write，Redis key 已加 hashtag

代码已经分布式 ready，但**实际起 replica/cluster 是部署侧动作**。本文是配置 cookbook，让你能在本地 / 生产把基础设施切到分布式。

---

## 1. MySQL 1 主 1 从（Semi-sync）

### 1.1 master 配置（`my.cnf`）
```ini
[mysqld]
server-id = 1
log_bin = /var/log/mysql/mysql-bin
binlog_format = ROW
binlog_row_image = FULL
sync_binlog = 1
innodb_flush_log_at_trx_commit = 1

# semi-sync：主等至少一个从收到 binlog 才返回
plugin-load-add = semisync_master.so
rpl_semi_sync_master_enabled = 1
rpl_semi_sync_master_timeout = 1000   # 1s 等不到从 → 退化为异步
```

### 1.2 slave 配置
```ini
[mysqld]
server-id = 2
relay-log = /var/log/mysql/mysql-relay
read_only = 1                         # 防误写
super_read_only = 1
plugin-load-add = semisync_slave.so
rpl_semi_sync_slave_enabled = 1
```

### 1.3 复制账号 + 拉起复制
```sql
-- master
CREATE USER 'repl'@'%' IDENTIFIED BY 'repl-pass';
GRANT REPLICATION SLAVE ON *.* TO 'repl'@'%';
SHOW MASTER STATUS;  -- 记下 File + Position

-- slave
CHANGE MASTER TO
  MASTER_HOST='master.example.com',
  MASTER_USER='repl',
  MASTER_PASSWORD='repl-pass',
  MASTER_LOG_FILE='mysql-bin.000001',
  MASTER_LOG_POS=1234;
START SLAVE;
SHOW SLAVE STATUS\G  -- Slave_IO_Running=Yes Slave_SQL_Running=Yes
```

### 1.4 muduo-im 端配置
ChatServer 持有的 MySQLPool 是单池。要启用读写分离，需改 main.cpp 注册副本：

```cpp
// main.cpp
auto masterPool = std::make_shared<MySQLPool>(masterCfg);

// 每个 slave 一个独立 MySQLPool
for (auto& slaveCfg : slaveConfigs) {
    auto replicaPool = std::make_shared<MySQLPool>(slaveCfg);
    masterPool->addReadReplica(replicaPool);
}

// 把 masterPool 传给 ChatServer 即可：MessageService 的读路径
// 自动 round-robin 到 replicas
ChatServer server(loop, ports, masterCfg, redisCfg, jwt, masterPool);
```

(目前 main.cpp 只取 `mysqlConfig`；要完成接入还需要扩展 ChatServer ctor 接受预构建池。生产部署前补这步。)

### 1.5 验证
```sql
-- master 写
INSERT INTO private_messages(...) VALUES (...);

-- slave 读（毫秒级延迟）
SELECT * FROM private_messages WHERE msg_id='...';
```

监控关键指标：
- `Seconds_Behind_Master`：副本延迟，> 5s 告警
- `rpl_semi_sync_master_no_tx`：semi-sync 退化次数，频繁 = 网络问题

---

## 2. Redis Cluster（3 主 3 从）

### 2.1 docker compose 起 6 节点
```yaml
# deploy/redis-cluster/docker-compose.yml
services:
  redis-1: { image: redis:7, command: redis-server --port 7001 --cluster-enabled yes --cluster-config-file nodes-1.conf --cluster-node-timeout 5000 --appendonly yes, ports: ["7001:7001"] }
  redis-2: { image: redis:7, command: redis-server --port 7002 --cluster-enabled yes --cluster-config-file nodes-2.conf --cluster-node-timeout 5000 --appendonly yes, ports: ["7002:7002"] }
  redis-3: { image: redis:7, command: redis-server --port 7003 --cluster-enabled yes --cluster-config-file nodes-3.conf --cluster-node-timeout 5000 --appendonly yes, ports: ["7003:7003"] }
  redis-4: { image: redis:7, command: redis-server --port 7004 --cluster-enabled yes --cluster-config-file nodes-4.conf --cluster-node-timeout 5000 --appendonly yes, ports: ["7004:7004"] }
  redis-5: { image: redis:7, command: redis-server --port 7005 --cluster-enabled yes --cluster-config-file nodes-5.conf --cluster-node-timeout 5000 --appendonly yes, ports: ["7005:7005"] }
  redis-6: { image: redis:7, command: redis-server --port 7006 --cluster-enabled yes --cluster-config-file nodes-6.conf --cluster-node-timeout 5000 --appendonly yes, ports: ["7006:7006"] }
```

### 2.2 创建 cluster
```bash
docker compose up -d
sleep 5
docker exec redis-1 redis-cli --cluster create \
  127.0.0.1:7001 127.0.0.1:7002 127.0.0.1:7003 \
  127.0.0.1:7004 127.0.0.1:7005 127.0.0.1:7006 \
  --cluster-replicas 1 --cluster-yes
```

### 2.3 hashtag 已就绪
代码（commit 6c68ec0）已经把所有 per-user 的 key 加上了 `{uid}` hashtag：

| Key | 路由依据 |
|-----|---------|
| `online:{178}:web` | uid 178 |
| `online:{178}:mobile` | uid 178（同 slot）|
| `unread:{178}:99` | uid 178 |
| `unread_mentions:{178}:42` | uid 178 |
| `jwt_blacklist:{jti}` | jti（每个独立，自然分散） |
| `push_queue` / `thumb_queue` | 全局 list（pin 到一个 slot，正常） |

### 2.4 muduo-im 客户端
当前 `RedisPool` 是基于 hiredis 的单连接池——**不支持 cluster MOVED 重定向**。要真上 cluster 必须换 client：

选项 1：改用 [hiredis-vip](https://github.com/vipshop/hiredis-vip) 或 [redis++](https://github.com/sewenew/redis-plus-plus)（带 cluster 支持）
选项 2：在前面挂 [redis-cluster-proxy](https://github.com/RedisLabs/redis-cluster-proxy)，让现有 client 透明使用

**结论**：代码层 hashtag 已经就绪；client 升级是另一项工作，不在本 Phase 范围。当前部署仍是单 Redis，hashtag 不影响。

---

## 3. 验证脚本

```bash
# MySQL 复制延迟监控
mysql -e "SHOW SLAVE STATUS\G" | grep -E 'Slave_IO|Seconds_Behind'

# Redis Cluster 状态
docker exec redis-1 redis-cli -p 7001 cluster info | grep cluster_state

# 查同 uid 的所有 keys 都在同 slot
docker exec redis-1 redis-cli -c -p 7001 cluster keyslot 'online:{178}:web'   # → 5347
docker exec redis-1 redis-cli -c -p 7001 cluster keyslot 'unread:{178}:99'    # → 5347 (同 slot)
docker exec redis-1 redis-cli -c -p 7001 cluster keyslot 'online:{99}:web'    # → 不同 slot
```

---

## 4. 已知限制与下一步

- **muduo-im 端 cluster client 缺失**：hashtag 不解决"client 拒绝 MOVED"。生产前必换 client。
- **MySQL slave 故障转移**：当前 `addReadReplica` 不做 health check，slave 挂了会一次失败 + fallback 到 master。补：addReadReplica 接 CircuitBreaker。
- **stale read**：`read-after-write` 同行在 semi-sync 下还是有 ~ms lag。强一致读必须 acquireWrite。

---

## 5. 检查清单（生产前）

- [ ] master 开 binlog ROW + sync_binlog=1
- [ ] semi-sync 双方 plugin 都装
- [ ] slave 有 read_only=1 + super_read_only=1
- [ ] muduo-im main.cpp 把 slave config 注册到 masterPool.addReadReplica()
- [ ] Redis cluster 6 节点起来 + cluster create 完成
- [ ] muduo-im 换 cluster-aware redis client
- [ ] Prometheus 抓 Seconds_Behind_Master 与 redis cluster_state 指标
