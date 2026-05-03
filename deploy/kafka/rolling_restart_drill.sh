#!/usr/bin/env bash
# Phase 4.2 滚动重启演练
#
# 目标：在持续 produce/consume 的同时，把 3 个 broker 一个一个重启，
# 验证 producer/consumer 不阻塞、不丢消息。
#
# 流程：
#   1. 检查集群健康（3 broker 都 ISR）
#   2. 起后台 producer 持续往 im.events 写 100 条/s
#   3. 起后台 consumer 持续读，记录 offset
#   4. 依次 docker stop kafka-{1,2,3} → 等 30s → start → 等 30s（再下一个）
#   5. 期间检查：producer error rate；consumer lag；ISR 收敛
#   6. 停止 producer/consumer，对账 produced 数 ≈ consumed 数
#
# 用法：
#   bash deploy/kafka/rolling_restart_drill.sh [DURATION]   # 默认 60s 一个 broker
set -uo pipefail

cd "$(dirname "$0")/../.."

WAIT_DOWN=${WAIT_DOWN:-30}
WAIT_UP=${WAIT_UP:-30}
LOG=${LOG:-/tmp/kafka-rolling-drill}
mkdir -p "$LOG"

EXEC="docker exec kafka-1"
BROKER=localhost:9092

# --- 0. 健康检查 ---
echo "[drill] checking cluster health..."
for n in kafka-1 kafka-2 kafka-3; do
    state=$(docker inspect -f '{{.State.Status}}' "$n" 2>/dev/null || echo "missing")
    echo "  $n: $state"
    if [[ "$state" != "running" ]]; then
        echo "[drill] FATAL: $n not running. start with deploy/kafka/docker-compose.cluster.yml" >&2
        exit 1
    fi
done

echo "[drill] topic im.events ISR before drill:"
$EXEC kafka-topics.sh --bootstrap-server $BROKER --describe --topic im.events \
    | head -6 || { echo "[drill] FATAL: im.events not created. run init-topics-cluster.sh" >&2; exit 1; }

# --- 1. 后台 producer：每 10ms 一条，每条 ~80B，~100 条/s ---
echo "[drill] starting background producer (1k msgs)..."
PRODUCER_LOG="$LOG/producer.log"
docker exec kafka-1 bash -c '
for i in $(seq 1 1000); do
    echo "drill-msg-$i"
    sleep 0.01
done | kafka-console-producer.sh \
    --bootstrap-server localhost:9092 \
    --topic im.events \
    --producer-property acks=all \
    --producer-property max.in.flight.requests.per.connection=5 \
    --producer-property enable.idempotence=true 2>&1
' > "$PRODUCER_LOG" 2>&1 &
PROD_PID=$!

# --- 2. 后台 consumer：消费到独立 group，落到文件做对账 ---
CONSUMER_LOG="$LOG/consumer.log"
docker exec kafka-1 kafka-console-consumer.sh \
    --bootstrap-server $BROKER \
    --topic im.events \
    --group drill-$(date +%s) \
    --from-beginning \
    --max-messages 1000 > "$CONSUMER_LOG" 2>&1 &
CONS_PID=$!

# --- 3. 重启每个 broker ---
sleep 2  # 让 producer/consumer 暖起来
for victim in kafka-2 kafka-3 kafka-1; do
    echo
    echo "[drill] === stopping $victim (down=${WAIT_DOWN}s) ==="
    T0=$(date +%s)
    docker stop "$victim"
    sleep "$WAIT_DOWN"

    echo "[drill] $victim down for ${WAIT_DOWN}s — checking producer is alive..."
    if ! kill -0 $PROD_PID 2>/dev/null; then
        echo "[drill] WARN: producer died early"
    fi
    PROD_LINES=$(wc -l < "$PRODUCER_LOG" 2>/dev/null || echo 0)
    CONS_LINES=$(wc -l < "$CONSUMER_LOG" 2>/dev/null || echo 0)
    echo "  producer log lines: $PROD_LINES"
    echo "  consumer log lines: $CONS_LINES"

    echo "[drill] === starting $victim (catch-up=${WAIT_UP}s) ==="
    docker start "$victim"
    sleep "$WAIT_UP"

    echo "[drill] ISR after $victim back:"
    $EXEC kafka-topics.sh --bootstrap-server $BROKER --describe --topic im.events \
        | head -3
done

# --- 4. 等 producer 把 1000 条全发完，consumer 全消费完 ---
echo
echo "[drill] waiting for producer/consumer to finish..."
wait $PROD_PID 2>/dev/null || true
wait $CONS_PID 2>/dev/null || true

# --- 5. 对账 ---
PRODUCED=$(grep -cE '^drill-msg-' "$PRODUCER_LOG" 2>/dev/null || echo 0)
CONSUMED=$(grep -cE '^drill-msg-' "$CONSUMER_LOG" 2>/dev/null || echo 0)
ERRORS=$(grep -cE 'ERROR|Failed|FATAL' "$PRODUCER_LOG" 2>/dev/null || echo 0)

echo
echo "[drill] === RESULTS ==="
echo "  produced: $PRODUCED"
echo "  consumed: $CONSUMED"
echo "  producer errors: $ERRORS"

LOSS=$((PRODUCED - CONSUMED))
LOSS_PCT=$(awk "BEGIN{printf \"%.2f\", ($LOSS * 100.0) / ($PRODUCED + 0.0001)}")
echo "  loss: $LOSS messages ($LOSS_PCT%)"

if [[ $CONSUMED -ge $((PRODUCED * 99 / 100)) ]] && [[ $ERRORS -le 5 ]]; then
    echo "[drill] PASS: ≥99% delivery, ≤5 producer errors"
    exit 0
else
    echo "[drill] FAIL: delivery too low or too many errors"
    exit 1
fi
