#!/usr/bin/env bash
# 初始化 Phase 1 设计中定义的 4 类 topic
# 用法: bash deploy/kafka/init-topics.sh

set -e

BROKER=${KAFKA_BROKER:-localhost:9092}
EXEC="docker exec kafka-dev"

create_topic() {
    local name=$1
    local partitions=$2
    local retention_ms=$3
    echo "create $name partitions=$partitions retention=${retention_ms}ms"
    $EXEC kafka-topics.sh --bootstrap-server $BROKER \
        --create --if-not-exists \
        --topic "$name" \
        --partitions "$partitions" \
        --replication-factor 1 \
        --config retention.ms="$retention_ms" \
        --config compression.type=snappy
}

# 业务消息总线 — 7 天保留
create_topic im.messages       24 604800000

# 元事件 — 1 天保留
create_topic im.events         12 86400000

# 推送命令 — 1 小时（短时延，过期意义不大）
create_topic im.push.commands   8 3600000

# 死信队列 — 30 天
create_topic im.deadletter     4 2592000000

echo
echo "current topics:"
$EXEC kafka-topics.sh --bootstrap-server $BROKER --list
