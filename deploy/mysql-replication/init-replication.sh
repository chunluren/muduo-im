#!/usr/bin/env bash
# 在已经启动的 master + 2 replica 上配 GTID-based 复制
#
# 前提：docker compose up 后 master 已 ready
set -euo pipefail

MASTER_PORT=${MASTER_PORT:-3306}
REPLICA_PORTS=${REPLICA_PORTS:-3307 3308}

run_master() { mysql -h 127.0.0.1 -P "$MASTER_PORT" -uroot "$@"; }
run_replica() { mysql -h 127.0.0.1 -P "$1" -uroot "$@"; }

echo "[init] create replication user on master :${MASTER_PORT}"
run_master <<'SQL'
CREATE USER IF NOT EXISTS 'repl'@'%' IDENTIFIED WITH mysql_native_password BY 'repl';
GRANT REPLICATION SLAVE ON *.* TO 'repl'@'%';
FLUSH PRIVILEGES;
SQL

for port in $REPLICA_PORTS; do
    echo "[init] CHANGE REPLICATION SOURCE on replica :$port → master :$MASTER_PORT (auto-position)"
    mysql -h 127.0.0.1 -P "$port" -uroot <<SQL
STOP REPLICA;
RESET REPLICA ALL;
CHANGE REPLICATION SOURCE TO
  SOURCE_HOST='mysql-master',
  SOURCE_PORT=3306,
  SOURCE_USER='repl',
  SOURCE_PASSWORD='repl',
  SOURCE_AUTO_POSITION=1,
  SOURCE_SSL=0,
  GET_SOURCE_PUBLIC_KEY=1;
START REPLICA;
SQL
done

sleep 2
echo
echo "[init] replica status:"
for port in $REPLICA_PORTS; do
    echo "  --- replica :$port ---"
    mysql -h 127.0.0.1 -P "$port" -uroot -e "SHOW REPLICA STATUS\G" \
        | grep -E "Replica_IO_Running|Replica_SQL_Running|Last_Error|Seconds_Behind_Source|Source_Host" \
        | head -6
done
