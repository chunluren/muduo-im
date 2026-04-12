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

## 数据库初始化

启动 MySQL 服务，然后执行初始化脚本:

```bash
# 启动 MySQL
sudo systemctl start mysql

# 初始化数据库和表
mysql -u root < sql/init.sql
```

该脚本会:
1. 创建 `muduo_im` 数据库（utf8mb4 字符集）
2. 创建 users、friends、friend_requests、groups、group_members、private_messages、group_messages 七张表
3. 建立必要的索引

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

当前配置硬编码在 `src/server/main.cpp` 中，修改后需重新编译:

### MySQL 连接

```cpp
MySQLPoolConfig mysqlConfig;
mysqlConfig.host = "127.0.0.1";    // MySQL 地址
mysqlConfig.port = 3306;            // MySQL 端口
mysqlConfig.user = "root";          // 用户名
mysqlConfig.password = "";           // 密码
mysqlConfig.database = "muduo_im";   // 数据库名
mysqlConfig.minSize = 5;            // 连接池最小连接数
mysqlConfig.maxSize = 20;           // 连接池最大连接数
```

### Redis 连接

```cpp
RedisPoolConfig redisConfig;
redisConfig.host = "127.0.0.1";    // Redis 地址
redisConfig.port = 6379;            // Redis 端口
redisConfig.minSize = 3;            // 连接池最小连接数
redisConfig.maxSize = 10;           // 连接池最大连接数
```

### JWT 密钥

```cpp
ChatServer server(&loop, 8080, 9090, mysqlConfig, redisConfig, "muduo-im-jwt-secret-key");
```

生产环境务必修改 JWT 密钥为强随机字符串。

## 运行

```bash
# 确保 MySQL 和 Redis 正在运行
sudo systemctl start mysql redis

# 启动服务
cd build
./muduo-im
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

测试内容（共 21 项检查）: 注册、登录、密码验证、用户资料查看/修改、修改密码、好友申请/同意/拒绝/列表、群组创建/加入/退出/解散/成员查询、消息长度校验、文件上传限制、未认证访问拒绝。

注意: 旧的 `/api/friends/add` 接口已移除，好友添加改为申请制（`/api/friends/request` + `/api/friends/handle`）。

### WebSocket 压力测试

```bash
# 默认 10 客户端，每客户端 100 条消息
python3 benchmark/ws_benchmark.py

# 自定义参数
python3 benchmark/ws_benchmark.py <客户端数> <每客户端消息数>
python3 benchmark/ws_benchmark.py 50 500
```

输出: 连接成功率、消息发送数、ACK 接收数、QPS、延迟 P50/P99。

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
