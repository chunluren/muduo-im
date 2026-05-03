#!/usr/bin/env bash
# Phase 4.2 滚动重启演练（tarball 模式）
#
# 持续 produce 1000 条 + 持续 consume，期间依次 SIGTERM/重启 broker-2/3/1。
# 期望：≥99% 投递成功；producer 错误数 ≤ 5。
set -uo pipefail
cd "$(dirname "$0")"

KAFKA_HOME=${KAFKA_HOME:-/home/ly/kafka/kafka_2.13-3.7.2}
DATA_ROOT=/tmp/kafka-cluster-tarball
BROKER_LIST=localhost:9092,localhost:9192,localhost:9292
WAIT_DOWN=${WAIT_DOWN:-15}
WAIT_UP=${WAIT_UP:-25}
LOG=${LOG:-/tmp/kafka-rolling-drill}
TOTAL_MSGS=${TOTAL_MSGS:-1000}
mkdir -p "$LOG"

# --- 0. 健康 ---
echo "[drill] checking 3 brokers..."
for i in 1 2 3; do
    PORT=$(( 9092 + (i-1)*100 ))
    if "$KAFKA_HOME/bin/kafka-broker-api-versions.sh" --bootstrap-server "localhost:$PORT" >/dev/null 2>&1; then
        echo "  broker-$i: OK (:$PORT)"
    else
        echo "[drill] FATAL: broker-$i not reachable on :$PORT" >&2
        exit 1
    fi
done

# topic 必须 RF=3
ISR_LINE=$("$KAFKA_HOME/bin/kafka-topics.sh" --bootstrap-server localhost:9092 \
    --describe --topic im.events 2>/dev/null | head -1)
if [[ -z "$ISR_LINE" ]]; then
    echo "[drill] FATAL: topic im.events 未创建。先跑 init-topics.sh" >&2
    exit 1
fi
echo "[drill] $ISR_LINE"

# --- 1. 后台 producer：从 stdin 读，每 10ms 一条 ---
echo "[drill] starting producer (target=$TOTAL_MSGS, ~100/s)"
PRODUCER_LOG="$LOG/producer.log"
(
  for i in $(seq 1 $TOTAL_MSGS); do
      echo "drill-msg-$i"
      sleep 0.01
  done
) | "$KAFKA_HOME/bin/kafka-console-producer.sh" \
    --bootstrap-server "$BROKER_LIST" \
    --topic im.events \
    --producer-property acks=all \
    --producer-property max.in.flight.requests.per.connection=5 \
    --producer-property enable.idempotence=true \
    --producer-property delivery.timeout.ms=120000 \
    --producer-property request.timeout.ms=30000 \
    > "$PRODUCER_LOG" 2>&1 &
PROD_PID=$!

# --- 2. 后台 consumer ---
CONSUMER_LOG="$LOG/consumer.log"
GROUP=drill-$(date +%s)
"$KAFKA_HOME/bin/kafka-console-consumer.sh" \
    --bootstrap-server "$BROKER_LIST" \
    --topic im.events \
    --group "$GROUP" \
    --from-beginning \
    --max-messages "$TOTAL_MSGS" \
    --timeout-ms 180000 \
    > "$CONSUMER_LOG" 2>&1 &
CONS_PID=$!

restart_broker() {
    local i=$1
    local PID_FILE="$DATA_ROOT/broker-$i.pid"
    local LOG_FILE="$DATA_ROOT/broker-$i.log"
    local PORT=$(( 9092 + (i-1)*100 ))
    local PID=$(cat "$PID_FILE" 2>/dev/null || echo "")

    echo
    echo "[drill] === stopping broker-$i (pid=$PID) — down=${WAIT_DOWN}s ==="
    [[ -n "$PID" ]] && kill -TERM "$PID" || true
    # 等真正下线
    for t in $(seq 1 30); do
        if ! nc -z localhost "$PORT" 2>/dev/null; then break; fi
        sleep 0.5
    done

    # producer 应继续工作
    PROD_LINES_BEFORE=$(wc -l < "$PRODUCER_LOG" 2>/dev/null || echo 0)
    sleep "$WAIT_DOWN"
    PROD_LINES_AFTER=$(wc -l < "$PRODUCER_LOG" 2>/dev/null || echo 0)
    DELTA=$(( PROD_LINES_AFTER - PROD_LINES_BEFORE ))
    echo "[drill] broker-$i down for ${WAIT_DOWN}s — producer wrote $DELTA more lines"

    echo "[drill] === starting broker-$i (catch-up=${WAIT_UP}s) ==="
    nohup "$KAFKA_HOME/bin/kafka-server-start.sh" "$(pwd)/configs/server-$i.properties" \
        > "$LOG_FILE" 2>&1 &
    echo $! > "$PID_FILE"

    # 等就绪
    for t in $(seq 1 60); do
        if "$KAFKA_HOME/bin/kafka-broker-api-versions.sh" --bootstrap-server "localhost:$PORT" >/dev/null 2>&1; then
            echo "[drill] broker-$i back up on :$PORT"
            break
        fi
        sleep 1
    done
    sleep "$WAIT_UP"  # 给 ISR 收敛时间
}

sleep 3  # 暖一下
restart_broker 2
restart_broker 3
restart_broker 1

echo
echo "[drill] waiting for producer/consumer to drain..."
wait $PROD_PID 2>/dev/null || true
wait $CONS_PID 2>/dev/null || true

PRODUCED=$(grep -cE '^drill-msg-' "$PRODUCER_LOG" 2>/dev/null | grep -oE "^[0-9]+" || echo 0)
CONSUMED=$(grep -cE '^drill-msg-' "$CONSUMER_LOG" 2>/dev/null | grep -oE "^[0-9]+" || echo 0)
ERRORS=$(grep -cE 'ERROR|Failed |FATAL' "$PRODUCER_LOG" 2>/dev/null | grep -oE "^[0-9]+" || echo 0)
PRODUCED=${PRODUCED:-0}
CONSUMED=${CONSUMED:-0}
ERRORS=${ERRORS:-0}

echo
echo "[drill] === RESULTS ==="
echo "  produced (input): $TOTAL_MSGS"
echo "  consumed:         $CONSUMED"
echo "  producer errors:  $ERRORS"
LOSS=$(( TOTAL_MSGS - CONSUMED ))
echo "  apparent loss:    $LOSS"

THRESHOLD=$(( TOTAL_MSGS * 99 / 100 ))
if [[ $CONSUMED -ge $THRESHOLD ]]; then
    echo "[drill] PASS: consumed ≥99% ($CONSUMED ≥ $THRESHOLD)"
    exit 0
else
    echo "[drill] FAIL: consumed only $CONSUMED, threshold $THRESHOLD"
    echo "--- producer log tail ---"; tail -20 "$PRODUCER_LOG"
    echo "--- consumer log tail ---"; tail -10 "$CONSUMER_LOG"
    exit 1
fi
