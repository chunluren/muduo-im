#!/usr/bin/env bash
# 停整套 observability。不擦数据目录 /tmp/observability/prom-data。
set -uo pipefail
DATA_ROOT=/tmp/observability

for name in prometheus kafka-exporter mysqld-exporter redis-exporter node-exporter; do
    pidfile="$DATA_ROOT/$name.pid"
    [[ -f "$pidfile" ]] || continue
    pid=$(cat "$pidfile")
    if kill -0 "$pid" 2>/dev/null; then
        echo "stop $name pid=$pid"
        kill -TERM "$pid"
    fi
    rm -f "$pidfile"
done
sleep 0.5
echo "all observability processes down"
