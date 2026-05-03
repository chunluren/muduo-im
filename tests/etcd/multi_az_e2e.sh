#!/usr/bin/env bash
# Phase 5: 多 AZ 路由 e2e
#
# 拓扑：
#   az1: logic-A1 (9100), logic-A2 (9101); gateway gw-A (9091)
#   az2: logic-B1 (9102), logic-B2 (9103); gateway gw-B (9192)
#
# 验：
#   1. 200 条消息通过 gw-A → ≥ 95% 落到 az1 logic
#   2. 杀光 az1 logic → 通过 gw-A 仍能发，全部 fail-open 到 az2
set -uo pipefail
cd "$(dirname "$0")/../.."

BUILD=${BUILD:-build}
LOG=${LOG:-/tmp/muduo-im-multi-az}
ETCD_PORT=${ETCD_PORT:-2379}
mkdir -p "$LOG"

cleanup() {
    echo "[multi-az] cleanup"
    for f in "$LOG"/*.pid; do
        [[ -f $f ]] || continue
        pid=$(cat "$f"); [[ -n $pid ]] && kill -9 "$pid" 2>/dev/null || true
        rm -f "$f"
    done
    sleep 0.3
}
trap cleanup EXIT INT TERM
cleanup

if ! curl -sf -m 1 -X POST "http://127.0.0.1:${ETCD_PORT}/v3beta/maintenance/status" -d '{}' >/dev/null 2>&1; then
    echo "[multi-az] FATAL: etcd not running" >&2; exit 1
fi

start_logic() {
    local id=$1 az=$2 port=$3 wid=$4
    MUDUO_IM_USE_ETCD=1 MUDUO_IM_AZ=$az MUDUO_IM_LOGIC_INSTANCE_ID=$id \
      MUDUO_IM_WORKER_ID=$wid \
      "$BUILD/muduo-im-logic" 0.0.0.0:$port &> "$LOG/$id.log" &
    echo $! > "$LOG/$id.pid"
}
start_logic logic-A1 az1 9100 11
start_logic logic-A2 az1 9101 12
start_logic logic-B1 az2 9102 13
start_logic logic-B2 az2 9103 14
sleep 1.5

start_gw() {
    local id=$1 az=$2 port=$3
    MUDUO_IM_USE_ETCD=1 MUDUO_IM_AZ=$az MUDUO_IM_GATEWAY_ID=$id \
      "$BUILD/muduo-im-gateway" $port &> "$LOG/$id.log" &
    echo $! > "$LOG/$id.pid"
}
start_gw gw-A az1 9091
start_gw gw-B az2 9192
sleep 2.5

echo "[multi-az] gw-A pool:"
grep -E "add logic|localAz" "$LOG/gw-A.log" | head -6
echo "[multi-az] gw-B pool:"
grep -E "add logic|localAz" "$LOG/gw-B.log" | head -6

# Test 1: through gw-A → expect ≥ 95% to az1
echo
echo "[multi-az] === test 1: same-AZ preference (200 msgs through gw-A) ==="
for f in $LOG/logic-*.log; do : > "$f"; done
GW_A=ws://127.0.0.1:9091/ws GW_B=ws://127.0.0.1:9091/ws \
  N_PAIRS=20 M_MSGS=10 TIMEOUT=10 \
  python3 tests/grpc/topology_e2e.py 2>&1 | grep -E "OK|FAIL"
A1=$(grep -c "HandleMessage" "$LOG/logic-A1.log")
A2=$(grep -c "HandleMessage" "$LOG/logic-A2.log")
B1=$(grep -c "HandleMessage" "$LOG/logic-B1.log")
B2=$(grep -c "HandleMessage" "$LOG/logic-B2.log")
AZ1=$((A1+A2)); AZ2=$((B1+B2)); TOT=$((AZ1+AZ2))
echo "[multi-az] az1=$AZ1 az2=$AZ2 total=$TOT"
if [[ $TOT -gt 0 ]] && [[ $((AZ1 * 100 / TOT)) -ge 95 ]]; then
    echo "[multi-az] ✓ same-AZ ratio $((AZ1 * 100 / TOT))% ≥ 95%"
else
    echo "[multi-az] ✗ same-AZ ratio too low"; exit 1
fi

# Test 2: kill az1 logic, expect all to go to az2 (fail-open)
echo
echo "[multi-az] === test 2: fail-open to az2 (kill az1 logic) ==="
PID_A1=$(cat "$LOG/logic-A1.pid")
PID_A2=$(cat "$LOG/logic-A2.pid")
kill -9 $PID_A1 $PID_A2
rm -f "$LOG/logic-A1.pid" "$LOG/logic-A2.pid"
echo "[multi-az] az1 logic killed; waiting for lease expire (≤ 16s)..."
for i in $(seq 1 32); do
    if grep -q "remove logic instance=logic-A2" "$LOG/gw-A.log" \
       && grep -q "remove logic instance=logic-A1" "$LOG/gw-A.log"; then
        echo "[multi-az] gw-A removed both az1 logic in ${i}*0.5s"
        break
    fi
    sleep 0.5
done

for f in $LOG/logic-B*.log; do : > "$f"; done
GW_A=ws://127.0.0.1:9091/ws GW_B=ws://127.0.0.1:9091/ws \
  N_PAIRS=10 M_MSGS=5 TIMEOUT=10 \
  python3 tests/grpc/topology_e2e.py 2>&1 | grep -E "OK|FAIL"
B1=$(grep -c "HandleMessage" "$LOG/logic-B1.log")
B2=$(grep -c "HandleMessage" "$LOG/logic-B2.log")
B_TOT=$((B1+B2))
if [[ $B_TOT -ge 45 ]]; then  # 50 sent; 容忍 5
    echo "[multi-az] ✓ fail-open: $B_TOT msgs to az2 (B1=$B1 B2=$B2)"
else
    echo "[multi-az] ✗ fail-open broke: only $B_TOT msgs to az2"; exit 1
fi

echo
echo "[multi-az] PASS"
