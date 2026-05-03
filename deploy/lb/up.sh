#!/usr/bin/env bash
# Phase 4.3 启动 haproxy（前置 gateway-A:9091 + gateway-B:9192）
#
# 输入：
#   /etc/haproxy/haproxy.cfg 不动；用本地 deploy/lb/haproxy.cfg
# 输出：
#   listen *:9090 (ws frontend)
#   listen *:7777 (stats UI, http://localhost:7777)
set -uo pipefail

CFG="$(cd "$(dirname "$0")" && pwd)/haproxy.cfg"
PID=/tmp/im-haproxy.pid

# 如果系统服务在跑，停掉（避免和我们 9090/7777 冲突）
if systemctl is-active --quiet haproxy 2>/dev/null; then
    echo "stopping system haproxy.service"
    sudo systemctl stop haproxy
fi

# 老进程在的话先停
if [[ -f $PID ]] && kill -0 "$(cat $PID)" 2>/dev/null; then
    echo "killing old haproxy pid=$(cat $PID)"
    kill "$(cat $PID)"
    sleep 0.5
fi

echo "starting haproxy with $CFG"
haproxy -f "$CFG" -D -p "$PID"

sleep 0.5
if [[ -f $PID ]] && kill -0 "$(cat $PID)" 2>/dev/null; then
    echo "✓ haproxy up (pid=$(cat $PID))"
    echo "   ws front : ws://localhost:9090/ws"
    echo "   stats UI : http://localhost:7777"
    echo "   stop     : kill \$(cat $PID)"
else
    echo "haproxy failed to start"
    exit 1
fi
