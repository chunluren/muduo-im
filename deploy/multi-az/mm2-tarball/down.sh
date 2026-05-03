#!/usr/bin/env bash
set -uo pipefail
DATA_ROOT=/tmp/mm2-tarball

for name in mm2 az2-broker az1-broker; do
    pidfile="$DATA_ROOT/$name.pid"
    [[ -f "$pidfile" ]] || continue
    pid=$(cat "$pidfile")
    if kill -0 "$pid" 2>/dev/null; then
        echo "stop $name pid=$pid"
        kill -TERM "$pid"
    fi
    rm -f "$pidfile"
done
sleep 1
echo "all mm2 / brokers down"
