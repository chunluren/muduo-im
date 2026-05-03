#!/usr/bin/env bash
# Phase 4.4 把一个 replica 提升为新 master
#
# 用法：
#   bash deploy/mysql-replication/promote.sh \
#     --new-master-port 3307 \
#     --remaining-replica-ports "3308" \
#     [--new-master-host 127.0.0.1]
#     [--new-master-internal-host mysql-replica-1]   # 容器内别名
#
# 步骤（GTID 自动定位 + ROW binlog）：
#   1. 在新 master：STOP REPLICA; RESET REPLICA ALL; SET @@global.read_only=OFF
#   2. 对剩下的 replica：CHANGE REPLICATION SOURCE TO 新 master + START REPLICA
#   3. 打印新拓扑
#
# 不做：
#   - 配置文件改写（应用 config.ini 的 mysql.host/port）；调用方自己更新
#   - 应用流量切换；DBA/SRE 自己负责
set -euo pipefail

NEW_MASTER_PORT=""
REMAINING_REPLICAS=""
NEW_MASTER_HOST="127.0.0.1"
NEW_MASTER_INTERNAL_HOST="mysql-replica-1"

while [[ $# -gt 0 ]]; do
    case $1 in
        --new-master-port) NEW_MASTER_PORT=$2; shift 2 ;;
        --remaining-replica-ports) REMAINING_REPLICAS=$2; shift 2 ;;
        --new-master-host) NEW_MASTER_HOST=$2; shift 2 ;;
        --new-master-internal-host) NEW_MASTER_INTERNAL_HOST=$2; shift 2 ;;
        *) echo "unknown arg: $1" >&2; exit 1 ;;
    esac
done

if [[ -z "$NEW_MASTER_PORT" ]]; then
    echo "FATAL: --new-master-port required" >&2
    exit 1
fi

# 1. 提升新 master
echo "[promote] >>> promote $NEW_MASTER_HOST:$NEW_MASTER_PORT to master"
mysql -h "$NEW_MASTER_HOST" -P "$NEW_MASTER_PORT" -uroot <<'SQL'
STOP REPLICA;
RESET REPLICA ALL;
SET @@global.read_only = OFF;
SET @@global.super_read_only = OFF;
SQL

# 验证可写
mysql -h "$NEW_MASTER_HOST" -P "$NEW_MASTER_PORT" -uroot -e \
    "CREATE DATABASE IF NOT EXISTS _promote_test; DROP DATABASE _promote_test;"
echo "[promote] new master accepts writes ✓"

# 2. 把剩下的 replica 指过来
if [[ -n "$REMAINING_REPLICAS" ]]; then
    for port in $REMAINING_REPLICAS; do
        echo "[promote] re-point replica :$port to new master $NEW_MASTER_INTERNAL_HOST:3306"
        mysql -h 127.0.0.1 -P "$port" -uroot <<SQL
STOP REPLICA;
RESET REPLICA ALL;
CHANGE REPLICATION SOURCE TO
  SOURCE_HOST='$NEW_MASTER_INTERNAL_HOST',
  SOURCE_PORT=3306,
  SOURCE_USER='repl',
  SOURCE_PASSWORD='repl',
  SOURCE_AUTO_POSITION=1,
  SOURCE_SSL=0,
  GET_SOURCE_PUBLIC_KEY=1;
START REPLICA;
SQL
    done
fi

sleep 2
echo
echo "[promote] new topology:"
echo "  master    : $NEW_MASTER_HOST:$NEW_MASTER_PORT"
echo "  replicas  : $REMAINING_REPLICAS"
for port in $REMAINING_REPLICAS; do
    echo "  --- replica :$port ---"
    mysql -h 127.0.0.1 -P "$port" -uroot -e "SHOW REPLICA STATUS\G" \
        | grep -E "Replica_IO_Running|Replica_SQL_Running|Source_Host|Seconds_Behind_Source" \
        | head -4
done

echo
echo "[promote] DONE. 别忘了改 config.ini 的 mysql.host/port + 重启应用"
