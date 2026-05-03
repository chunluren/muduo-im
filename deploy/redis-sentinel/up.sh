#!/usr/bin/env bash
# Phase 4.5 启动 Redis Sentinel 拓扑（本地 6 进程）
#
# 端口:
#   master    127.0.0.1:6379
#   replica-1 127.0.0.1:6380
#   replica-2 127.0.0.1:6381
#   sentinel-1 127.0.0.1:26379
#   sentinel-2 127.0.0.1:26380
#   sentinel-3 127.0.0.1:26381
#
# Sentinel 第一次启动会重写 conf（追加 known-replica/known-sentinel 等），
# 所以先 cp 到 /tmp/redis-sentinel/sentinel-N.conf 再改读它。
set -uo pipefail
cd "$(dirname "$0")"

ROOT=/tmp/redis-sentinel
mkdir -p "$ROOT"/{master,replica-1,replica-2,sentinel-1,sentinel-2,sentinel-3}

# 拷贝 conf 到运行时目录（sentinel 会改它）
for s in sentinel-1 sentinel-2 sentinel-3; do
    cp "configs/$s.conf" "$ROOT/$s.conf"
done

start_redis() {
    local name=$1
    local conf=$2
    local pidfile="$ROOT/$name.pid"
    if [[ -f "$pidfile" ]] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
        echo "$name already running pid=$(cat "$pidfile")"
        return
    fi
    echo "start $name"
    nohup redis-server "$conf" > /dev/null 2>&1 &
    echo $! > "$pidfile"
    sleep 0.3
}

start_sentinel() {
    local name=$1
    local conf="$ROOT/$name.conf"
    local pidfile="$ROOT/$name.pid"
    if [[ -f "$pidfile" ]] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
        echo "$name already running pid=$(cat "$pidfile")"
        return
    fi
    echo "start $name (sentinel mode)"
    nohup redis-server "$conf" --sentinel > /dev/null 2>&1 &
    echo $! > "$pidfile"
    sleep 0.3
}

start_redis master    "$(pwd)/configs/master.conf"
start_redis replica-1 "$(pwd)/configs/replica-1.conf"
start_redis replica-2 "$(pwd)/configs/replica-2.conf"
sleep 1.0  # replicas connect to master before sentinels see topology

start_sentinel sentinel-1
start_sentinel sentinel-2
start_sentinel sentinel-3

sleep 1.5
echo
echo "=== sentinel sees ==="
redis-cli -p 26379 sentinel master im-master | head -16
echo
echo "=== replicas of im-master (sentinel-1) ==="
redis-cli -p 26379 sentinel replicas im-master | head -16
echo
echo "✓ Sentinel topology up"
echo "  master  : 127.0.0.1:6379"
echo "  replicas: 127.0.0.1:6380, 6381"
echo "  sentinels: 127.0.0.1:26379, 26380, 26381 (quorum=2)"
echo "  failover drill: bash deploy/redis-sentinel/failover_drill.sh"
echo "  shutdown:       bash deploy/redis-sentinel/down.sh"
