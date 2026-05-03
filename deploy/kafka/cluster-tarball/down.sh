#!/usr/bin/env bash
# 停所有 broker（用 SIGTERM，不擦数据）。擦数据：rm -rf /tmp/kafka-cluster-tarball
set -uo pipefail
DATA_ROOT=/tmp/kafka-cluster-tarball

for i in 1 2 3; do
    PID_FILE="$DATA_ROOT/broker-$i.pid"
    [[ -f "$PID_FILE" ]] || continue
    PID=$(cat "$PID_FILE")
    if kill -0 "$PID" 2>/dev/null; then
        echo "stop broker-$i (pid=$PID)"
        kill -TERM "$PID"
    fi
    rm -f "$PID_FILE"
done

# 等真正 down
for tries in $(seq 1 60); do
    alive=0
    for i in 1 2 3; do
        nc -z localhost $((9092 + (i-1)*100)) 2>/dev/null && alive=$((alive+1))
    done
    [[ $alive -eq 0 ]] && break
    sleep 1
done
echo "all brokers down"
