# muduo-im 部署指南

## 环境要求

| 组件 | 最低版本 | 说明 |
|------|---------|------|
| GCC | 7+ | 需要 C++17 支持 |
| CMake | 3.10+ | 构建系统 |
| MySQL | 8.0+ | 消息和用户数据存储 |
| Redis | 6.0+ | 连接池已集成（预留扩展） |
| OpenSSL | 1.1+ | JWT 签名 + 密码哈希 |
| zlib | - | HTTP 压缩支持 |
| libmysqlclient | - | MySQL C 客户端库 |
| hiredis | - | Redis C 客户端库 |
| nlohmann/json | 3.11+ | JSON 解析（自动 FetchContent 下载） |

## 依赖安装

### Ubuntu / Debian

```bash
sudo apt update
sudo apt install -y build-essential cmake \
    libmysqlclient-dev libhiredis-dev \
    libssl-dev zlib1g-dev \
    mysql-server redis-server
```

### CentOS / Rocky Linux

```bash
sudo dnf install -y gcc-c++ cmake \
    mysql-devel hiredis-devel \
    openssl-devel zlib-devel \
    mysql-server redis
```

### 新增依赖

- libargon2-dev：Argon2id 密码哈希库

```bash
sudo apt install -y libargon2-dev         # Ubuntu / Debian
sudo dnf install -y libargon2-devel       # CentOS / Rocky（若可用）
```

### Redis 持久化配置（生产环境必须）

生产环境必须启用 AOF 持久化避免消息丢失。详见 [REDIS_CONFIG.md](REDIS_CONFIG.md)。

快速启用：
```bash
redis-cli CONFIG SET appendonly yes
redis-cli CONFIG SET appendfsync everysec
redis-cli CONFIG REWRITE
```

## 数据库初始化

启动 MySQL 服务，然后执行初始化脚本:

```bash
# 启动 MySQL
sudo systemctl start mysql

# 创建数据库
mysql -u root -e "CREATE DATABASE IF NOT EXISTS muduo_im DEFAULT CHARACTER SET utf8mb4"

# 初始化表结构（init.sql 不再自带 CREATE DATABASE，需要在命令行指定目标库）
mysql -u root muduo_im < sql/init.sql
```

该脚本会:
1. 在指定库内创建 users、friends、friend_requests、groups、group_members、private_messages、group_messages、audit_log 八张表
2. 建立必要的索引和外键约束（ON DELETE CASCADE）

## 编译

```bash
# 克隆项目（含子模块）
git clone --recursive <repo-url>
cd muduo-im

# 如果已克隆但子模块未初始化
git submodule update --init --recursive

# 编译
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

编译产物: `build/muduo-im`

## 配置

支持通过 `config.ini` 文件配置，无需重新编译:

### 配置文件

```bash
# 复制示例配置
cp config.ini.example config.ini

# 按需修改
vim config.ini
```

配置项示例:

```ini
# MySQL
mysql_host=127.0.0.1
mysql_port=3306
mysql_user=root
mysql_password=
mysql_database=muduo_im
mysql_min_conn=5
mysql_max_conn=20

# Redis
redis_host=127.0.0.1
redis_port=6379
redis_min_conn=3
redis_max_conn=10

# JWT
jwt_secret=muduo-im-jwt-secret-key

# Server
http_port=8080
ws_port=9090
```

生产环境务必修改 `jwt_secret` 为强随机字符串。

### 加载方式

```bash
# 方式 1: 命令行指定配置文件路径
./muduo-im ../config.ini

# 方式 2: 自动加载当前目录下的 config.ini（如存在）
./muduo-im

# 方式 3: 无配置文件，使用代码中的默认值
./muduo-im
```

## 运行

```bash
# 确保 MySQL 和 Redis 正在运行
sudo systemctl start mysql redis

# 启动服务（自动加载同目录 config.ini）
cd build
./muduo-im

