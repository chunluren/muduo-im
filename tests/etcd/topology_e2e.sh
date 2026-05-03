#!/usr/bin/env bash
# Phase 3E: etcd 服务发现 拓扑 e2e
#
# 与 tests/grpc/topology_e2e.sh 对照：
#   - 起单节点 etcd（如未运行）
#   - 不再起 muduo-im-registry
#   - logic/gateway 通过 MUDUO_IM_USE_ETCD=1 走 etcd 路径
#   - 复用 tests/grpc/topology_e2e.py（仅依赖 ws 端口，不关心 service discovery）
#
# 用法（在 build/ 目录已编出 muduo-im-{logic,gateway} 之后）：
#   tests/etcd/topology_e2e.sh
#
# 依赖：
#   - etcd 二进制在 PATH 上；或者已经在 127.0.0.1:2379 跑着
#   - MySQL 可连（默认 root@127.0.0.1, db=muduo_im）
#   - python3 + websocket-client
set -uo pipefail
cd "$(dirname "$0")/../.."

BUILD=${BUILD:-build}
LOG=${LOG:-/tmp/muduo-im-topology-etcd}
ETCD_DATA=${ETCD_DATA:-/tmp/etcd-data-e2e}
ETCD_PORT=${ETCD_PORT:-2379}
mkdir -p "$LOG"

started_etcd=0

cleanup() {
    echo "[topology] cleanup"
    for f in "$LOG"/*.pid; do
        [[ -f $f ]] || continue
        pid=$(cat "$f"); [[ -n $pid ]] && kill "$pid" 2>/dev/null || true
        rm -f "$f"
    done
    if [[ $started_etcd -eq 1 ]]; then
        kill "$(cat "$LOG/etcd.pid" 2>/dev/null)" 2>/dev/null || true
        rm -f "$LOG/etcd.pid"
    fi
    sleep 0.3
}
trap cleanup EXIT INT TERM
cleanup  # 防止上一轮残留

# --- etcd（如已经在跑就直接用，不起新进程）---
if curl -sf -m 1 -X POST "http://127.0.0.1:${ETCD_PORT}/v3beta/maintenance/status" -d '{}' >/dev/null 2>&1; then
    echo "[topology] reusing existing etcd on :${ETCD_PORT}"
elif command -v etcd >/dev/null 2>&1; then
    echo "[topology] starting local etcd on :${ETCD_PORT}"
    rm -rf "$ETCD_DATA"
    etcd --name e2e --data-dir "$ETCD_DATA" \
         --listen-client-urls "http://127.0.0.1:${ETCD_PORT}" \
         --advertise-client-urls "http://127.0.0.1:${ETCD_PORT}" \
         --listen-peer-urls "http://127.0.0.1:2380" \
         --initial-advertise-peer-urls "http://127.0.0.1:2380" \
         --initial-cluster "e2e=http://127.0.0.1:2380" \
         --initial-cluster-state new \
         --enable-v2=false &> "$LOG/etcd.log" &
    echo $! > "$LOG/etcd.pid"
    started_etcd=1
    # 等就绪
    for i in {1..30}; do
        if curl -sf -m 1 -X POST "http://127.0.0.1:${ETCD_PORT}/v3beta/maintenance/status" -d '{}' >/dev/null 2>&1; then
            break
        fi
        sleep 0.2
    done
else
    echo "[topology] FATAL: etcd not running and not in PATH" >&2
    exit 1
fi

# --- 2 logic（MUDUO_IM_USE_ETCD=1）---
MUDUO_IM_USE_ETCD=1 MUDUO_IM_WORKER_ID=11 \
  MUDUO_IM_LOGIC_INSTANCE_ID=logic-A MUDUO_IM_LOGIC_ADVERTISE_HOST=127.0.0.1 \
  "$BUILD/muduo-im-logic" 0.0.0.0:9100 &> "$LOG/logic-a.log" &
echo $! > "$LOG/logic-a.pid"

MUDUO_IM_USE_ETCD=1 MUDUO_IM_WORKER_ID=12 \
  MUDUO_IM_LOGIC_INSTANCE_ID=logic-B MUDUO_IM_LOGIC_ADVERTISE_HOST=127.0.0.1 \
  "$BUILD/muduo-im-logic" 0.0.0.0:9101 &> "$LOG/logic-b.log" &
echo $! > "$LOG/logic-b.pid"

sleep 1.2

# --- 2 gateway（MUDUO_IM_USE_ETCD=1）---
MUDUO_IM_USE_ETCD=1 MUDUO_IM_GATEWAY_ID=gw-A \
  "$BUILD/muduo-im-gateway" 9091 &> "$LOG/gw-a.log" &
echo $! > "$LOG/gw-a.pid"

MUDUO_IM_USE_ETCD=1 MUDUO_IM_GATEWAY_ID=gw-B \
  "$BUILD/muduo-im-gateway" 9192 &> "$LOG/gw-b.log" &
echo $! > "$LOG/gw-b.pid"

# 等 gateway 把两个 logic 都拉进 pool（poll 间隔 1s）
sleep 2.5

echo "[topology] up. ports:"
ss -ltn | grep -E ':(9091|9192|9100|9101|2379)\s' || true

echo "[topology] etcd KV (services/logic/):"
curl -sf -X POST "http://127.0.0.1:${ETCD_PORT}/v3beta/kv/range" \
  -d "{\"key\":\"$(printf 'services/logic/' | base64)\",\"range_end\":\"$(printf 'services/logic0' | base64)\"}" \
  | python3 -c '
import sys, json, base64
j = json.load(sys.stdin)
for kv in j.get("kvs", []):
    k = base64.b64decode(kv["key"]).decode()
    v = base64.b64decode(kv["value"]).decode()
    print(f"  {k} -> {v}")
'

echo "[topology] gateway pool size:"
grep -c "add logic instance" "$LOG/gw-a.log" "$LOG/gw-b.log" || true

echo "[topology] running e2e..."
echo

python3 tests/grpc/topology_e2e.py
RC=$?

echo
echo "[topology] e2e exit=$RC"
if [[ $RC -ne 0 ]]; then
    echo "--- logic-a tail ---"; tail -10 "$LOG/logic-a.log" | grep -v MYSQL_OPT
    echo "--- logic-b tail ---"; tail -10 "$LOG/logic-b.log" | grep -v MYSQL_OPT
    echo "--- gw-a tail ---";    tail -15 "$LOG/gw-a.log"
    echo "--- gw-b tail ---";    tail -15 "$LOG/gw-b.log"
fi
exit $RC
