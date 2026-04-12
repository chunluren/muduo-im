#!/bin/bash
# muduo-im 重启脚本

cd "$(dirname "$0")"
bash stop.sh
sleep 1
bash start.sh
