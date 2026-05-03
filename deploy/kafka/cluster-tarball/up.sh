#!/usr/bin/env bash
# Phase 4.2 启动 3 个 Kafka broker（tarball 模式，KRaft 单 cluster）
#
# 端口分配：
#   broker-1: PLAINTEXT 9092 / CONTROLLER 9093
#   broker-2: PLAINTEXT 9192 / CONTROLLER 9193
#   broker-3: PLAINTEXT 9292 / CONTROLLER 9293
#
# 数据目录：/tmp/kafka-cluster-tarball/broker-{1,2,3}
# 日志：    /tmp/kafka-cluster-tarball/broker-{1,2,3}.log
# PID：     /tmp/kafka-cluster-tarball/broker-{1,2,3}.pid
set -euo pipefail

KAFKA_HOME=${KAFKA_HOME:-/home/ly/kafka/kafka_2.13-3.7.2}
DATA_ROOT=/tmp/kafka-cluster-tarball
CLUSTER_ID=${KAFKA_CLUSTER_ID:-im-kafka-cluster-001}

cd "$(dirname "$0")"
CONF_DIR="$(pwd)/configs"

if [[ ! -x "$KAFKA_HOME/bin/kafka-server-start.sh" ]]; then
    echo "FATAL: KAFKA_HOME=$KAFKA_HOME 不存在 kafka-server-start.sh" >&2
    exit 1
fi

mkdir -p "$DATA_ROOT"

# Format storage 一次性（首次起集群）。每个 broker 独立 log.dirs，但用同一个 cluster-id
for i in 1 2 3; do
    DATA="$DATA_ROOT/broker-$i"
    if [[ -f "$DATA/meta.properties" ]]; then
        echo "broker-$i already formatted (cluster.id=$(grep cluster.id $DATA/meta.properties | cut -d= -f2))"
    else
        mkdir -p "$DATA"
        echo "format broker-$i with cluster.id=$CLUSTER_ID"
        "$KAFKA_HOME/bin/kafka-storage.sh" format \
            --cluster-id "$CLUSTER_ID" \
            --config "$CONF_DIR/server-$i.properties" \
            --ignore-formatted
    fi
done

# 启动
for i in 1 2 3; do
    PID_FILE="$DATA_ROOT/broker-$i.pid"
    LOG_FILE="$DATA_ROOT/broker-$i.log"
    if [[ -f "$PID_FILE" ]] && kill -0 "$(cat "$PID_FILE")" 2>/dev/null; then
        echo "broker-$i already running (pid=$(cat "$PID_FILE"))"
        continue
    fi
    echo "start broker-$i (log → $LOG_FILE)"
    nohup "$KAFKA_HOME/bin/kafka-server-start.sh" "$CONF_DIR/server-$i.properties" \
        > "$LOG_FILE" 2>&1 &
    echo $! > "$PID_FILE"
    sleep 2
done

echo
echo "wait for all 3 to come up..."
for i in 1 2 3; do
    PORT=$(( 9092 + (i-1) * 100 ))
    for tries in $(seq 1 30); do
        if "$KAFKA_HOME/bin/kafka-broker-api-versions.sh" \
                --bootstrap-server "localhost:$PORT" >/dev/null 2>&1; then
            echo "  broker-$i ready on :$PORT"
            break
        fi
        sleep 1
    done
done

echo
echo "cluster status:"
"$KAFKA_HOME/bin/kafka-metadata-quorum.sh" --bootstrap-server localhost:9092 describe --status \
    || echo "(metadata-quorum 命令不可用，跳过)"

echo
echo "topics:"
"$KAFKA_HOME/bin/kafka-topics.sh" --bootstrap-server localhost:9092 --list || true

echo
echo "✓ cluster up. Bootstrap (host)='localhost:9092,localhost:9192,localhost:9292'"
echo "  init topics: bash deploy/kafka/cluster-tarball/init-topics.sh"
echo "  drill:       bash deploy/kafka/cluster-tarball/rolling_restart_drill.sh"
echo "  shutdown:    bash deploy/kafka/cluster-tarball/down.sh"
