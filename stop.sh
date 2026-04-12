#!/bin/bash
# muduo-im 停止脚本

cd "$(dirname "$0")"

if [ -f muduo-im.pid ]; then
    PID=$(cat muduo-im.pid)
    if kill -0 "$PID" 2>/dev/null; then
        echo "停止 muduo-im (PID: $PID)..."
        kill "$PID"
        sleep 2
        # 如果还没停，强制杀
        if kill -0 "$PID" 2>/dev/null; then
            kill -9 "$PID"
        fi
        echo "已停止"
    else
        echo "进程 $PID 已不存在"
    fi
    rm -f muduo-im.pid
else
    # 没有 pid 文件，尝试按进程名杀
    if pkill -f "muduo-im"; then
        echo "已停止 muduo-im"
    else
        echo "muduo-im 未运行"
    fi
fi
