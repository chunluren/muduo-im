#!/usr/bin/env bash
# Phase 4.4 主从切换演练
#
# 流程：
#   1. 在 master 写 100 条
#   2. 验证两个 replica 已经 catch-up
#   3. 杀 master（docker stop）
#   4. 调 promote.sh 把 replica-1 (3307) 提为新 master
#   5. 在新 master 写 50 条
#   6. 把 replica-2 (3308) 拉到新 master 后，验证它能看到新写入
#
# 跑前提：docker compose -f docker-compose.yml up -d + init-replication.sh 跑过
set -uo pipefail
cd "$(dirname "$0")"

OLD_MASTER_PORT=3306
NEW_MASTER_PORT=3307
OTHER_REPLICA_PORT=3308

# 0. 健康
for p in $OLD_MASTER_PORT $NEW_MASTER_PORT $OTHER_REPLICA_PORT; do
    if ! mysql -h 127.0.0.1 -P "$p" -uroot -e "SELECT 1" >/dev/null 2>&1; then
        echo "[drill] FATAL: mysql :$p not reachable" >&2
        exit 1
    fi
done

# 1. 写 100 条到 master
echo "[drill] write 100 rows to master :$OLD_MASTER_PORT"
mysql -h 127.0.0.1 -P "$OLD_MASTER_PORT" -uroot muduo_im <<'SQL'
CREATE TABLE IF NOT EXISTS drill_kv (k INT PRIMARY KEY, v VARCHAR(64));
SQL
for i in $(seq 1 100); do
    mysql -h 127.0.0.1 -P "$OLD_MASTER_PORT" -uroot muduo_im \
        -e "INSERT INTO drill_kv VALUES ($i, 'pre-failover-$i') ON DUPLICATE KEY UPDATE v=VALUES(v);"
done

PRE=$(mysql -h 127.0.0.1 -P "$OLD_MASTER_PORT" -uroot muduo_im -sN -e "SELECT COUNT(*) FROM drill_kv")
echo "[drill] master count=$PRE"

# 2. 等 replica 跟上
sleep 2
for p in $NEW_MASTER_PORT $OTHER_REPLICA_PORT; do
    n=$(mysql -h 127.0.0.1 -P "$p" -uroot muduo_im -sN -e "SELECT COUNT(*) FROM drill_kv" 2>/dev/null || echo 0)
    echo "[drill] replica :$p count=$n"
    if [[ $n -lt $PRE ]]; then
        echo "[drill] WARN: replica :$p lagging ($n < $PRE)"
    fi
done

# 3. 杀 master
echo "[drill] >>> docker stop mysql-master"
T0=$(date +%s)
docker stop mysql-master

# 4. promote
bash promote.sh \
    --new-master-port $NEW_MASTER_PORT \
    --remaining-replica-ports "$OTHER_REPLICA_PORT" \
    --new-master-internal-host mysql-replica-1

PROMO_DT=$(($(date +%s) - T0))
echo "[drill] promote took ${PROMO_DT}s"

# 5. 新 master 写 50 条
echo "[drill] write 50 rows to new master :$NEW_MASTER_PORT"
for i in $(seq 101 150); do
    mysql -h 127.0.0.1 -P "$NEW_MASTER_PORT" -uroot muduo_im \
        -e "INSERT INTO drill_kv VALUES ($i, 'post-failover-$i');"
done

# 6. 验证 replica-2 收到新写
sleep 2
N2=$(mysql -h 127.0.0.1 -P "$OTHER_REPLICA_PORT" -uroot muduo_im -sN -e "SELECT COUNT(*) FROM drill_kv")
echo "[drill] replica :$OTHER_REPLICA_PORT count after promote = $N2"

# 验收
if [[ $N2 -ge $((PRE + 50 - 5)) ]]; then
    echo "[drill] PASS: failover < 30s, replica-2 sees new writes"
    exit 0
else
    echo "[drill] FAIL: replica-2 only sees $N2 rows (expected ≥ $((PRE + 50 - 5)))"
    exit 1
fi
