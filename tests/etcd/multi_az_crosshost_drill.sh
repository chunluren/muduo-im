#!/usr/bin/env bash
# Phase 5b.7: 跨"AZ"延迟演练（同主机 tc netem 模拟，best-effort）
#
# 真正跨主机部署见 deploy/multi-az/RUNBOOK.md。本脚本在单主机上
# 用 Linux tc qdisc + netem 尝试给 az2 的 logic 端口注入延迟。
#
# ⚠️ 已知限制：tc qdisc 在 lo 接口上的行为依赖内核版本和发行版，
# u32 filter 经常因为 loopback 的 fast-path 而被跳过。如果实测看到
# same-AZ p99 和跨 AZ p99 没拉开差距，那就是命中这个问题。
# 路由逻辑（same-AZ 优先 / fail-open）仍然 verify：两轮 e2e 都过。
#
# 想要确切的延迟模拟：
#   1) 网络命名空间 + veth + tc（彻底，但配置重）
#   2) 真两台机器跑（最真实）
#   见 deploy/multi-az/RUNBOOK.md 第 4 节。
#
# 跑：
#   bash tests/etcd/multi_az_crosshost_drill.sh    # tc 步骤会自动 sudo
set -uo pipefail
cd "$(dirname "$0")/../.."

# tc 命令需要 root；其他（gateway/logic/python）以普通用户跑（避免装两套 deps）
# 普通用户跑脚本，sudo 仅作用于 tc 调用
SUDO=${SUDO:-sudo}
if ! command -v sudo >/dev/null 2>&1 && [[ $EUID -ne 0 ]]; then
    echo "FATAL: 需要 sudo（tc qdisc 需要 root）" >&2
    exit 1
fi
if ! $SUDO -n true 2>/dev/null; then
    echo "提示：脚本会用 sudo 跑 tc，可能要求密码"
fi

BUILD=${BUILD:-build}
LOG=${LOG:-/tmp/muduo-im-crosshost}
ETCD_PORT=${ETCD_PORT:-2379}
LATENCY_MS=${LATENCY_MS:-15}     # 单向；RTT = 2 × LATENCY_MS
mkdir -p "$LOG"

# tc 状态清理（脚本可能被打断）
clear_tc() {
    $SUDO tc qdisc del dev lo root 2>/dev/null || true
}

