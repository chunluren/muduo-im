#!/usr/bin/env bash
# Phase 5 MirrorMaker 2 演练
#
# 流程：
#   1. 在 az1 produce 100 条到 mm2-test.events
#   2. 等 MM2 把它们镜像到 az2（会变成 az1.mm2-test.events）
#   3. 在 az2 consume，对账数量
#
# 跑前提：bash up.sh 已起完
set -uo pipefail
KAFKA_HOME=${KAFKA_HOME:-/home/ly/kafka/kafka_2.13-3.7.2}
TOTAL_MSGS=${TOTAL_MSGS:-100}

# 健康
for port in 9492 9592; do
    if ! "$KAFKA_HOME/bin/kafka-broker-api-versions.sh" --bootstrap-server "localhost:$port" >/dev/null 2>&1; then
        echo "FATAL: broker on :$port not reachable" >&2
        exit 1
    fi
done

echo "[drill] producing $TOTAL_MSGS messages to az1: mm2-test.events"
for i in $(seq 1 $TOTAL_MSGS); do
    echo "mm2-msg-$i"
done | "$KAFKA_HOME/bin/kafka-console-producer.sh" \
        --bootstrap-server localhost:9492 \
        --topic mm2-test.events 2>/dev/null
echo "[drill] produce done"

# 等 MM2 同步
echo "[drill] waiting 10s for MM2 to mirror..."
sleep 10

# az2 应当有镜像后的 topic：az1.mm2-test.events
echo
echo "[drill] az2 topic list:"
"$KAFKA_HOME/bin/kafka-topics.sh" --bootstrap-server localhost:9592 --list \
    | grep -E "mm2-test|az1\." | head -10

echo
echo "[drill] consume from az2 (az1.mm2-test.events):"
CONSUMED=$("$KAFKA_HOME/bin/kafka-console-consumer.sh" \
    --bootstrap-server localhost:9592 \
    --topic az1.mm2-test.events \
    --from-beginning \
    --max-messages "$TOTAL_MSGS" \
    --timeout-ms 30000 2>/dev/null \
    | grep -cE '^mm2-msg-' || echo 0)

echo
echo "[drill] === RESULT ==="
echo "  produced (az1): $TOTAL_MSGS"
echo "  consumed (az2): $CONSUMED"

if [[ $CONSUMED -ge $((TOTAL_MSGS * 99 / 100)) ]]; then
    echo "[drill] PASS: ≥99% mirrored ($CONSUMED / $TOTAL_MSGS)"
    exit 0
else
    echo "[drill] FAIL: 只镜像了 $CONSUMED / $TOTAL_MSGS"
    echo "--- mm2 log tail ---"
    tail -30 /tmp/mm2-tarball/mm2.log
    exit 1
fi
