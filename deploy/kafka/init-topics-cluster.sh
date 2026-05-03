#!/usr/bin/env bash
# Phase 4.2 集群版初始化：RF=3, min.isr=2
#
# 与 init-topics.sh 区别：
#   - replication-factor: 1 → 3
#   - min.insync.replicas: 1 → 2  （任 1 broker 挂 producer 仍能写）
set -e

BROKER=${KAFKA_BROKER:-localhost:9092}
EXEC=${KAFKA_EXEC:-docker exec kafka-1}

create_topic() {
    local name=$1
    local partitions=$2
    local retention_ms=$3
    echo "create $name partitions=$partitions retention=${retention_ms}ms RF=3 min.isr=2"
    $EXEC kafka-topics.sh --bootstrap-server $BROKER \
        --create --if-not-exists \
        --topic "$name" \
        --partitions "$partitions" \
        --replication-factor 3 \
        --config retention.ms="$retention_ms" \
        --config compression.type=snappy \
        --config min.insync.replicas=2
}

create_topic im.messages       24 604800000   # 7d
create_topic im.events         12 86400000    # 1d
create_topic im.push.commands   8 3600000     # 1h
create_topic im.deadletter      4 2592000000  # 30d

echo
echo "topic descriptions:"
$EXEC kafka-topics.sh --bootstrap-server $BROKER --describe \
    | grep -E "^Topic|Partition: 0" | head -16
