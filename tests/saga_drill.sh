#!/usr/bin/env bash
# Phase 6.1c+d: SagaCoordinator 崩溃恢复 + 故障注入 e2e drill
#
# 三个场景：
#   A. happy path        — 正常创建群，验证 saga_log state=done
#   B. crash 恢复        — 在 saga step1 commit 后、step2 之前 kill -9，
#                          重启 ChatServer，验证 recoverIncomplete 续跑 step2
#   C. step2 注入失败    — 用一个故意触发 step2 INSERT 失败的 owner_id（DB 外键
#                          没建，所以这个测试改成"step1 用一个名字 + duplicate
#                          INSERT 触发 unique key conflict 模拟失败"）→ 验证
#                          step1 反向 DELETE 把 groups 表清干净
#
# 跑：bash tests/saga_drill.sh
set -uo pipefail
cd "$(dirname "$0")/.."

BUILD=${BUILD:-build}
LOG=${LOG:-/tmp/muduo-im-saga-drill}
mkdir -p "$LOG"

cleanup() {
    pkill -9 -f "$BUILD/muduo-im" 2>/dev/null || true
    rm -f /tmp/cs-saga-drill.ini
}
trap cleanup EXIT INT TERM
cleanup

# 用专属 ws 端口避免和 prom 冲突
sed 's|^server.ws_port = .*|server.ws_port = 9095|' config.ini > /tmp/cs-saga-drill.ini

start_server() {
    "$BUILD/muduo-im" /tmp/cs-saga-drill.ini > "$LOG/server.log" 2>&1 &
    echo $! > "$LOG/server.pid"
    for i in $(seq 1 30); do
        sleep 0.3
        if curl -sf -m 1 http://127.0.0.1:8080/health >/dev/null 2>&1 \
           || curl -sf -m 1 http://127.0.0.1:8080/ >/dev/null 2>&1 \
           || ss -ltn 2>/dev/null | grep -q ":8080 "; then return 0; fi
    done
    return 1
}

stop_server() {
    pid=$(cat "$LOG/server.pid" 2>/dev/null || echo "")
    [[ -n "$pid" ]] && kill -TERM "$pid" 2>/dev/null
    sleep 0.5
    [[ -n "$pid" ]] && kill -9 "$pid" 2>/dev/null || true
    rm -f "$LOG/server.pid"
}

kill_server_hard() {
    pid=$(cat "$LOG/server.pid" 2>/dev/null || echo "")
    [[ -n "$pid" ]] && kill -9 "$pid" 2>/dev/null
    rm -f "$LOG/server.pid"
    sleep 0.5
}

