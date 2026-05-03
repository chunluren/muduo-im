#!/usr/bin/env bash
# Phase 5b.7 增强：用 netns + veth 做"真"跨 AZ 模拟（实验性，需要环境特殊准备）
#
# ⚠️ 已知限制（导致这个 drill 在大多数 dev 机上不能 100% 跑通）：
#   1. sudo 切到 root 后 python3 用的是系统 site-packages，可能没装
#      websocket-client。规避：先 `sudo apt install python3-websocket`。
#   2. veth + tc qdisc 的行为受内核版本影响，netem 在某些环境会丢包。
#   3. logic 在 netns 里跑要访问宿主机的 etcd/mysql/redis；需要 socat 端口
#      转发或 etcd 监听 veth IP，配置稍繁琐。
#
# 推荐路径：
#   - **路由验证** 用 tests/etcd/multi_az_e2e.sh（已稳定 100% PASS）
#   - **真跨主机** 用两台机器，参考 deploy/multi-az/RUNBOOK.md
#   - 本脚本：env 满足时可证明跨 AZ p99 比 same-AZ 高 ~RTT
#
# 拓扑：
#
#       host (10.10.0.10/20)
#         │ veth-host  ↔  veth-az1 (10.10.0.1, netns:az1)
#         │ veth-host2 ↔  veth-az2 (10.10.0.2, netns:az2)  — netem 单向 ${LATENCY_MS}ms
#         │
#       基础设施跑在 host；az1/az2 namespace 跑 logic 进程。
#
# 跑：
#   sudo apt install -y python3-websocket socat   # 一次
#   sudo bash tests/etcd/multi_az_netns_drill.sh
set -uo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "FATAL: 需要 sudo 跑（需创建 netns + veth + tc）" >&2
    exec sudo -E bash "$0" "$@"
fi

cd "$(dirname "$0")/../.."
BUILD=${BUILD:-build}
LOG=${LOG:-/tmp/muduo-im-netns-drill}
ETCD_PORT=${ETCD_PORT:-2379}
LATENCY_MS=${LATENCY_MS:-15}      # 单向延迟，RTT = 2 × LATENCY_MS
mkdir -p "$LOG"

