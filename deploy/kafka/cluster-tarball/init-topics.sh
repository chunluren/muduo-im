#!/usr/bin/env bash
# Phase 4.2 集群版 init-topics（tarball 模式）
# RF=3, min.isr=2
set -e

KAFKA_HOME=${KAFKA_HOME:-/home/ly/kafka/kafka_2.13-3.7.2}
BROKER=${KAFKA_BROKER:-localhost:9092}

create_topic() {
    local name=$1 partitions=$2 retention_ms=$3
    echo "create $name partitions=$partitions retention=${retention_ms}ms RF=3 min.isr=2"
    "$KAFKA_HOME/bin/kafka-topics.sh" --bootstrap-server "$BROKER" \
        --create --if-not-exists \
        --topic "$name" \
        --partitions "$partitions" \
        --replication-factor 3 \
        --config retention.ms="$retention_ms" \
        --config compression.type=snappy \
        --config min.insync.replicas=2
}

create_topic im.messages       24 604800000
create_topic im.events         12 86400000
create_topic im.push.commands   8 3600000
create_topic im.deadletter      4 2592000000

echo
echo "topics:"
"$KAFKA_HOME/bin/kafka-topics.sh" --bootstrap-server "$BROKER" --list

echo
echo "im.messages partitions 0-2:"
"$KAFKA_HOME/bin/kafka-topics.sh" --bootstrap-server "$BROKER" --describe --topic im.messages \
    | head -5
