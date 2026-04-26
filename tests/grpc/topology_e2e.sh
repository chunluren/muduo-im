#!/usr/bin/env bash
# Phase 1.2 W3.D3-D4: 集成 + 简单压测
# 起 1 registry + 2 logic + 2 gateway，跑 Python e2e 验证消息互通 & 延迟
#
# 用法（在 build/ 目录已编出 muduo-im-{registry,logic,gateway} 之后）：
#   tests/grpc/topology_e2e.sh
# 期望：MySQL 可连（默认 root@127.0.0.1, db=muduo_im），Redis 可选
set -uo pipefail
cd "$(dirname "$0")/../.."

BUILD=${BUILD:-build}
LOG=${LOG:-/tmp/muduo-im-topology}
mkdir -p "$LOG"

cleanup() {
    echo "[topology] cleanup"
    for f in "$LOG"/*.pid; do
        [[ -f $f ]] || continue
        pid=$(cat "$f"); [[ -n $pid ]] && kill "$pid" 2>/dev/null || true
        rm -f "$f"
    done
    sleep 0.3
}
trap cleanup EXIT INT TERM

cleanup  # in case prior run left things

# --- registry ---
"$BUILD/muduo-im-registry" 0.0.0.0:9200 &> "$LOG/registry.log" &
echo $! > "$LOG/registry.pid"
sleep 0.4

# --- 2 logic ---
MUDUO_IM_WORKER_ID=11 MUDUO_IM_REGISTRY_ADDR=127.0.0.1:9200 \
  MUDUO_IM_LOGIC_INSTANCE_ID=logic-A MUDUO_IM_LOGIC_ADVERTISE_HOST=127.0.0.1 \
  "$BUILD/muduo-im-logic" 0.0.0.0:9100 &> "$LOG/logic-a.log" &
echo $! > "$LOG/logic-a.pid"

MUDUO_IM_WORKER_ID=12 MUDUO_IM_REGISTRY_ADDR=127.0.0.1:9200 \
  MUDUO_IM_LOGIC_INSTANCE_ID=logic-B MUDUO_IM_LOGIC_ADVERTISE_HOST=127.0.0.1 \
  "$BUILD/muduo-im-logic" 0.0.0.0:9101 &> "$LOG/logic-b.log" &
echo $! > "$LOG/logic-b.pid"

sleep 1

# --- 2 gateways ---
MUDUO_IM_GATEWAY_ID=gw-A MUDUO_IM_REGISTRY_ADDR=127.0.0.1:9200 \
  "$BUILD/muduo-im-gateway" 9091 &> "$LOG/gw-a.log" &
echo $! > "$LOG/gw-a.pid"

MUDUO_IM_GATEWAY_ID=gw-B MUDUO_IM_REGISTRY_ADDR=127.0.0.1:9200 \
  "$BUILD/muduo-im-gateway" 9092 &> "$LOG/gw-b.log" &
echo $! > "$LOG/gw-b.pid"

sleep 1.5

echo "[topology] up. ports:"
ss -ltn | grep -E ':(9091|9092|9100|9101|9200)\s' || true
echo "[topology] running e2e..."
echo

python3 tests/grpc/topology_e2e.py
RC=$?

echo
echo "[topology] e2e exit=$RC"
if [[ $RC -ne 0 ]]; then
    echo "--- registry tail ---"; tail -10 "$LOG/registry.log"
    echo "--- logic-a tail ---"; tail -10 "$LOG/logic-a.log" | grep -v MYSQL_OPT
    echo "--- logic-b tail ---"; tail -10 "$LOG/logic-b.log" | grep -v MYSQL_OPT
    echo "--- gw-a tail ---";    tail -15 "$LOG/gw-a.log"
    echo "--- gw-b tail ---";    tail -15 "$LOG/gw-b.log"
fi
exit $RC