# 或指定配置文件路径
./muduo-im ../config.ini
```

启动成功输出:

```
=== muduo-im ===
HTTP API: http://localhost:8080
WebSocket: ws://localhost:9090/ws?token=xxx
```

停止: Ctrl+C（SIGINT）或 kill（SIGTERM），服务会优雅退出。

## 端口说明

| 端口 | 协议 | 用途 |
|------|------|------|
| 8080 | HTTP | REST API + 静态文件（前端页面 + 上传文件） |
| 9090 | WebSocket | 实时消息通信 |

静态文件目录:
- 前端页面: `../web/`（相对于工作目录）
- 上传文件: `../uploads/`（自动创建）

## 测试

### 端到端测试

```bash
# 确保服务正在运行
bash tests/e2e_test.sh
```

测试内容: 注册、登录、密码验证、用户资料查看/修改、修改密码、好友申请/同意/拒绝/列表、群组创建/加入/退出/解散/成员查询/公告/踢人、消息长度校验、消息搜索、文件上传限制、已读状态查询、未认证访问拒绝。

注意: 旧的 `/api/friends/add` 接口已移除，好友添加改为申请制（`/api/friends/request` + `/api/friends/handle`）。

### WebSocket 压力测试（Python）

```bash
# 默认 10 客户端，每客户端 100 条消息
python3 benchmark/ws_benchmark.py

# 自定义参数
python3 benchmark/ws_benchmark.py <客户端数> <每客户端消息数>
python3 benchmark/ws_benchmark.py 50 500
```

输出: 连接成功率、消息发送数、ACK 接收数、QPS、延迟 P50/P99。

### C++ WebSocket 压测客户端

```bash
# 编译后在 build 目录下
cd build

# 用法: ./ws_bench <host> <port> <并发数> <每客户端消息数> <token>
./ws_bench 127.0.0.1 9090 10 1000 <token>
```

与 Python 版本相比，C++ 压测客户端开销更低，适合更高并发场景。

### 手动测试

```bash
# 注册
curl -X POST http://localhost:8080/api/register \
  -H "Content-Type: application/json" \
  -d '{"username":"test","password":"123456"}'

# 登录
curl -X POST http://localhost:8080/api/login \
  -H "Content-Type: application/json" \
  -d '{"username":"test","password":"123456"}'

# WebSocket（使用 websocat 工具）
websocat ws://localhost:9090/ws?token=<TOKEN>
```

## 前端访问

启动服务后打开浏览器访问 `http://localhost:8080`，即可使用内置的聊天界面进行注册、登录和聊天。

## 生产运维

### 优雅关闭

服务接收 SIGTERM/SIGINT 时会：
1. 立即刷写 Redis 消息队列到 MySQL（最后机会持久化）
2. 关闭 HTTP/WebSocket 服务器（拒绝新连接，等待当前请求完成 2s）
3. 退出事件循环

systemd 配置示例：
```ini
[Service]
ExecStart=/path/to/muduo-im /path/to/config.ini
ExecStop=/bin/kill -TERM $MAINPID
TimeoutStopSec=10
KillMode=mixed
```

### Kubernetes 健康探针

```yaml
livenessProbe:
  httpGet:
    path: /health
    port: 8080
  initialDelaySeconds: 5
  periodSeconds: 10

readinessProbe:
  httpGet:
    path: /health
    port: 8080
  initialDelaySeconds: 2
  periodSeconds: 5
```

`/health` 返回 503 时 K8s 会停止流量；返回 200 时恢复流量。

## 环境变量（生产必配）

```bash
# JWT 签名密钥（必须覆盖 config.ini 默认值）
export MUDUO_IM_JWT_SECRET="$(openssl rand -hex 32)"
```

未设置时启动会输出 WARNING 并使用硬编码默认值（仅开发用）。

## 审计日志查询

```sql
-- 最近 100 条敏感操作
SELECT user_id, action, target, ip, created_at
FROM audit_log
ORDER BY id DESC LIMIT 100;

-- 某用户的全部操作
SELECT * FROM audit_log WHERE user_id=<id> ORDER BY id DESC;

-- 登录失败统计
SELECT ip, COUNT(*) as failures
FROM audit_log
WHERE action='login_failed' AND created_at > NOW() - INTERVAL 1 HOUR
GROUP BY ip
ORDER BY failures DESC;
```

## 持续集成

`.github/workflows/ci.yml` 在每次 push/PR 触发：
1. 启动 MySQL 8 + Redis 6 service container
2. 安装 libargon2-dev 等依赖
3. 构建 muduo-im 和 mymuduo-http
4. 运行所有单元测试

CI 徽章：在 GitHub 仓库 Actions 页面可查看。

## Docker 部署

```bash
# 环境变量（必配）
export MUDUO_IM_JWT_SECRET=$(openssl rand -hex 32)

# 一键启动
docker-compose up -d

# 查看日志
docker-compose logs -f muduo-im

# 停止
docker-compose down
```

镜像含 MySQL + Redis + muduo-im 完整栈。更多细节见 TECHNICAL_DETAILS.md 的「生产级部署」小节。