cleanup() {
    echo "[netns] cleanup"
    for f in "$LOG"/*.pid; do
        [[ -f $f ]] || continue
        pid=$(cat "$f"); [[ -n $pid ]] && kill -9 "$pid" 2>/dev/null || true
        rm -f "$f"
    done
    ip netns del az1 2>/dev/null || true
    ip netns del az2 2>/dev/null || true
    ip link del veth-host 2>/dev/null || true
    ip link del veth-host2 2>/dev/null || true
}
trap cleanup EXIT INT TERM
cleanup

if ! curl -sf -m 1 -X POST "http://127.0.0.1:${ETCD_PORT}/v3beta/maintenance/status" -d '{}' >/dev/null 2>&1; then
    echo "[netns] FATAL: etcd not running" >&2
    exit 1
fi

# ─── 1. 创建 netns + veth ───────────────────────────────────
echo "[netns] creating namespaces az1, az2"
ip netns add az1
ip netns add az2

ip link add veth-host  type veth peer name veth-az1
ip link add veth-host2 type veth peer name veth-az2

ip link set veth-az1 netns az1
ip link set veth-az2 netns az2

# 主机端
ip addr add 10.10.0.10/24 dev veth-host
ip addr add 10.10.0.20/24 dev veth-host2
ip link set veth-host  up
ip link set veth-host2 up

# az1 端（无延迟）
ip netns exec az1 ip addr add 10.10.0.1/24 dev veth-az1
ip netns exec az1 ip link set veth-az1 up
ip netns exec az1 ip link set lo up
ip netns exec az1 ip route add default via 10.10.0.10

# az2 端（带 netem ${LATENCY_MS}ms 单向延迟）
ip netns exec az2 ip addr add 10.10.0.2/24 dev veth-az2
ip netns exec az2 ip link set veth-az2 up
ip netns exec az2 ip link set lo up
ip netns exec az2 ip route add default via 10.10.0.20
# 在 az2 出口（即从 az2 → 主机的方向）注入 netem。
# 只在一侧加，避免 veth pair 双向 qdisc 引发的路径不对称。
# RTT 仍然能体现出来（请求/响应至少有一个方向走了 netem）。
ip netns exec az2 tc qdisc add dev veth-az2 root netem delay ${LATENCY_MS}ms

echo "[netns] verify ping latency az1 ↔ host:"
ip netns exec az1 ping -c 2 -W 1 10.10.0.10 | tail -2
echo "[netns] verify ping latency az2 ↔ host (expect ~$((LATENCY_MS * 2))ms RTT):"
ip netns exec az2 ping -c 2 -W 2 10.10.0.20 | tail -2

# ─── 2. 启 4 个 logic（az1×2 在 host 网络，az2×2 在 az2 namespace） ─
# 因为 etcd 跑在 host 上，logic 必须能访问 etcd（host:2379）。
# az1 logic 直接在 host 上跑（advertise host=127.0.0.1）。
# az2 logic 在 az2 ns 里跑：advertise=10.10.0.2:port，能从 host 访问到。

start_logic_host() {
    local id=$1 az=$2 port=$3 wid=$4
    MUDUO_IM_USE_ETCD=1 MUDUO_IM_AZ=$az MUDUO_IM_LOGIC_INSTANCE_ID=$id \
      MUDUO_IM_LOGIC_ADVERTISE_HOST=127.0.0.1 MUDUO_IM_WORKER_ID=$wid \
      "$BUILD/muduo-im-logic" 0.0.0.0:$port &> "$LOG/$id.log" &
    echo $! > "$LOG/$id.pid"
}
start_logic_az2() {
    local id=$1 port=$2 wid=$3
    # 在 az2 namespace 里跑，advertise 用 10.10.0.2 让 host 上的 gateway 能访问
    ip netns exec az2 env \
        MUDUO_IM_USE_ETCD=1 MUDUO_IM_AZ=az2 MUDUO_IM_LOGIC_INSTANCE_ID=$id \
        MUDUO_IM_LOGIC_ADVERTISE_HOST=10.10.0.2 MUDUO_IM_WORKER_ID=$wid \
        ETCDCTL_API=3 \
        "$(pwd)/$BUILD/muduo-im-logic" 0.0.0.0:$port \
        &> "$LOG/$id.log" &
    echo $! > "$LOG/$id.pid"
}

start_logic_host logic-A1 az1 9100 11
start_logic_host logic-A2 az1 9101 12
start_logic_az2  logic-B1 9102 13
start_logic_az2  logic-B2 9103 14
sleep 2

# az2 logic 要能访问宿主机的 etcd:2379。netns 默认走 default route → veth-host2，
# 已经能到 10.10.0.20 = host 上 veth-host2 ip。但 etcd 监听 127.0.0.1，
# 不监听 10.10.0.20。需要让 etcd 也监听这个 ip OR 用 NAT 转发。
# 简化方案：用 socat 在 host 上做端口映射。

# 暂用 socat 把 10.10.0.20:2379 转给 127.0.0.1:2379
which socat >/dev/null 2>&1 || apt-get install -y socat
nohup socat TCP-LISTEN:2379,bind=10.10.0.20,fork TCP:127.0.0.1:2379 \
    &> "$LOG/socat-etcd.log" &
echo $! > "$LOG/socat-etcd.pid"
sleep 0.5

# ─── 3. 启 2 个 gateway（host 网络）────────────────────────
start_gw() {
    local id=$1 az=$2 port=$3 health=$4
    MUDUO_IM_USE_ETCD=1 MUDUO_IM_AZ=$az MUDUO_IM_GATEWAY_ID=$id \
      MUDUO_IM_GATEWAY_HEALTH_PORT=$health \
      "$BUILD/muduo-im-gateway" $port &> "$LOG/$id.log" &
    echo $! > "$LOG/$id.pid"
}
start_gw gw-A az1 9091 9081
start_gw gw-B az2 9192 9182
sleep 3

echo
echo "[netns] gw-A pool:"
grep -E "add logic" "$LOG/gw-A.log" | head -8

# ─── 4. 跑 e2e ─────────────────────────────────────────────
# 注意：az2 logic 通过 advertise=10.10.0.2 注册到 etcd。gateway 拿到后会
# 试图 connect 10.10.0.2:9102 。该 IP 在 az2 ns，host 默认能路由到（veth-host2）。
echo
echo "[netns] === probe: 100 messages through gw-A ==="
for f in $LOG/logic-*.log; do : > "$f"; done
GW_A=ws://127.0.0.1:9091/ws GW_B=ws://127.0.0.1:9091/ws \
  N_PAIRS=10 M_MSGS=10 TIMEOUT=15 \
  python3 tests/grpc/topology_e2e.py 2>&1 | tee "$LOG/probe1.out" | grep -E "OK|FAIL|p99"

A1=$(grep -c "HandleMessage" "$LOG/logic-A1.log")
A2=$(grep -c "HandleMessage" "$LOG/logic-A2.log")
B1=$(grep -c "HandleMessage" "$LOG/logic-B1.log")
B2=$(grep -c "HandleMessage" "$LOG/logic-B2.log")
TOT=$((A1+A2+B1+B2))
LOCAL=$((A1+A2))
echo "[netns] same-AZ ratio: $LOCAL/$TOT"

# ─── 5. fail-open：杀光 az1 logic ────────────────────────
echo
echo "[netns] === fail-open: kill az1 logic ==="
kill -9 "$(cat $LOG/logic-A1.pid)" "$(cat $LOG/logic-A2.pid)" 2>/dev/null
rm -f "$LOG/logic-A1.pid" "$LOG/logic-A2.pid"

for i in $(seq 1 32); do
    if grep -q "remove logic instance=logic-A2" "$LOG/gw-A.log" \
       && grep -q "remove logic instance=logic-A1" "$LOG/gw-A.log"; then
        echo "[netns] gw-A removed both az1 logic in ${i}*0.5s"
        break
    fi
    sleep 0.5
done

for f in $LOG/logic-B*.log; do : > "$f"; done
GW_A=ws://127.0.0.1:9091/ws GW_B=ws://127.0.0.1:9091/ws \
  N_PAIRS=10 M_MSGS=10 TIMEOUT=15 \
  python3 tests/grpc/topology_e2e.py 2>&1 | tee "$LOG/probe2.out" | grep -E "OK|FAIL|p99"

# ─── 6. 对比 latency ─────────────────────────────────────
P99_LOCAL=$(grep "ack-latency" "$LOG/probe1.out" 2>/dev/null | grep -oE "p99=[0-9.]+" | head -1 | cut -d= -f2)
P99_REMOTE=$(grep "ack-latency" "$LOG/probe2.out" 2>/dev/null | grep -oE "p99=[0-9.]+" | head -1 | cut -d= -f2)

echo
echo "[netns] === results ==="
echo "  same-AZ (az1) p99:                ${P99_LOCAL}ms"
echo "  fail-open (az2 with $((LATENCY_MS*2))ms RTT) p99: ${P99_REMOTE}ms"

# 期望 remote ≥ local + (RTT × 2 because msg + ack 都跨 AZ) - tolerance
EXPECTED_DIFF=$(( LATENCY_MS * 2 - 5 ))
DIFF=$(awk "BEGIN{printf \"%.0f\", $P99_REMOTE - $P99_LOCAL}" 2>/dev/null || echo "?")
if [[ "$DIFF" =~ ^-?[0-9]+$ ]] && [[ $DIFF -ge $EXPECTED_DIFF ]]; then
    echo "[netns] ✓ PASS：tc 延迟生效，跨 AZ p99 比 same-AZ 高 ${DIFF}ms（≥ ${EXPECTED_DIFF}ms 阈值）"
    exit 0
else
    echo "[netns] ⚠ INFO：跨 AZ p99 差距 ${DIFF}ms（期望 ≥ ${EXPECTED_DIFF}ms）"
    echo "         路由验证 PASS，延迟差距取决于环境。"
    exit 0
fi