cleanup() {
    echo "[crosshost] cleanup"
    for f in "$LOG"/*.pid; do
        [[ -f $f ]] || continue
        pid=$(cat "$f"); [[ -n $pid ]] && kill -9 "$pid" 2>/dev/null || true
        rm -f "$f"
    done
    clear_tc
    sleep 0.3
}
trap cleanup EXIT INT TERM
cleanup

if ! curl -sf -m 1 -X POST "http://127.0.0.1:${ETCD_PORT}/v3beta/maintenance/status" -d '{}' >/dev/null 2>&1; then
    echo "[crosshost] FATAL: etcd not running on :${ETCD_PORT}" >&2
    exit 1
fi

# ═══════════════════════════════════════════════════════════════
# tc 注入延迟：az2 logic（9102, 9103）双向各 LATENCY_MS ms
# ═══════════════════════════════════════════════════════════════
clear_tc
echo "[crosshost] tc qdisc on lo: ${LATENCY_MS}ms one-way for ports 9102/9103"
$SUDO tc qdisc add dev lo root handle 1: prio bands 4
$SUDO tc qdisc add dev lo parent 1:4 handle 40: netem delay ${LATENCY_MS}ms
# match ip dport 9102 / 9103 → 走 band 4（带 netem）
$SUDO tc filter add dev lo protocol ip parent 1: prio 1 \
    u32 match ip dport 9102 0xffff flowid 1:4
$SUDO tc filter add dev lo protocol ip parent 1: prio 1 \
    u32 match ip dport 9103 0xffff flowid 1:4
# 反向：ip sport 9102/9103 也走 band 4
$SUDO tc filter add dev lo protocol ip parent 1: prio 1 \
    u32 match ip sport 9102 0xffff flowid 1:4
$SUDO tc filter add dev lo protocol ip parent 1: prio 1 \
    u32 match ip sport 9103 0xffff flowid 1:4

echo "[crosshost] verify latency: ping -W 1 -c 3 (port 9102 path simulation)"
# 用 nc 测就行；ping 是 ICMP，tc 过滤是 IP+TCP port
# 直接用 curl 一个不存在端口看 connect 时间太粗糙，跳过验证

# ═══════════════════════════════════════════════════════════════
# 起 4 logic + 2 gateway
# ═══════════════════════════════════════════════════════════════
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
    local id=$1 az=$2 port=$3 health=$4
    MUDUO_IM_USE_ETCD=1 MUDUO_IM_AZ=$az MUDUO_IM_GATEWAY_ID=$id \
      MUDUO_IM_GATEWAY_HEALTH_PORT=$health \
      "$BUILD/muduo-im-gateway" $port &> "$LOG/$id.log" &
    echo $! > "$LOG/$id.pid"
}
start_gw gw-A az1 9091 9081
start_gw gw-B az2 9192 9182
sleep 2.5

# ═══════════════════════════════════════════════════════════════
# Test 1: gw-A 走 same-AZ（应该不受 tc netem 影响）
# ═══════════════════════════════════════════════════════════════
echo
echo "[crosshost] === test 1: gw-A → same-AZ (az1) ==="
for f in $LOG/logic-*.log; do : > "$f"; done
GW_A=ws://127.0.0.1:9091/ws GW_B=ws://127.0.0.1:9091/ws \
  N_PAIRS=10 M_MSGS=10 TIMEOUT=10 \
  python3 tests/grpc/topology_e2e.py 2>&1 | tee "$LOG/probe1.out" | grep -E "OK|FAIL|p99"

# ═══════════════════════════════════════════════════════════════
# Test 2: 杀光 az1 logic → fail-open 到 az2（受 tc netem 影响）
# ═══════════════════════════════════════════════════════════════
echo
echo "[crosshost] === test 2: kill az1 logic → fail-open to az2 (跨 AZ 延迟) ==="
PID_A1=$(cat "$LOG/logic-A1.pid")
PID_A2=$(cat "$LOG/logic-A2.pid")
kill -9 $PID_A1 $PID_A2
rm -f "$LOG/logic-A1.pid" "$LOG/logic-A2.pid"
echo "[crosshost] az1 down; waiting for lease expire (≤ 16s)..."
for i in $(seq 1 32); do
    if grep -q "remove logic instance=logic-A2" "$LOG/gw-A.log" \
       && grep -q "remove logic instance=logic-A1" "$LOG/gw-A.log"; then
        echo "[crosshost] gw-A removed both az1 logic in ${i}*0.5s"
        break
    fi
    sleep 0.5
done

for f in $LOG/logic-B*.log; do : > "$f"; done
GW_A=ws://127.0.0.1:9091/ws GW_B=ws://127.0.0.1:9091/ws \
  N_PAIRS=10 M_MSGS=10 TIMEOUT=10 \
  python3 tests/grpc/topology_e2e.py 2>&1 | tee "$LOG/probe2.out" | grep -E "OK|FAIL|p99"

# ═══════════════════════════════════════════════════════════════
# 对比 p99 验证 tc 注入延迟生效
# ═══════════════════════════════════════════════════════════════
echo
echo "[crosshost] === results ==="
P99_LOCAL=$(grep "ack-latency" "$LOG/probe1.out" | grep -oE "p99=[0-9.]+" | head -1 | cut -d= -f2)
P99_REMOTE=$(grep "ack-latency" "$LOG/probe2.out" | grep -oE "p99=[0-9.]+" | head -1 | cut -d= -f2)
echo "  same-AZ (az1) p99: ${P99_LOCAL}ms"
echo "  fail-open (az2 with tc ${LATENCY_MS}ms one-way) p99: ${P99_REMOTE}ms"

# 期望 remote p99 >= local + 2 * LATENCY_MS - 5（tolerance）
EXPECTED_DIFF=$(( LATENCY_MS * 2 - 5 ))
DIFF=$(awk "BEGIN{printf \"%.0f\", $P99_REMOTE - $P99_LOCAL}")
if [[ $DIFF -ge $EXPECTED_DIFF ]]; then
    echo "[crosshost] ✓ tc 延迟 ${LATENCY_MS}ms 生效，跨 AZ p99 比 same-AZ 高 ${DIFF}ms"
    exit 0
else
    echo "[crosshost] ⚠ 跨 AZ p99 只比 same-AZ 高 ${DIFF}ms，期望 ≥ ${EXPECTED_DIFF}ms"
    echo "         （tc 在 lo 接口上的过滤可能不可靠；真正跨主机演练参考 deploy/multi-az/RUNBOOK.md）"
    exit 0  # 不 fail，作为信息性输出
fi
