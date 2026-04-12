#!/bin/bash
# muduo-im 状态查看

cd "$(dirname "$0")"

echo "=== muduo-im 状态 ==="

# 进程
if [ -f muduo-im.pid ] && kill -0 "$(cat muduo-im.pid)" 2>/dev/null; then
    echo "状态: 运行中 (PID: $(cat muduo-im.pid))"
else
    echo "状态: 未运行"
fi

# 端口
echo ""
echo "端口监听:"
ss -tlnp 2>/dev/null | grep -E "8080|9090" || echo "  无"

# MySQL
echo ""
echo -n "MySQL: "
mysqladmin ping -u root --silent 2>/dev/null && echo "正常" || echo "未运行"

# Redis
echo -n "Redis: "
redis-cli ping 2>/dev/null || echo "未运行"

# 日志最后几行
if [ -f logs/server.log ]; then
    echo ""
    echo "最近日志:"
    tail -5 logs/server.log
fi