mysql_cleanup() {
    mysql -uroot muduo_im -e "
        DELETE FROM saga_log WHERE saga_type = 'group_create' AND payload LIKE '%saga-drill-%';
        DELETE FROM group_members WHERE group_id IN (SELECT id FROM \`groups\` WHERE name LIKE 'saga-drill-%');
        DELETE FROM \`groups\` WHERE name LIKE 'saga-drill-%';
        DELETE FROM users WHERE username LIKE 'sagadrill_%';
    " 2>/dev/null
}

# ─────── Case A: happy path ───────
mysql_cleanup
echo "[drill] === Case A: happy path ==="
start_server || { echo "FATAL: server didn't start"; exit 1; }
TS=$(date +%s)
USER="sagadrill_a_$TS"
curl -sf -X POST http://127.0.0.1:8080/api/register \
  -H "Content-Type: application/json" \
  -d "{\"username\":\"$USER\",\"password\":\"pw123456\"}" >/dev/null
TOKEN=$(curl -sf -X POST http://127.0.0.1:8080/api/login \
  -H "Content-Type: application/json" \
  -d "{\"username\":\"$USER\",\"password\":\"pw123456\"}" \
  | python3 -c "import sys,json;print(json.load(sys.stdin)['token'])")

RES=$(curl -sf -X POST http://127.0.0.1:8080/api/groups/create \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $TOKEN" \
  -d '{"name":"saga-drill-A"}')
echo "  response: $RES"
GID=$(echo "$RES" | python3 -c "import sys,json;print(json.load(sys.stdin).get('groupId',0))")
SID=$(echo "$RES" | python3 -c "import sys,json;print(json.load(sys.stdin).get('sagaId',0))")
STATE=$(mysql -uroot muduo_im -sN -e "SELECT state FROM saga_log WHERE saga_id = $SID")
GEXISTS=$(mysql -uroot muduo_im -sN -e "SELECT COUNT(*) FROM \`groups\` WHERE id = $GID")
MEXISTS=$(mysql -uroot muduo_im -sN -e "SELECT COUNT(*) FROM group_members WHERE group_id = $GID")
[[ "$STATE" = "done" && "$GEXISTS" = 1 && "$MEXISTS" = 1 ]] \
    && echo "  ✓ Case A PASS  (state=done, group=1, member=1)" \
    || { echo "  ✗ Case A FAIL  state=$STATE g=$GEXISTS m=$MEXISTS"; exit 1; }

stop_server

# ─────── Case B: crash recovery ───────
echo
echo "[drill] === Case B: crash between step1 and step2 → recover continues ==="
mysql_cleanup
# 先注册一个真用户（FK 要求 owner_id 必须是有效 user）
TS_B=$(date +%s)$RANDOM
USER_B="sagadrill_b_$TS_B"
start_server || { echo "FATAL: server didn't start"; exit 1; }
curl -sf -X POST http://127.0.0.1:8080/api/register \
  -H "Content-Type: application/json" \
  -d "{\"username\":\"$USER_B\",\"password\":\"pw123456\"}" >/dev/null
USER_ID_B=$(mysql -uroot muduo_im -sN -e "SELECT id FROM users WHERE username='$USER_B'")
stop_server

# 手工塞一个 saga_log 行：state=running, current_step=1, group_id 已回填，
# 但 group_members 还没 INSERT。模拟"step1 commit 后崩溃"。
SID_B=$((1777800000000000000 + RANDOM * 1000 + RANDOM))
mysql -uroot muduo_im -e "INSERT INTO \`groups\` (name, owner_id) VALUES ('saga-drill-B', $USER_ID_B)"
GID_B=$(mysql -uroot muduo_im -sN -e "SELECT id FROM \`groups\` WHERE name='saga-drill-B' ORDER BY id DESC LIMIT 1")
mysql -uroot muduo_im -e "INSERT INTO saga_log (saga_id, saga_type, state, current_step, payload) VALUES ($SID_B, 'group_create', 'running', 1, '{\"owner_id\":$USER_ID_B,\"name\":\"saga-drill-B\",\"group_id\":$GID_B}')"
echo "  before recover: group_id=$GID_B saga_id=$SID_B state=running step=1"
echo "  (group exists but group_members 还没写)"
M_BEFORE=$(mysql -uroot muduo_im -sN -e "SELECT COUNT(*) FROM group_members WHERE group_id=$GID_B")
echo "  group_members rows before: $M_BEFORE (expect 0)"

# 启动 server → enableSaga 启动期会调 recoverIncomplete()
start_server || { echo "FATAL"; exit 1; }
sleep 1
echo "  saga init log:"
grep -iE "saga|recovered" "$LOG/server.log" | head -5

STATE_B=$(mysql -uroot muduo_im -sN -e "SELECT state FROM saga_log WHERE saga_id = $SID_B")
M_AFTER=$(mysql -uroot muduo_im -sN -e "SELECT COUNT(*) FROM group_members WHERE group_id=$GID_B")
echo "  after recover: state=$STATE_B group_members rows=$M_AFTER (expect 1)"
[[ "$STATE_B" = "done" && "$M_AFTER" = 1 ]] \
    && echo "  ✓ Case B PASS (recover 续跑 step2 成功)" \
    || { echo "  ✗ Case B FAIL"; exit 1; }
stop_server

# ─────── Case C: step2 注入失败 → step1 compensate ───────
echo
echo "[drill] === Case C: step2 fail → step1 compensate (DELETE groups) ==="
mysql_cleanup
# 故障注入：往 group_members 加一个 NOT NULL 无默认值的列，saga 的 step2
# INSERT 不提供这个列就会 1364 报错。结束后 DROP 这一列还原。
mysql -uroot muduo_im -e 'ALTER TABLE group_members ADD COLUMN _drill_block VARCHAR(8) NOT NULL'

start_server || { echo "FATAL"; exit 1; }
TS=$(date +%s)
USER_C="sagadrill_c_$TS"
curl -sf -X POST http://127.0.0.1:8080/api/register \
  -H "Content-Type: application/json" \
  -d "{\"username\":\"$USER_C\",\"password\":\"pw123456\"}" >/dev/null
TOKEN_C=$(curl -sf -X POST http://127.0.0.1:8080/api/login \
  -H "Content-Type: application/json" \
  -d "{\"username\":\"$USER_C\",\"password\":\"pw123456\"}" \
  | python3 -c "import sys,json;print(json.load(sys.stdin)['token'])")

RES_C=$(curl -s -X POST http://127.0.0.1:8080/api/groups/create \
  -H "Content-Type: application/json" \
  -H "Authorization: Bearer $TOKEN_C" \
  -d '{"name":"saga-drill-C"}' || echo '{"success":false}')
echo "  response: $RES_C"
SID_C=$(echo "$RES_C" | python3 -c "import sys,json;print(json.load(sys.stdin).get('sagaId',0))" 2>/dev/null || echo 0)

STATE_C=$(mysql -uroot muduo_im -sN -e "SELECT state FROM saga_log WHERE saga_id = $SID_C" 2>/dev/null)
G_AFTER_C=$(mysql -uroot muduo_im -sN -e "SELECT COUNT(*) FROM \`groups\` WHERE name='saga-drill-C'")
echo "  saga state=$STATE_C, groups rows=$G_AFTER_C (expect failed + 0)"

# 还原表
mysql -uroot muduo_im -e 'ALTER TABLE group_members DROP COLUMN _drill_block' 2>/dev/null

[[ "$STATE_C" = "failed" && "$G_AFTER_C" = 0 ]] \
    && echo "  ✓ Case C PASS (saga compensated, groups 表干净)" \
    || { echo "  ✗ Case C FAIL  state=$STATE_C g_count=$G_AFTER_C"; exit 1; }

stop_server
mysql_cleanup
echo
echo "[drill] ALL CASES PASS"
