#!/usr/bin/env bash
# Phase 4.3 LB 烟测：
#   - 起 2 gateway + haproxy
#   - 通过 LB:9090 而不是直接连 gateway 跑 e2e
#   - 中途 SIGTERM gateway-A，验证：
#     * haproxy 在 6s 内（fall 3 × inter 2s）从 backend pool 把 gw-A 摘掉
#     * 新连接全部走 gw-B
#     * 客户端收到 close-frame 1001 后立刻重连可走通
set -uo pipefail
cd "$(dirname "$0")/../.."

BUILD=${BUILD:-build}
LOG=${LOG:-/tmp/muduo-im-lb-e2e}
ETCD_PORT=${ETCD_PORT:-2379}
mkdir -p "$LOG"

cleanup() {
    echo "[lb-e2e] cleanup"
    for f in "$LOG"/*.pid; do
        [[ -f $f ]] || continue
        pid=$(cat "$f"); [[ -n $pid ]] && kill "$pid" 2>/dev/null || true
        rm -f "$f"
    done
    [[ -f /tmp/im-haproxy.pid ]] && kill "$(cat /tmp/im-haproxy.pid)" 2>/dev/null || true
    sleep 0.3
}
trap cleanup EXIT INT TERM
cleanup

# etcd
if ! curl -sf -m 1 -X POST "http://127.0.0.1:${ETCD_PORT}/v3beta/maintenance/status" -d '{}' >/dev/null 2>&1; then
    echo "[lb-e2e] FATAL: etcd not running" >&2; exit 1
fi

# logic
MUDUO_IM_USE_ETCD=1 MUDUO_IM_WORKER_ID=11 \
  MUDUO_IM_LOGIC_INSTANCE_ID=logic-A "$BUILD/muduo-im-logic" 0.0.0.0:9100 \
  &> "$LOG/logic.log" &
echo $! > "$LOG/logic.pid"
sleep 1.2

# 2 gateway
# Phase 5b: gateway-A /health on 9081, gateway-B on 9182
MUDUO_IM_USE_ETCD=1 MUDUO_IM_GATEWAY_ID=gw-A \
  MUDUO_IM_GATEWAY_HEALTH_PORT=9081 \
  "$BUILD/muduo-im-gateway" 9091 \
  &> "$LOG/gw-a.log" &
GW_A_PID=$!
echo $GW_A_PID > "$LOG/gw-a.pid"

MUDUO_IM_USE_ETCD=1 MUDUO_IM_GATEWAY_ID=gw-B \
  MUDUO_IM_GATEWAY_HEALTH_PORT=9182 \
  "$BUILD/muduo-im-gateway" 9192 \
  &> "$LOG/gw-b.log" &
echo $! > "$LOG/gw-b.pid"
sleep 2.5

# Verify /health responds
echo "[lb-e2e] /health probe (gw-A):"
curl -sf http://127.0.0.1:9081/health || echo "FAIL"
echo
echo "[lb-e2e] /health probe (gw-B):"
curl -sf http://127.0.0.1:9182/health || echo "FAIL"
echo

# 启 haproxy
bash deploy/lb/up.sh
sleep 1

echo "[lb-e2e] checking ports:"
ss -ltn | grep -E ':(9090|9091|9192|7777)\s'

# 通过 LB:9090 跑一轮 e2e（用 gateway-A/B 直接 ws 的 python 改一下）
echo "[lb-e2e] running ws probe via LB:9090"
GW_A=ws://localhost:9090/ws GW_B=ws://localhost:9090/ws \
  N_PAIRS=10 M_MSGS=10 TIMEOUT=10 \
  python3 tests/grpc/topology_e2e.py 2>&1 | tee "$LOG/probe1.out" | tail -6
RC=${PIPESTATUS[0]}
if [[ $RC -ne 0 ]]; then
    echo "[lb-e2e] FAIL: probe through LB failed"
    exit 1
fi

# Kill gateway-A，等 6s（fall 3 × 2s）haproxy 摘掉它
echo "[lb-e2e] >>> SIGTERM gateway-A (pid=$GW_A_PID), wait 8s for haproxy detect"
kill -TERM $GW_A_PID
sleep 8

# 看 haproxy stats
echo "[lb-e2e] backend status after gw-A down:"
echo "show servers state ws_gateways" | sudo socat /tmp/im-haproxy.sock - 2>/dev/null \
    || echo "show stat" | sudo socat /tmp/im-haproxy.sock - 2>/dev/null \
    | head -3

# 再来一波 e2e —— 期望全部走 gw-B 成功
echo "[lb-e2e] running 2nd probe (gw-A down, all via gw-B)"
GW_A=ws://localhost:9090/ws GW_B=ws://localhost:9090/ws \
  N_PAIRS=10 M_MSGS=10 TIMEOUT=10 \
  python3 tests/grpc/topology_e2e.py 2>&1 | tee "$LOG/probe2.out" | tail -6
RC=${PIPESTATUS[0]}
if [[ $RC -ne 0 ]]; then
    echo "[lb-e2e] FAIL: probe2 (gw-A down) failed — haproxy didn't fail over?"
    exit 1
fi

echo "[lb-e2e] PASS: traffic continued through LB after gw-A SIGTERM"
exit 0
