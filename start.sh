#!/bin/bash
# muduo-im 启动脚本

cd "$(dirname "$0")"

# 检查 MySQL
if ! mysqladmin ping -u root --silent 2>/dev/null; then
    echo "启动 MySQL..."
    sudo service mysql start
    sleep 2
fi

# 检查 Redis
if ! redis-cli ping 2>/dev/null | grep -q PONG; then
    echo "启动 Redis..."
    sudo service redis-server start
    sleep 1
fi

# 建库 + 建表（幂等）
# 注意：init.sql 已改为不带 CREATE DATABASE / USE，需在命令行指定目标库
sudo mysql -e "CREATE DATABASE IF NOT EXISTS muduo_im DEFAULT CHARACTER SET utf8mb4" 2>/dev/null
sudo mysql muduo_im < sql/init.sql 2>/dev/null

# 编译（如果没有二进制或源码更新了）
if [ ! -f build/muduo-im ] || [ "$(find src/ -newer build/muduo-im -name '*.h' -o -name '*.cpp' 2>/dev/null)" ]; then
    echo "编译中..."
    mkdir -p build && cd build && cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1 && make muduo-im -j$(nproc) 2>&1 | tail -3
    cd ..
fi

# 检查端口占用
if ss -tlnp | grep -q ":8080 "; then
    echo "端口 8080 已被占用，先停止旧进程..."
    pkill -f "muduo-im" 2>/dev/null
    sleep 1
fi

# 启动
echo "启动 muduo-im..."
cd build && ./muduo-im > ../logs/server.log 2>&1 &
echo $! > ../muduo-im.pid
cd ..
mkdir -p logs

echo "================================"
echo "  muduo-im 已启动"
echo "  PID: $(cat muduo-im.pid)"
echo "  HTTP:      http://localhost:8080"
echo "  前端:      http://localhost:8080/index.html"
echo "  WebSocket: ws://localhost:9090"
echo "  日志:      logs/server.log"
echo "================================"
