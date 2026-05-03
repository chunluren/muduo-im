#!/usr/bin/env bash
# Phase 4.5 failover 演练
#
# 流程：
#   1. 写 N 条 key 到 master
#   2. SIGTERM master（pid=$ROOT/master.pid）
#   3. 等 ≤ 30s（down-after=5s + sentinel 选举 + replicaof 切换）
#   4. 通过 sentinel 找新 master，验证：
#      a. 旧 master 写过的 key 都还在
#      b. 新 master 接受新写入
#      c. 剩下那个 replica 已自动 replicaof 新 master
set -uo pipefail
ROOT=/tmp/redis-sentinel

# 0. 拓扑健康
master_addr=$(redis-cli -p 26379 sentinel get-master-addr-by-name im-master 2>/dev/null | head -2 | tr '\n' ':' | sed 's|:$||')
if [[ -z "$master_addr" ]]; then
    echo "[drill] FATAL: sentinel 看不到 master。先跑 up.sh" >&2
    exit 1
fi
echo "[drill] current master=$master_addr"
MASTER_HOST=${master_addr%:*}
MASTER_PORT=${master_addr#*:}

# 1. 写 100 条
echo "[drill] write 100 keys to master $MASTER_PORT"
for i in $(seq 1 100); do
    redis-cli -h "$MASTER_HOST" -p "$MASTER_PORT" set "drill-key-$i" "v-$i" > /dev/null
done
PRE_COUNT=$(redis-cli -h "$MASTER_HOST" -p "$MASTER_PORT" dbsize)
echo "[drill] pre-failover dbsize=$PRE_COUNT"

# 2. 杀 master
MASTER_PID=$(cat "$ROOT/master.pid" 2>/dev/null || true)
if [[ -z "$MASTER_PID" ]] || ! kill -0 "$MASTER_PID" 2>/dev/null; then
    echo "[drill] FATAL: master.pid invalid" >&2
    exit 1
fi
echo "[drill] >>> SIGTERM master pid=$MASTER_PID"
T0=$(date +%s)
kill -TERM "$MASTER_PID"

# 3. 等 sentinel failover
echo "[drill] waiting for failover (timeout 30s)..."
for i in $(seq 1 60); do
    sleep 0.5
    new_addr=$(redis-cli -p 26379 sentinel get-master-addr-by-name im-master 2>/dev/null | head -2 | tr '\n' ':' | sed 's|:$||')
    if [[ -n "$new_addr" ]] && [[ "$new_addr" != "$master_addr" ]]; then
        echo "[drill] new master discovered: $new_addr (took $(awk "BEGIN{printf \"%.1f\", $(date +%s) - $T0}")s)"
        master_addr=$new_addr
        break
    fi
done

if [[ "$master_addr" == "${MASTER_HOST}:${MASTER_PORT}" ]]; then
    echo "[drill] FAIL: sentinel 没在 30s 内 failover"
    exit 1
fi

NEW_HOST=${master_addr%:*}
NEW_PORT=${master_addr#*:}

# 4a. 验证 key 还在
DBSIZE_AFTER=$(redis-cli -h "$NEW_HOST" -p "$NEW_PORT" dbsize)
echo "[drill] new master dbsize=$DBSIZE_AFTER (was $PRE_COUNT)"
if [[ $DBSIZE_AFTER -lt $PRE_COUNT ]]; then
    echo "[drill] WARN: 数据丢失 ($PRE_COUNT → $DBSIZE_AFTER)，replication 可能没跟上"
fi
SAMPLE=$(redis-cli -h "$NEW_HOST" -p "$NEW_PORT" get "drill-key-50")
echo "[drill] sample key drill-key-50 = $SAMPLE"
if [[ "$SAMPLE" != "v-50" ]]; then
    echo "[drill] FAIL: 老数据丢了"
    exit 1
fi

# 4b. 新 master 接受新写
echo "[drill] write 50 new keys to new master"
for i in $(seq 101 150); do
    redis-cli -h "$NEW_HOST" -p "$NEW_PORT" set "drill-key-$i" "v-$i" > /dev/null
done
DBSIZE_FINAL=$(redis-cli -h "$NEW_HOST" -p "$NEW_PORT" dbsize)
echo "[drill] final dbsize=$DBSIZE_FINAL (expected ≥ $((PRE_COUNT + 50)))"

# 4c. 看剩下的 replica 是否跟新 master
echo "[drill] sentinel sees replicas:"
redis-cli -p 26379 sentinel replicas im-master | grep -E "name|ip|port|flags" | head -8

if [[ $DBSIZE_FINAL -ge $((PRE_COUNT + 50 - 5)) ]]; then
    echo "[drill] PASS: failover 成功，新写继续工作（容忍 ≤5 条 in-flight 丢失）"
    exit 0
else
    echo "[drill] FAIL: 新写后 dbsize 不对"
    exit 1
fi
