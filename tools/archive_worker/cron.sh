#!/bin/bash
# 每天凌晨 2 点跑一次归档（添加到 crontab）
# crontab -e
#   0 2 * * * /home/ly/workspaces/im/muduo-im/tools/archive_worker/cron.sh
set -e
cd "$(dirname "$0")"
LOG=/home/ly/workspaces/im/muduo-im/logs/archive_$(date +%Y%m%d).log
mkdir -p "$(dirname "$LOG")"
/home/ly/miniconda3/bin/python3 archive.py >> "$LOG" 2>&1
