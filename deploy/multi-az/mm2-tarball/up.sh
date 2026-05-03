#!/usr/bin/env bash
# Phase 5 跨 AZ Kafka MirrorMaker 2 演练 — 双 broker（每"AZ"1 台，KRaft 单点）
#
# 拓扑：
#   az1 broker  :9492 (PLAINTEXT) :9493 (controller)
#   az2 broker  :9592 (PLAINTEXT) :9593 (controller)
#   MM2 az1 → az2 单向复制
set -euo pipefail
cd "$(dirname "$0")"

KAFKA_HOME=${KAFKA_HOME:-/home/ly/kafka/kafka_2.13-3.7.2}
DATA_ROOT=/tmp/mm2-tarball
mkdir -p "$DATA_ROOT"

if [[ ! -x "$KAFKA_HOME/bin/kafka-server-start.sh" ]]; then
    echo "FATAL: KAFKA_HOME=$KAFKA_HOME 没装" >&2
    exit 1
fi

CONF_DIR=$(pwd)/configs

# Format storage
for which in az1 az2; do
    DATA="$DATA_ROOT/${which}-broker"
    if [[ -f "$DATA/meta.properties" ]]; then
        echo "[$which] already formatted"
    else
        mkdir -p "$DATA"
        # 用不同 cluster.id 避免相互窜
        CID="mm2-${which}-cluster"
        echo "[$which] format with cluster.id=$CID"
        "$KAFKA_HOME/bin/kafka-storage.sh" format \
            --cluster-id "$CID" \
            --config "$CONF_DIR/${which}-broker.properties" \
            --ignore-formatted
    fi
done

start_broker() {
    local az=$1
    local pidfile="$DATA_ROOT/${az}-broker.pid"
    local logfile="$DATA_ROOT/${az}-broker.log"
    if [[ -f "$pidfile" ]] && kill -0 "$(cat "$pidfile")" 2>/dev/null; then
        echo "[$az] broker already running pid=$(cat "$pidfile")"
        return
    fi
    echo "[$az] starting broker"
    nohup "$KAFKA_HOME/bin/kafka-server-start.sh" "$CONF_DIR/${az}-broker.properties" \
        > "$logfile" 2>&1 &
    echo $! > "$pidfile"
    sleep 2
}

start_broker az1
start_broker az2

echo
echo "wait for both brokers ready..."
for port in 9492 9592; do
    for tries in $(seq 1 30); do
        if "$KAFKA_HOME/bin/kafka-broker-api-versions.sh" --bootstrap-server "localhost:$port" >/dev/null 2>&1; then
            echo "  ✓ broker ready on :$port"
            break
        fi
        sleep 1
    done
done

# 在 az1 创建测试 topic
echo
echo "creating test topic on az1..."
"$KAFKA_HOME/bin/kafka-topics.sh" --bootstrap-server localhost:9492 \
    --create --if-not-exists \
    --topic mm2-test.events --partitions 3 --replication-factor 1

# 启动 MirrorMaker 2
MM2_PID_FILE="$DATA_ROOT/mm2.pid"
if [[ -f "$MM2_PID_FILE" ]] && kill -0 "$(cat "$MM2_PID_FILE")" 2>/dev/null; then
    echo "MirrorMaker 2 already running pid=$(cat "$MM2_PID_FILE")"
else
    echo
    echo "starting MirrorMaker 2 (az1 → az2)"
    nohup "$KAFKA_HOME/bin/connect-mirror-maker.sh" "$CONF_DIR/mm2.properties" \
        > "$DATA_ROOT/mm2.log" 2>&1 &
    echo $! > "$MM2_PID_FILE"
    sleep 8  # MM2 启动需要点时间
fi

echo
echo "✓ Two-AZ Kafka with MM2 up"
echo "  az1 broker : localhost:9492"
echo "  az2 broker : localhost:9592"
echo "  MM2 log    : $DATA_ROOT/mm2.log"
echo "  drill      : bash deploy/multi-az/mm2-tarball/drill.sh"
echo "  shutdown   : bash deploy/multi-az/mm2-tarball/down.sh"
