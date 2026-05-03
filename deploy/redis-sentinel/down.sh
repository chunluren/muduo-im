#!/usr/bin/env bash
# 停整套 sentinel 拓扑（不擦数据；擦数据：rm -rf /tmp/redis-sentinel）
set -uo pipefail
ROOT=/tmp/redis-sentinel

for name in sentinel-1 sentinel-2 sentinel-3 replica-1 replica-2 master; do
    PID_FILE="$ROOT/$name.pid"
    [[ -f "$PID_FILE" ]] || continue
    PID=$(cat "$PID_FILE")
    if kill -0 "$PID" 2>/dev/null; then
        echo "stop $name (pid=$PID)"
        kill -TERM "$PID"
    fi
    rm -f "$PID_FILE"
done
sleep 0.5
echo "all redis processes down"
