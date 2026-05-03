#!/usr/bin/env bash
# Phase 5b.6: 起整套 observability —— Prometheus + 4 个 exporter
#
# 端口：
#   prometheus               9090
#   node_exporter            9100
#   redis_exporter           9121
#   mysqld_exporter          9104
#   kafka_exporter           9308
#   haproxy /metrics         8404 (haproxy 自身已起)
#   muduo-im /metrics        8080 (chat server 已起)
#   etcd /metrics            2379
#
# 启动：bash deploy/observability/up.sh
# 停止：bash deploy/observability/down.sh
# 看：  http://127.0.0.1:9090
set -uo pipefail
cd "$(dirname "$0")"

CONF=$(pwd)/configs/prometheus.yml
MYCNF=$(pwd)/configs/.my.cnf
DATA_ROOT=/tmp/observability
mkdir -p "$DATA_ROOT"

# 系统服务起着的话先停（避免端口冲突）
for svc in prometheus prometheus-node-exporter prometheus-redis-exporter prometheus-mysqld-exporter; do
    if systemctl is-active --quiet "$svc" 2>/dev/null; then
        echo "stopping system $svc"
        sudo systemctl stop "$svc"
    fi
done

start_proc() {
    local name=$1
    local pidfile="$DATA_ROOT/$name.pid"
    local logfile="$DATA_ROOT/$name.log"
    shift
    if [[ -f "$pidfile" ]] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
        echo "$name already running pid=$(cat "$pidfile")"
        return
    fi
    echo "start $name -> $logfile"
    nohup "$@" > "$logfile" 2>&1 &
    echo $! > "$pidfile"
    sleep 0.3
}

# 1. node_exporter (机器层指标)
start_proc node-exporter prometheus-node-exporter \
    --web.listen-address=:9100

# 2. redis_exporter (默认连 :6379；sentinel 模式自动发现)
start_proc redis-exporter prometheus-redis-exporter \
    -redis.addr=redis://127.0.0.1:6379 \
    -web.listen-address=:9121

# 3. mysqld_exporter
start_proc mysqld-exporter env DATA_SOURCE_NAME="root:@(127.0.0.1:3306)/" \
    prometheus-mysqld-exporter \
    --config.my-cnf="$MYCNF" \
    --web.listen-address=:9104 \
    --collect.global_status \
    --collect.global_variables \
    --collect.slave_status \
    --collect.info_schema.processlist

# 4. kafka_exporter
start_proc kafka-exporter kafka_exporter \
    --kafka.server=127.0.0.1:9092 \
    --kafka.server=127.0.0.1:9192 \
    --kafka.server=127.0.0.1:9292 \
    --web.listen-address=:9308

# 5. Prometheus
start_proc prometheus prometheus \
    --config.file="$CONF" \
    --storage.tsdb.path="$DATA_ROOT/prom-data" \
    --web.listen-address=:9090 \
    --storage.tsdb.retention.time=7d \
    --web.enable-lifecycle

sleep 1
echo
echo "=== process state ==="
for name in node-exporter redis-exporter mysqld-exporter kafka-exporter prometheus; do
    pidfile="$DATA_ROOT/$name.pid"
    if [[ -f "$pidfile" ]] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
        echo "  ✓ $name pid=$(cat "$pidfile")"
    else
        echo "  ✗ $name DOWN — see $DATA_ROOT/$name.log"
    fi
done

echo
echo "=== ports ==="
ss -ltn 2>/dev/null | grep -E ':(9090|9100|9104|9121|9308)\s' | head -10

echo
echo "✓ Observability stack up."
echo "  Prometheus UI : http://127.0.0.1:9090"
echo "  Targets       : http://127.0.0.1:9090/targets"
echo "  Sample query  : http://127.0.0.1:9090/graph?g0.expr=up"
echo "  Stop          : bash deploy/observability/down.sh"
