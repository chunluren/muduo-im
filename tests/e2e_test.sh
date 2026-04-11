#!/bin/bash
# muduo-im 端到端联调测试
# 前提: MySQL + Redis 运行中, muduo-im 在 localhost:8080/9090
set -e

API="http://localhost:8080"
PASS=0
FAIL=0

check() {
    local desc="$1" expected="$2" actual="$3"
    if echo "$actual" | grep -q "$expected"; then
        echo "  ✓ $desc"
        ((PASS++))
    else
        echo "  ✗ $desc (expected '$expected', got '$actual')"
        ((FAIL++))
    fi
}

echo "========================================"
echo "  muduo-im 端到端测试"
echo "========================================"

# 清理测试数据
echo ""
echo "--- 1. 注册用户 ---"
R1=$(curl -s -X POST $API/api/register -H "Content-Type: application/json" \
    -d '{"username":"testuser1","password":"pass123","nickname":"Test1"}')
check "注册 user1" "success" "$R1"

R2=$(curl -s -X POST $API/api/register -H "Content-Type: application/json" \
    -d '{"username":"testuser2","password":"pass456","nickname":"Test2"}')
check "注册 user2" "success" "$R2"

# 重复注册应该失败
R3=$(curl -s -X POST $API/api/register -H "Content-Type: application/json" \
    -d '{"username":"testuser1","password":"pass123","nickname":"Test1"}')
check "重复注册拒绝" "already exists" "$R3"

echo ""
echo "--- 2. 登录 ---"
L1=$(curl -s -X POST $API/api/login -H "Content-Type: application/json" \
    -d '{"username":"testuser1","password":"pass123"}')
check "登录 user1" "token" "$L1"
TOKEN1=$(echo "$L1" | python3 -c "import sys,json; print(json.load(sys.stdin).get('token',''))" 2>/dev/null)
USERID1=$(echo "$L1" | python3 -c "import sys,json; print(json.load(sys.stdin).get('userId',''))" 2>/dev/null)

L2=$(curl -s -X POST $API/api/login -H "Content-Type: application/json" \
    -d '{"username":"testuser2","password":"pass456"}')
check "登录 user2" "token" "$L2"
TOKEN2=$(echo "$L2" | python3 -c "import sys,json; print(json.load(sys.stdin).get('token',''))" 2>/dev/null)
USERID2=$(echo "$L2" | python3 -c "import sys,json; print(json.load(sys.stdin).get('userId',''))" 2>/dev/null)

# 错误密码
L3=$(curl -s -X POST $API/api/login -H "Content-Type: application/json" \
    -d '{"username":"testuser1","password":"wrong"}')
check "错误密码拒绝" "wrong password" "$L3"

echo ""
echo "--- 3. 好友管理 ---"
AUTH1="Authorization: Bearer $TOKEN1"
AUTH2="Authorization: Bearer $TOKEN2"

F1=$(curl -s -X POST $API/api/friends/add -H "$AUTH1" -H "Content-Type: application/json" \
    -d "{\"friendId\":$USERID2}")
check "添加好友" "success" "$F1"

FL=$(curl -s -H "$AUTH1" $API/api/friends)
check "好友列表包含 user2" "testuser2" "$FL"

echo ""
echo "--- 4. 群组管理 ---"
G1=$(curl -s -X POST $API/api/groups/create -H "$AUTH1" -H "Content-Type: application/json" \
    -d '{"name":"测试群"}')
check "创建群组" "success" "$G1"
GROUPID=$(echo "$G1" | python3 -c "import sys,json; print(json.load(sys.stdin).get('groupId',''))" 2>/dev/null)

G2=$(curl -s -X POST $API/api/groups/join -H "$AUTH2" -H "Content-Type: application/json" \
    -d "{\"groupId\":$GROUPID}")
check "加入群组" "success" "$G2"

GM=$(curl -s -H "$AUTH1" "$API/api/groups/members?groupId=$GROUPID")
check "群成员包含 user2" "testuser2" "$GM"

GL=$(curl -s -H "$AUTH2" $API/api/groups)
check "user2 群列表" "测试群" "$GL"

echo ""
echo "--- 5. 未认证请求 ---"
U1=$(curl -s $API/api/friends)
check "无 token 返回 401" "unauthorized" "$U1"

echo ""
echo "========================================"
echo "  结果: $PASS 通过, $FAIL 失败"
echo "========================================"

if [ $FAIL -gt 0 ]; then
    exit 1
fi
