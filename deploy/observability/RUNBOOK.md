# Phase 5b.6 Observability Runbook

> Prometheus + 4 个 exporter 一键起套，把所有基础设施 + 应用指标抓进来。

## 1. 拓扑

```
                ┌──────────────────────────────────────────┐
                │  Prometheus  :9090  (UI + tsdb)          │
                └────┬───────┬──────┬──────┬──────┬────────┘
                     │ scrape every 15s
       ┌─────────────┼───────┼──────┼──────┼─────────────┐
       ▼             ▼       ▼      ▼      ▼             ▼
  /metrics       9100      9104   9121   9308           8404
   (apps          node     mysql  redis  kafka         haproxy
    自带 ─        exp.     exp.   exp.   exp.       (内置 exporter)
    chat:8080
    etcd:2379)
```

| 端口 | 进程 | 来源 | 暴露什么 |
|---|---|---|---|
| 9090 | prometheus | apt | 自身 + UI |
| 9100 | node_exporter | apt | cpu/mem/disk/net |
| 9104 | mysqld_exporter | apt | qps/replication/innodb |
| 9121 | redis_exporter | apt | sentinel/master/replica/keys |
| 9308 | kafka_exporter | github tarball /usr/local/bin | broker/topic/consumer lag |
| 8404 | haproxy 自身 | haproxy.cfg `frontend prometheus` | backend/server status |
| 8080 | muduo-im chat | HttpServer.enableMetrics() | chat_*/redis_sentinel_master_switches_total |
| 2379 | etcd 自身 | 内置 | leader/raft/watch |

## 2. 一键起

```bash
# 装 Prometheus + 3 个 apt exporter（一次）
sudo apt install -y prometheus prometheus-node-exporter \
                    prometheus-redis-exporter prometheus-mysqld-exporter

# kafka_exporter（github release，已放在 /usr/local/bin/）
# 装的时候：
#   wget https://github.com/danielqsj/kafka_exporter/releases/download/v1.7.0/kafka_exporter-1.7.0.linux-amd64.tar.gz
#   tar xzf kafka_exporter-1.7.0.linux-amd64.tar.gz
#   sudo cp kafka_exporter-1.7.0.linux-amd64/kafka_exporter /usr/local/bin/

# 起
bash deploy/observability/up.sh

# 看
xdg-open http://127.0.0.1:9090/targets

# 停
bash deploy/observability/down.sh
```

实测（基础设施跑着的话）：8 target，**7 UP, 1 DOWN**（haproxy 没跑就 DOWN）。

## 3. 关键查询

### 平台健康
```promql
up                        # 每个 target 的 1/0
sum by (job) (up == 0)    # DOWN 的 target 列表
```

### 业务（chat server）
```promql
chat_online_users                             # 在线用户数
rate(http_requests_total[1m])                 # HTTP QPS
chat_worker_pool_pending_tasks                # 业务队列堆积
rate(chat_worker_pool_dropped_tasks[1m])      # 业务丢任务速率
increase(redis_sentinel_master_switches_total[1h])  # redis failover 次数
```

### Kafka
```promql
kafka_consumergroup_lag                       # 消费 lag（按 topic/partition/group）
kafka_topic_partitions                        # 各 topic 分区数
sum(kafka_consumergroup_lag) by (consumergroup) > 1000   # 单组累计 lag > 1000
```

### Redis
```promql
redis_connected_clients                       # 连接数
redis_memory_used_bytes                       # 内存占用
redis_master_link_up                          # replica 是否跟主连着
```

### MySQL
```promql
mysql_up                                      # 实例存活
mysql_global_status_threads_connected         # 当前连接数
rate(mysql_global_status_queries[1m])         # qps
mysql_slave_status_seconds_behind_master      # 主从延迟（s）
```

### haproxy
```promql
haproxy_server_status                         # 0=DOWN/1=UP/2=GOING_DOWN ...
sum by (proxy) (haproxy_server_status == 0)   # 每个 backend 的 down 节点数
```

### Node
```promql
1 - avg by (instance) (rate(node_cpu_seconds_total{mode="idle"}[1m]))   # CPU 利用率
node_memory_MemAvailable_bytes / node_memory_MemTotal_bytes             # 内存可用率
```

## 4. 告警建议（未实现，参考）

```yaml
# /etc/prometheus/alert.rules.yml （需要 alertmanager）
groups:
  - name: muduo-im
    rules:
      - alert: ServiceDown
        expr: up == 0
        for: 1m
      - alert: WorkerQueueFull
        expr: chat_worker_pool_pending_tasks > 5000
        for: 30s
      - alert: KafkaConsumerLagHigh
        expr: sum(kafka_consumergroup_lag) by (consumergroup) > 10000
        for: 2m
      - alert: RedisMasterChurning
        expr: increase(redis_sentinel_master_switches_total[10m]) >= 3
        for: 1m
      - alert: MySQLReplicaLag
        expr: mysql_slave_status_seconds_behind_master > 30
        for: 5m
```

## 5. 加 Grafana（可选）

```bash
sudo apt install -y grafana
sudo systemctl start grafana-server
# 访问 http://127.0.0.1:3000  默认 admin/admin
# Add data source → Prometheus → http://127.0.0.1:9090
```

可直接 import 的现成 dashboard ID（Grafana.com）：
- 1860（Node Exporter Full）
- 7362（MySQL Overview）
- 11835（Redis Dashboard）
- 7589（Kafka Exporter Overview）
- 12693（HAProxy 2 Full）

## 6. 故障排查

### Prometheus 起不来
```bash
tail -20 /tmp/observability/prometheus.log
# 常见：
#   - 配置语法错：prometheus --check-config configs/prometheus.yml
#   - 端口占用：ss -ltn | grep :9090
#   - tsdb 数据损坏：rm -rf /tmp/observability/prom-data
```

### 某个 target 一直 DOWN
```bash
# 看 lastError
curl -s http://127.0.0.1:9090/api/v1/targets | python3 -c '
import sys, json
for t in json.load(sys.stdin)["data"]["activeTargets"]:
    if t["health"] != "up":
        print(t["labels"]["job"], "→", t.get("lastError"))
'
```

### kafka_exporter crash on startup
启动时所有 broker 都不通就直接 exit。先确认 kafka 跑着：
```bash
ss -ltn | grep -E ':(9092|9192|9292)\s'
# 没的话起 cluster
bash deploy/kafka/cluster-tarball/up.sh
```

### mysqld_exporter 拒绝认证
检查 `deploy/observability/configs/.my.cnf`，确保 `user`/`password` 跟实际 MySQL 一致：
```bash
mysql --defaults-file=deploy/observability/configs/.my.cnf -e "SELECT 1"
```

## 7. 端口冲突注意

下表的端口不能让别的进程占用：

| 端口 | 谁用 |
|---|---|
| 9090 | Prometheus（也是 chat server 的 ws 默认端口！）|
| 8080 | chat server `/metrics` |
| 8404 | haproxy `/metrics` |
| 9100/9104/9121/9308 | exporters |

**已知冲突**：chat server 默认 `server.ws_port = 9090` ↔ Prometheus :9090。
要同时跑两个，改 `config.ini` 里 `server.ws_port` 为别的（如 9095）。
