# Phase 3: muduo-im 服务端 实施计划

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 基于 mymuduo-http 网络库，构建一个支持单聊/群聊、好友管理、在线状态的 IM 通信服务端。

**Architecture:** 单进程多线程架构。HttpServer 提供 REST API（注册/登录/好友/群组），WebSocketServer 提供实时消息推送。MySQL 存持久化数据，Redis 做在线状态和消息队列。JWT 认证贯穿 HTTP 和 WebSocket。

**Tech Stack:** C++17, mymuduo-http (WebSocket + HTTP + MySQLPool + RedisPool), OpenSSL (HMAC-SHA256 for JWT), nlohmann/json

---

## Task 1: 项目脚手架

**Files:**
- Create: `CMakeLists.txt`
- Create: `sql/init.sql`
- Create: `src/server/main.cpp` (空 main，编译验证)

### Step 1: 初始化 git 仓库和 submodule

```bash
cd /home/ly/workspaces/im
mkdir -p muduo-im && cd muduo-im
git init
git submodule add ../mymuduo-http third_party/mymuduo-http
mkdir -p src/server src/db src/common sql web tests docs/plans
```

### Step 2: 创建 CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.10)
project(muduo-im)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wall -Wextra -O2")

# 第三方: mymuduo-http
add_subdirectory(third_party/mymuduo-http)

# 查找依赖
find_package(OpenSSL REQUIRED)
find_package(ZLIB REQUIRED)

find_path(MYSQL_INCLUDE_DIR mysql/mysql.h)
find_library(MYSQL_LIBRARY mysqlclient)

find_path(REDIS_INCLUDE_DIR hiredis/hiredis.h)
find_library(REDIS_LIBRARY hiredis)

# nlohmann/json
find_package(nlohmann_json QUIET)
if(NOT nlohmann_json_FOUND)
    include(FetchContent)
    FetchContent_Declare(json URL https://github.com/nlohmann/json/releases/download/v3.11.2/json.tar.xz)
    FetchContent_MakeAvailable(json)
endif()

# 头文件
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mymuduo-http/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mymuduo-http/src/net
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mymuduo-http/src/http
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mymuduo-http/src/websocket
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mymuduo-http/src/pool
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/mymuduo-http/src/util
    ${MYSQL_INCLUDE_DIR}
    ${REDIS_INCLUDE_DIR}
)

# 主服务
add_executable(muduo-im src/server/main.cpp)
target_link_libraries(muduo-im
    mymuduo
    nlohmann_json::nlohmann_json
    OpenSSL::SSL OpenSSL::Crypto
    ZLIB::ZLIB
    ${MYSQL_LIBRARY}
    ${REDIS_LIBRARY}
    pthread
)
```

### Step 3: 创建 sql/init.sql

```sql
CREATE DATABASE IF NOT EXISTS muduo_im DEFAULT CHARACTER SET utf8mb4;
USE muduo_im;

CREATE TABLE IF NOT EXISTS users (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    username VARCHAR(64) UNIQUE NOT NULL,
    password VARCHAR(128) NOT NULL,
    nickname VARCHAR(64),
    avatar VARCHAR(256) DEFAULT '',
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS friends (
    user_id BIGINT NOT NULL,
    friend_id BIGINT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (user_id, friend_id),
    INDEX idx_user (user_id)
);

CREATE TABLE IF NOT EXISTS `groups` (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    name VARCHAR(128) NOT NULL,
    owner_id BIGINT NOT NULL,
    created_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP
);

CREATE TABLE IF NOT EXISTS group_members (
    group_id BIGINT NOT NULL,
    user_id BIGINT NOT NULL,
    joined_at TIMESTAMP DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (group_id, user_id),
    INDEX idx_user_groups (user_id)
);

CREATE TABLE IF NOT EXISTS private_messages (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    msg_id VARCHAR(36) UNIQUE,
    from_user BIGINT NOT NULL,
    to_user BIGINT NOT NULL,
    content TEXT,
    msg_type TINYINT DEFAULT 0,
    timestamp BIGINT NOT NULL,
    INDEX idx_chat (from_user, to_user, timestamp),
    INDEX idx_inbox (to_user, timestamp)
);

CREATE TABLE IF NOT EXISTS group_messages (
    id BIGINT PRIMARY KEY AUTO_INCREMENT,
    msg_id VARCHAR(36) UNIQUE,
    group_id BIGINT NOT NULL,
    from_user BIGINT NOT NULL,
    content TEXT,
    msg_type TINYINT DEFAULT 0,
    timestamp BIGINT NOT NULL,
    INDEX idx_group_time (group_id, timestamp)
);
```

### Step 4: 创建空 main.cpp

```cpp
#include <iostream>

int main() {
    std::cout << "muduo-im server starting..." << std::endl;
    return 0;
}
```

### Step 5: 编译验证

```bash
mkdir build && cd build && cmake .. && make -j$(nproc)
```

### Step 6: Commit

```bash
git add -A
git commit -m "init: muduo-im project scaffold with CMake and SQL schema"
```

---

## Task 2: JWT + Protocol 通用模块

**Files:**
- Create: `src/common/JWT.h`
- Create: `src/common/Protocol.h`

### Step 1: 实现 JWT.h

基于 OpenSSL HMAC-SHA256 的简单 JWT 实现:

```cpp
#pragma once

#include <string>
#include <chrono>
#include <openssl/hmac.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class JWT {
public:
    explicit JWT(const std::string& secret) : secret_(secret) {}

    /// 生成 token
    std::string generate(int64_t userId, int expireSeconds = 86400) {
        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        json header = {{"alg", "HS256"}, {"typ", "JWT"}};
        json payload = {
            {"userId", userId},
            {"iat", now},
            {"exp", now + expireSeconds}
        };

        std::string headerB64 = base64UrlEncode(header.dump());
        std::string payloadB64 = base64UrlEncode(payload.dump());
        std::string message = headerB64 + "." + payloadB64;
        std::string signature = base64UrlEncode(hmacSha256(message));

        return message + "." + signature;
    }

    /// 验证 token，返回 userId，失败返回 -1
    int64_t verify(const std::string& token) {
        // 拆分三部分
        auto dot1 = token.find('.');
        if (dot1 == std::string::npos) return -1;
        auto dot2 = token.find('.', dot1 + 1);
        if (dot2 == std::string::npos) return -1;

        std::string message = token.substr(0, dot2);
        std::string signature = token.substr(dot2 + 1);

        // 验证签名
        std::string expected = base64UrlEncode(hmacSha256(message));
        if (signature != expected) return -1;

        // 解析 payload
        std::string payloadStr = base64UrlDecode(token.substr(dot1 + 1, dot2 - dot1 - 1));
        try {
            json payload = json::parse(payloadStr);
            int64_t exp = payload["exp"].get<int64_t>();
            int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (now > exp) return -1;  // 过期
            return payload["userId"].get<int64_t>();
        } catch (...) {
            return -1;
        }
    }

private:
    std::string hmacSha256(const std::string& data) {
        unsigned char result[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        HMAC(EVP_sha256(), secret_.c_str(), secret_.size(),
             reinterpret_cast<const unsigned char*>(data.c_str()), data.size(),
             result, &len);
        return std::string(reinterpret_cast<char*>(result), len);
    }

    static std::string base64UrlEncode(const std::string& input) {
        static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        int i = 0;
        unsigned char arr3[3], arr4[4];
        int inLen = input.size();
        const unsigned char* bytesToEncode = reinterpret_cast<const unsigned char*>(input.c_str());

        while (inLen--) {
            arr3[i++] = *(bytesToEncode++);
            if (i == 3) {
                arr4[0] = (arr3[0] & 0xfc) >> 2;
                arr4[1] = ((arr3[0] & 0x03) << 4) + ((arr3[1] & 0xf0) >> 4);
                arr4[2] = ((arr3[1] & 0x0f) << 2) + ((arr3[2] & 0xc0) >> 6);
                arr4[3] = arr3[2] & 0x3f;
                for (i = 0; i < 4; i++) result += chars[arr4[i]];
                i = 0;
            }
        }
        if (i) {
            for (int j = i; j < 3; j++) arr3[j] = '\0';
            arr4[0] = (arr3[0] & 0xfc) >> 2;
            arr4[1] = ((arr3[0] & 0x03) << 4) + ((arr3[1] & 0xf0) >> 4);
            arr4[2] = ((arr3[1] & 0x0f) << 2) + ((arr3[2] & 0xc0) >> 6);
            for (int j = 0; j < i + 1; j++) result += chars[arr4[j]];
        }
        // URL-safe: + → -, / → _, 去掉 =
        for (auto& c : result) {
            if (c == '+') c = '-';
            else if (c == '/') c = '_';
        }
        return result;
    }

    static std::string base64UrlDecode(const std::string& input) {
        std::string s = input;
        for (auto& c : s) {
            if (c == '-') c = '+';
            else if (c == '_') c = '/';
        }
        while (s.size() % 4) s += '=';

        static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        int i = 0;
        unsigned char arr4[4], arr3[3];

        for (char c : s) {
            if (c == '=') break;
            auto pos = chars.find(c);
            if (pos == std::string::npos) continue;
            arr4[i++] = pos;
            if (i == 4) {
                arr3[0] = (arr4[0] << 2) + ((arr4[1] & 0x30) >> 4);
                arr3[1] = ((arr4[1] & 0xf) << 4) + ((arr4[2] & 0x3c) >> 2);
                arr3[2] = ((arr4[2] & 0x3) << 6) + arr4[3];
                for (i = 0; i < 3; i++) result += arr3[i];
                i = 0;
            }
        }
        if (i) {
            for (int j = i; j < 4; j++) arr4[j] = 0;
            arr3[0] = (arr4[0] << 2) + ((arr4[1] & 0x30) >> 4);
            arr3[1] = ((arr4[1] & 0xf) << 4) + ((arr4[2] & 0x3c) >> 2);
            for (int j = 0; j < i - 1; j++) result += arr3[j];
        }
        return result;
    }

    std::string secret_;
};
```

### Step 2: 实现 Protocol.h

```cpp
#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>

using json = nlohmann::json;

namespace Protocol {

// 消息类型
inline const char* MSG         = "msg";
inline const char* GROUP_MSG   = "group_msg";
inline const char* ACK         = "ack";
inline const char* ONLINE      = "online";
inline const char* OFFLINE     = "offline";
inline const char* ERROR_MSG   = "error";

/// 生成 UUID v4
inline std::string generateMsgId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

    uint32_t a = dist(gen), b = dist(gen), c = dist(gen), d = dist(gen);
    // 设置 version 4 和 variant bits
    b = (b & 0xFFFF0FFF) | 0x00004000;
    c = (c & 0x3FFFFFFF) | 0x80000000;

    char buf[37];
    snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%04x%08x",
             a, (b >> 16) & 0xFFFF, b & 0xFFFF,
             (c >> 16) & 0xFFFF, c & 0xFFFF, d);
    return buf;
}

/// 当前时间戳（毫秒）
inline int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

/// 构造私聊消息 (服务端→客户端)
inline std::string makePrivateMsg(int64_t from, int64_t to, const std::string& content, const std::string& msgId) {
    json j;
    j["type"] = MSG;
    j["from"] = std::to_string(from);
    j["to"] = std::to_string(to);
    j["content"] = content;
    j["msgId"] = msgId;
    j["timestamp"] = nowMs();
    return j.dump();
}

/// 构造群聊消息 (服务端→客户端)
inline std::string makeGroupMsg(int64_t from, int64_t groupId, const std::string& content, const std::string& msgId) {
    json j;
    j["type"] = GROUP_MSG;
    j["from"] = std::to_string(from);
    j["to"] = std::to_string(groupId);
    j["content"] = content;
    j["msgId"] = msgId;
    j["timestamp"] = nowMs();
    return j.dump();
}

/// 构造 ACK
inline std::string makeAck(const std::string& msgId) {
    return json({{"type", ACK}, {"msgId", msgId}}).dump();
}

/// 构造在线通知
inline std::string makeOnline(int64_t userId) {
    return json({{"type", ONLINE}, {"userId", std::to_string(userId)}}).dump();
}

/// 构造离线通知
inline std::string makeOffline(int64_t userId) {
    return json({{"type", OFFLINE}, {"userId", std::to_string(userId)}}).dump();
}

/// 构造错误消息
inline std::string makeError(const std::string& message) {
    return json({{"type", ERROR_MSG}, {"message", message}}).dump();
}

/// SHA256 哈希（密码存储用）
inline std::string sha256(const std::string& input) {
    unsigned char hash[32];
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, input.c_str(), input.size());
    EVP_DigestFinal_ex(ctx, hash, nullptr);
    EVP_MD_CTX_free(ctx);

    std::ostringstream oss;
    for (int i = 0; i < 32; ++i)
        oss << std::hex << std::setfill('0') << std::setw(2) << (int)hash[i];
    return oss.str();
}

/// 密码哈希（salt + SHA256）
inline std::string hashPassword(const std::string& password) {
    std::string salt = "muduo-im-salt-v1";  // 简化: 固定 salt
    return sha256(salt + password);
}

/// 验证密码
inline bool verifyPassword(const std::string& password, const std::string& hashed) {
    return hashPassword(password) == hashed;
}

}  // namespace Protocol
```

### Step 3: 编译验证

修改 main.cpp 加入 include 验证:
```cpp
#include <iostream>
#include "common/JWT.h"
#include "common/Protocol.h"

int main() {
    JWT jwt("test-secret");
    auto token = jwt.generate(1);
    auto userId = jwt.verify(token);
    std::cout << "JWT verify: userId=" << userId << std::endl;
    std::cout << "MsgId: " << Protocol::generateMsgId() << std::endl;
    return 0;
}
```

```bash
cd build && cmake .. && make -j$(nproc) && ./muduo-im
```

### Step 4: Commit

```bash
git add -A
git commit -m "feat: add JWT auth and Protocol utilities"
```

---

## Task 3: UserService

**Files:**
- Create: `src/server/UserService.h`

### 实现

```cpp
#pragma once

#include "common/JWT.h"
#include "common/Protocol.h"
#include "pool/MySQLPool.h"
#include <nlohmann/json.hpp>
#include <string>
#include <memory>

using json = nlohmann::json;

class UserService {
public:
    UserService(std::shared_ptr<MySQLPool> db, const std::string& jwtSecret)
        : db_(db), jwt_(jwtSecret) {}

    /// 注册
    json registerUser(const std::string& username, const std::string& password, const std::string& nickname) {
        if (username.empty() || password.empty()) {
            return {{"success", false}, {"message", "username and password required"}};
        }
        if (username.size() > 64 || password.size() > 64) {
            return {{"success", false}, {"message", "username or password too long"}};
        }

        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) {
            return {{"success", false}, {"message", "database error"}};
        }

        // 检查用户名是否已存在
        std::string escaped = conn->escape(username);
        auto result = conn->query("SELECT id FROM users WHERE username='" + escaped + "'");
        if (result && mysql_num_rows(result.get()) > 0) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "username already exists"}};
        }

        // 插入用户
        std::string hashedPwd = Protocol::hashPassword(password);
        std::string nick = nickname.empty() ? username : nickname;
        std::string sql = "INSERT INTO users (username, password, nickname) VALUES ('"
            + escaped + "', '" + hashedPwd + "', '" + conn->escape(nick) + "')";

        int affected = conn->execute(sql);
        if (affected <= 0) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "register failed"}};
        }

        int64_t userId = conn->lastInsertId();
        db_->release(std::move(conn));

        return {{"success", true}, {"userId", userId}, {"message", "registered"}};
    }

    /// 登录
    json login(const std::string& username, const std::string& password) {
        if (username.empty() || password.empty()) {
            return {{"success", false}, {"message", "username and password required"}};
        }

        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) {
            return {{"success", false}, {"message", "database error"}};
        }

        std::string escaped = conn->escape(username);
        auto result = conn->query("SELECT id, password, nickname FROM users WHERE username='" + escaped + "'");
        db_->release(std::move(conn));

        if (!result || mysql_num_rows(result.get()) == 0) {
            return {{"success", false}, {"message", "user not found"}};
        }

        MYSQL_ROW row = mysql_fetch_row(result.get());
        int64_t userId = std::stoll(row[0]);
        std::string storedHash = row[1];
        std::string nickname = row[2] ? row[2] : "";

        if (!Protocol::verifyPassword(password, storedHash)) {
            return {{"success", false}, {"message", "wrong password"}};
        }

        std::string token = jwt_.generate(userId);
        return {
            {"success", true},
            {"token", token},
            {"userId", userId},
            {"nickname", nickname}
        };
    }

    /// 从 token 获取 userId
    int64_t verifyToken(const std::string& token) {
        return jwt_.verify(token);
    }

    JWT& jwt() { return jwt_; }

private:
    std::shared_ptr<MySQLPool> db_;
    JWT jwt_;
};
```

### Commit

```bash
git add src/server/UserService.h
git commit -m "feat: add UserService with register/login/JWT"
```

---

## Task 4: OnlineManager + FriendService + GroupService

**Files:**
- Create: `src/server/OnlineManager.h`
- Create: `src/server/FriendService.h`
- Create: `src/server/GroupService.h`

### OnlineManager.h

```cpp
#pragma once

#include "websocket/WsSession.h"
#include <unordered_map>
#include <mutex>
#include <vector>

class OnlineManager {
public:
    void addUser(int64_t userId, const WsSessionPtr& session) {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_[userId] = session;
    }

    void removeUser(int64_t userId) {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.erase(userId);
    }

    WsSessionPtr getSession(int64_t userId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(userId);
        if (it != sessions_.end() && it->second->isOpen()) {
            return it->second;
        }
        return nullptr;
    }

    bool isOnline(int64_t userId) {
        return getSession(userId) != nullptr;
    }

    std::vector<int64_t> getOnlineUsers() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<int64_t> users;
        for (auto& [id, session] : sessions_) {
            if (session->isOpen()) users.push_back(id);
        }
        return users;
    }

    int64_t getUserId(const WsSessionPtr& session) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, s] : sessions_) {
            if (s == session) return id;
        }
        return -1;
    }

private:
    std::unordered_map<int64_t, WsSessionPtr> sessions_;
    std::mutex mutex_;
};
```

### FriendService.h

```cpp
#pragma once

#include "pool/MySQLPool.h"
#include <nlohmann/json.hpp>
#include <memory>

using json = nlohmann::json;

class FriendService {
public:
    explicit FriendService(std::shared_ptr<MySQLPool> db) : db_(db) {}

    json getFriends(int64_t userId) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return json::array();

        std::string sql = "SELECT u.id, u.username, u.nickname, u.avatar FROM friends f "
                          "JOIN users u ON f.friend_id = u.id WHERE f.user_id=" + std::to_string(userId);
        auto result = conn->query(sql);
        db_->release(std::move(conn));

        json friends = json::array();
        if (result) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(result.get()))) {
                friends.push_back({
                    {"userId", std::stoll(row[0])},
                    {"username", row[1]},
                    {"nickname", row[2] ? row[2] : ""},
                    {"avatar", row[3] ? row[3] : ""}
                });
            }
        }
        return friends;
    }

    json addFriend(int64_t userId, int64_t friendId) {
        if (userId == friendId) return {{"success", false}, {"message", "cannot add self"}};

        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {{"success", false}, {"message", "db error"}};

        // 双向插入
        std::string sql1 = "INSERT IGNORE INTO friends (user_id, friend_id) VALUES ("
            + std::to_string(userId) + "," + std::to_string(friendId) + ")";
        std::string sql2 = "INSERT IGNORE INTO friends (user_id, friend_id) VALUES ("
            + std::to_string(friendId) + "," + std::to_string(userId) + ")";

        conn->execute(sql1);
        conn->execute(sql2);
        db_->release(std::move(conn));

        return {{"success", true}};
    }

    json deleteFriend(int64_t userId, int64_t friendId) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {{"success", false}, {"message", "db error"}};

        conn->execute("DELETE FROM friends WHERE user_id=" + std::to_string(userId)
                      + " AND friend_id=" + std::to_string(friendId));
        conn->execute("DELETE FROM friends WHERE user_id=" + std::to_string(friendId)
                      + " AND friend_id=" + std::to_string(userId));
        db_->release(std::move(conn));

        return {{"success", true}};
    }

private:
    std::shared_ptr<MySQLPool> db_;
};
```

### GroupService.h

```cpp
#pragma once

#include "pool/MySQLPool.h"
#include <nlohmann/json.hpp>
#include <memory>
#include <vector>

using json = nlohmann::json;

class GroupService {
public:
    explicit GroupService(std::shared_ptr<MySQLPool> db) : db_(db) {}

    json getUserGroups(int64_t userId) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return json::array();

        std::string sql = "SELECT g.id, g.name, g.owner_id FROM group_members gm "
                          "JOIN `groups` g ON gm.group_id = g.id WHERE gm.user_id=" + std::to_string(userId);
        auto result = conn->query(sql);
        db_->release(std::move(conn));

        json groups = json::array();
        if (result) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(result.get()))) {
                groups.push_back({
                    {"groupId", std::stoll(row[0])},
                    {"name", row[1]},
                    {"ownerId", std::stoll(row[2])}
                });
            }
        }
        return groups;
    }

    json createGroup(int64_t ownerId, const std::string& name) {
        if (name.empty()) return {{"success", false}, {"message", "name required"}};

        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {{"success", false}, {"message", "db error"}};

        std::string sql = "INSERT INTO `groups` (name, owner_id) VALUES ('"
            + conn->escape(name) + "'," + std::to_string(ownerId) + ")";
        conn->execute(sql);
        int64_t groupId = conn->lastInsertId();

        // 创建者自动加入群
        conn->execute("INSERT INTO group_members (group_id, user_id) VALUES ("
            + std::to_string(groupId) + "," + std::to_string(ownerId) + ")");
        db_->release(std::move(conn));

        return {{"success", true}, {"groupId", groupId}};
    }

    json joinGroup(int64_t userId, int64_t groupId) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {{"success", false}, {"message", "db error"}};

        conn->execute("INSERT IGNORE INTO group_members (group_id, user_id) VALUES ("
            + std::to_string(groupId) + "," + std::to_string(userId) + ")");
        db_->release(std::move(conn));

        return {{"success", true}};
    }

    json getMembers(int64_t groupId) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return json::array();

        std::string sql = "SELECT u.id, u.username, u.nickname FROM group_members gm "
                          "JOIN users u ON gm.user_id = u.id WHERE gm.group_id=" + std::to_string(groupId);
        auto result = conn->query(sql);
        db_->release(std::move(conn));

        json members = json::array();
        if (result) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(result.get()))) {
                members.push_back({
                    {"userId", std::stoll(row[0])},
                    {"username", row[1]},
                    {"nickname", row[2] ? row[2] : ""}
                });
            }
        }
        return members;
    }

    std::vector<int64_t> getMemberIds(int64_t groupId) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {};

        auto result = conn->query("SELECT user_id FROM group_members WHERE group_id=" + std::to_string(groupId));
        db_->release(std::move(conn));

        std::vector<int64_t> ids;
        if (result) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(result.get()))) {
                ids.push_back(std::stoll(row[0]));
            }
        }
        return ids;
    }

private:
    std::shared_ptr<MySQLPool> db_;
};
```

### Commit

```bash
git add src/server/OnlineManager.h src/server/FriendService.h src/server/GroupService.h
git commit -m "feat: add OnlineManager, FriendService, GroupService"
```

---

## Task 5: MessageService

**Files:**
- Create: `src/server/MessageService.h`

```cpp
#pragma once

#include "pool/MySQLPool.h"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>

using json = nlohmann::json;

class MessageService {
public:
    explicit MessageService(std::shared_ptr<MySQLPool> db) : db_(db) {}

    /// 存储私聊消息
    bool savePrivateMessage(const std::string& msgId, int64_t from, int64_t to,
                            const std::string& content, int64_t timestamp) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return false;

        std::string sql = "INSERT INTO private_messages (msg_id, from_user, to_user, content, timestamp) VALUES ('"
            + conn->escape(msgId) + "'," + std::to_string(from) + "," + std::to_string(to)
            + ",'" + conn->escape(content) + "'," + std::to_string(timestamp) + ")";

        int ret = conn->execute(sql);
        db_->release(std::move(conn));
        return ret > 0;
    }

    /// 存储群聊消息
    bool saveGroupMessage(const std::string& msgId, int64_t groupId, int64_t from,
                          const std::string& content, int64_t timestamp) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return false;

        std::string sql = "INSERT INTO group_messages (msg_id, group_id, from_user, content, timestamp) VALUES ('"
            + conn->escape(msgId) + "'," + std::to_string(groupId) + "," + std::to_string(from)
            + ",'" + conn->escape(content) + "'," + std::to_string(timestamp) + ")";

        int ret = conn->execute(sql);
        db_->release(std::move(conn));
        return ret > 0;
    }

    /// 查询私聊历史消息
    json getPrivateHistory(int64_t userId, int64_t peerId, int limit = 50, int64_t before = 0) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return json::array();

        std::string sql = "SELECT msg_id, from_user, to_user, content, timestamp FROM private_messages "
            "WHERE ((from_user=" + std::to_string(userId) + " AND to_user=" + std::to_string(peerId) + ") "
            "OR (from_user=" + std::to_string(peerId) + " AND to_user=" + std::to_string(userId) + "))";
        if (before > 0) sql += " AND timestamp<" + std::to_string(before);
        sql += " ORDER BY timestamp DESC LIMIT " + std::to_string(limit);

        auto result = conn->query(sql);
        db_->release(std::move(conn));

        json messages = json::array();
        if (result) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(result.get()))) {
                messages.push_back({
                    {"msgId", row[0]},
                    {"from", std::stoll(row[1])},
                    {"to", std::stoll(row[2])},
                    {"content", row[3]},
                    {"timestamp", std::stoll(row[4])}
                });
            }
        }
        return messages;
    }

    /// 查询群聊历史消息
    json getGroupHistory(int64_t groupId, int limit = 50, int64_t before = 0) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return json::array();

        std::string sql = "SELECT msg_id, from_user, content, timestamp FROM group_messages "
            "WHERE group_id=" + std::to_string(groupId);
        if (before > 0) sql += " AND timestamp<" + std::to_string(before);
        sql += " ORDER BY timestamp DESC LIMIT " + std::to_string(limit);

        auto result = conn->query(sql);
        db_->release(std::move(conn));

        json messages = json::array();
        if (result) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(result.get()))) {
                messages.push_back({
                    {"msgId", row[0]},
                    {"from", std::stoll(row[1])},
                    {"content", row[2]},
                    {"timestamp", std::stoll(row[3])}
                });
            }
        }
        return messages;
    }

private:
    std::shared_ptr<MySQLPool> db_;
};
```

### Commit

```bash
git add src/server/MessageService.h
git commit -m "feat: add MessageService for message storage and history"
```

---

## Task 6: ChatServer — 主服务组装

**Files:**
- Create: `src/server/ChatServer.h`
- Modify: `src/server/main.cpp`

### ChatServer.h

组装 HttpServer + WebSocketServer + 所有 Service:

```cpp
#pragma once

#include "UserService.h"
#include "FriendService.h"
#include "GroupService.h"
#include "MessageService.h"
#include "OnlineManager.h"
#include "common/Protocol.h"
#include "common/JWT.h"
#include "pool/MySQLPool.h"
#include "pool/RedisPool.h"
#include "http/HttpServer.h"
#include "websocket/WebSocketServer.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include <nlohmann/json.hpp>
#include <memory>
#include <iostream>

using json = nlohmann::json;

class ChatServer {
public:
    ChatServer(EventLoop* loop, uint16_t httpPort, uint16_t wsPort,
               const MySQLPoolConfig& mysqlConfig, const RedisPoolConfig& redisConfig,
               const std::string& jwtSecret)
        : loop_(loop)
        , httpServer_(loop, InetAddress(httpPort), "HttpAPI")
        , wsServer_(loop, InetAddress(wsPort), "WebSocket")
        , db_(std::make_shared<MySQLPool>(mysqlConfig))
        , redis_(std::make_shared<RedisPool>(redisConfig))
        , userService_(db_, jwtSecret)
        , friendService_(db_)
        , groupService_(db_)
        , messageService_(db_)
    {
        setupHttpRoutes();
        setupWebSocket();
    }

    void start() {
        httpServer_.start();
        wsServer_.start();
        std::cout << "ChatServer started." << std::endl;
    }

private:
    // ==================== HTTP REST API ====================
    void setupHttpRoutes() {
        httpServer_.enableCors();
        httpServer_.setThreadNum(2);

        // 注册
        httpServer_.POST("/api/register", [this](const HttpRequest& req, HttpResponse& resp) {
            auto body = json::parse(req.body, nullptr, false);
            if (body.is_discarded()) { resp = HttpResponse::badRequest("invalid json"); return; }
            auto result = userService_.registerUser(
                body.value("username", ""), body.value("password", ""), body.value("nickname", ""));
            resp.setJson(result.dump());
        });

        // 登录
        httpServer_.POST("/api/login", [this](const HttpRequest& req, HttpResponse& resp) {
            auto body = json::parse(req.body, nullptr, false);
            if (body.is_discarded()) { resp = HttpResponse::badRequest("invalid json"); return; }
            auto result = userService_.login(body.value("username", ""), body.value("password", ""));
            resp.setJson(result.dump());
        });

        // 好友列表
        httpServer_.GET("/api/friends", [this](const HttpRequest& req, HttpResponse& resp) {
            int64_t userId = authFromRequest(req);
            if (userId < 0) { resp = HttpResponse::badRequest("unauthorized"); resp.setStatusCode(HttpStatusCode::UNAUTHORIZED); return; }
            resp.setJson(friendService_.getFriends(userId).dump());
        });

        // 添加好友
        httpServer_.POST("/api/friends/add", [this](const HttpRequest& req, HttpResponse& resp) {
            int64_t userId = authFromRequest(req);
            if (userId < 0) { resp.setStatusCode(HttpStatusCode::UNAUTHORIZED); resp.setText("unauthorized"); return; }
            auto body = json::parse(req.body, nullptr, false);
            if (body.is_discarded()) { resp = HttpResponse::badRequest("invalid json"); return; }
            int64_t friendId = body.value("friendId", (int64_t)0);
            resp.setJson(friendService_.addFriend(userId, friendId).dump());
        });

        // 删除好友
        httpServer_.POST("/api/friends/delete", [this](const HttpRequest& req, HttpResponse& resp) {
            int64_t userId = authFromRequest(req);
            if (userId < 0) { resp.setStatusCode(HttpStatusCode::UNAUTHORIZED); resp.setText("unauthorized"); return; }
            auto body = json::parse(req.body, nullptr, false);
            int64_t friendId = body.value("friendId", (int64_t)0);
            resp.setJson(friendService_.deleteFriend(userId, friendId).dump());
        });

        // 群列表
        httpServer_.GET("/api/groups", [this](const HttpRequest& req, HttpResponse& resp) {
            int64_t userId = authFromRequest(req);
            if (userId < 0) { resp.setStatusCode(HttpStatusCode::UNAUTHORIZED); resp.setText("unauthorized"); return; }
            resp.setJson(groupService_.getUserGroups(userId).dump());
        });

        // 创建群
        httpServer_.POST("/api/groups/create", [this](const HttpRequest& req, HttpResponse& resp) {
            int64_t userId = authFromRequest(req);
            if (userId < 0) { resp.setStatusCode(HttpStatusCode::UNAUTHORIZED); resp.setText("unauthorized"); return; }
            auto body = json::parse(req.body, nullptr, false);
            resp.setJson(groupService_.createGroup(userId, body.value("name", "")).dump());
        });

        // 加入群
        httpServer_.POST("/api/groups/join", [this](const HttpRequest& req, HttpResponse& resp) {
            int64_t userId = authFromRequest(req);
            if (userId < 0) { resp.setStatusCode(HttpStatusCode::UNAUTHORIZED); resp.setText("unauthorized"); return; }
            auto body = json::parse(req.body, nullptr, false);
            int64_t groupId = body.value("groupId", (int64_t)0);
            resp.setJson(groupService_.joinGroup(userId, groupId).dump());
        });

        // 群成员
        httpServer_.GET("/api/groups/members", [this](const HttpRequest& req, HttpResponse& resp) {
            int64_t userId = authFromRequest(req);
            if (userId < 0) { resp.setStatusCode(HttpStatusCode::UNAUTHORIZED); resp.setText("unauthorized"); return; }
            int64_t groupId = std::stoll(req.getParam("groupId", "0"));
            resp.setJson(groupService_.getMembers(groupId).dump());
        });

        // 历史消息
        httpServer_.GET("/api/messages/history", [this](const HttpRequest& req, HttpResponse& resp) {
            int64_t userId = authFromRequest(req);
            if (userId < 0) { resp.setStatusCode(HttpStatusCode::UNAUTHORIZED); resp.setText("unauthorized"); return; }
            int64_t peerId = std::stoll(req.getParam("peerId", "0"));
            int64_t groupId = std::stoll(req.getParam("groupId", "0"));
            int64_t before = std::stoll(req.getParam("before", "0"));

            if (groupId > 0) {
                resp.setJson(messageService_.getGroupHistory(groupId, 50, before).dump());
            } else if (peerId > 0) {
                resp.setJson(messageService_.getPrivateHistory(userId, peerId, 50, before).dump());
            } else {
                resp = HttpResponse::badRequest("peerId or groupId required");
            }
        });
    }

    // ==================== WebSocket 实时消息 ====================
    void setupWebSocket() {
        wsServer_.setThreadNum(4);

        // 握手验证: token 在 URL query 中 ?token=xxx
        wsServer_.setHandshakeValidator([this](const TcpConnectionPtr&, const std::string& path,
                                               const std::map<std::string, std::string>&) -> bool {
            // 从 path 中提取 token: /ws?token=xxx
            auto pos = path.find("token=");
            if (pos == std::string::npos) return false;
            std::string token = path.substr(pos + 6);
            auto amp = token.find('&');
            if (amp != std::string::npos) token = token.substr(0, amp);
            return userService_.verifyToken(token) > 0;
        });

        // 连接建立
        wsServer_.setConnectionHandler([this](const WsSessionPtr& session) {
            // 从 path context 中再次解析 token 获取 userId
            std::string path = session->getContext("path", "");
            int64_t userId = extractUserIdFromPath(path);
            if (userId > 0) {
                session->setContext("userId", std::to_string(userId));
                onlineManager_.addUser(userId, session);
                // 通知好友上线
                notifyFriendsStatus(userId, true);
            }
        });

        // 消息处理
        wsServer_.setMessageHandler([this](const WsSessionPtr& session, const WsMessage& msg) {
            if (!msg.isText()) return;
            std::string text = msg.text();
            handleWsMessage(session, text);
        });

        // 断开连接
        wsServer_.setCloseHandler([this](const WsSessionPtr& session) {
            std::string userIdStr = session->getContext("userId", "");
            if (!userIdStr.empty()) {
                int64_t userId = std::stoll(userIdStr);
                onlineManager_.removeUser(userId);
                notifyFriendsStatus(userId, false);
            }
        });
    }

    void handleWsMessage(const WsSessionPtr& session, const std::string& text) {
        auto msg = json::parse(text, nullptr, false);
        if (msg.is_discarded()) {
            session->sendText(Protocol::makeError("invalid json"));
            return;
        }

        std::string type = msg.value("type", "");
        int64_t fromId = std::stoll(session->getContext("userId", "0"));
        if (fromId <= 0) {
            session->sendText(Protocol::makeError("not authenticated"));
            return;
        }

        if (type == Protocol::MSG) {
            handlePrivateMsg(fromId, msg);
        } else if (type == Protocol::GROUP_MSG) {
            handleGroupMsg(fromId, msg);
        }
    }

    void handlePrivateMsg(int64_t fromId, const json& msg) {
        int64_t toId = std::stoll(msg.value("to", "0"));
        std::string content = msg.value("content", "");
        std::string msgId = msg.value("msgId", Protocol::generateMsgId());
        int64_t ts = Protocol::nowMs();

        // 存储消息
        messageService_.savePrivateMessage(msgId, fromId, toId, content, ts);

        // ACK 给发送者
        auto fromSession = onlineManager_.getSession(fromId);
        if (fromSession) fromSession->sendText(Protocol::makeAck(msgId));

        // 转发给接收者
        auto toSession = onlineManager_.getSession(toId);
        if (toSession) {
            toSession->sendText(Protocol::makePrivateMsg(fromId, toId, content, msgId));
        }
    }

    void handleGroupMsg(int64_t fromId, const json& msg) {
        int64_t groupId = std::stoll(msg.value("to", "0"));
        std::string content = msg.value("content", "");
        std::string msgId = msg.value("msgId", Protocol::generateMsgId());
        int64_t ts = Protocol::nowMs();

        // 存储
        messageService_.saveGroupMessage(msgId, groupId, fromId, content, ts);

        // ACK
        auto fromSession = onlineManager_.getSession(fromId);
        if (fromSession) fromSession->sendText(Protocol::makeAck(msgId));

        // 转发给群内所有在线成员
        auto memberIds = groupService_.getMemberIds(groupId);
        std::string payload = Protocol::makeGroupMsg(fromId, groupId, content, msgId);
        for (int64_t memberId : memberIds) {
            if (memberId == fromId) continue;
            auto s = onlineManager_.getSession(memberId);
            if (s) s->sendText(payload);
        }
    }

    void notifyFriendsStatus(int64_t userId, bool online) {
        auto friends = friendService_.getFriends(userId);
        std::string payload = online ? Protocol::makeOnline(userId) : Protocol::makeOffline(userId);
        for (auto& f : friends) {
            int64_t fid = f["userId"].get<int64_t>();
            auto s = onlineManager_.getSession(fid);
            if (s) s->sendText(payload);
        }
    }

    int64_t authFromRequest(const HttpRequest& req) {
        std::string auth = req.getHeader("authorization");
        if (auth.find("Bearer ") == 0) {
            return userService_.verifyToken(auth.substr(7));
        }
        return -1;
    }

    int64_t extractUserIdFromPath(const std::string& path) {
        auto pos = path.find("token=");
        if (pos == std::string::npos) return -1;
        std::string token = path.substr(pos + 6);
        auto amp = token.find('&');
        if (amp != std::string::npos) token = token.substr(0, amp);
        return userService_.verifyToken(token);
    }

    EventLoop* loop_;
    HttpServer httpServer_;
    WebSocketServer wsServer_;

    std::shared_ptr<MySQLPool> db_;
    std::shared_ptr<RedisPool> redis_;

    UserService userService_;
    FriendService friendService_;
    GroupService groupService_;
    MessageService messageService_;
    OnlineManager onlineManager_;
};
```

### main.cpp (完整版)

```cpp
#include "server/ChatServer.h"
#include "net/EventLoop.h"
#include <iostream>
#include <signal.h>

EventLoop* g_loop = nullptr;

void signalHandler(int) {
    if (g_loop) g_loop->quit();
}

int main() {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // MySQL 配置
    MySQLPoolConfig mysqlConfig;
    mysqlConfig.host = "127.0.0.1";
    mysqlConfig.port = 3306;
    mysqlConfig.user = "root";
    mysqlConfig.password = "";
    mysqlConfig.database = "muduo_im";
    mysqlConfig.minSize = 5;
    mysqlConfig.maxSize = 20;

    // Redis 配置
    RedisPoolConfig redisConfig;
    redisConfig.host = "127.0.0.1";
    redisConfig.port = 6379;
    redisConfig.minSize = 3;
    redisConfig.maxSize = 10;

    EventLoop loop;
    g_loop = &loop;

    ChatServer server(&loop, 8080, 9090, mysqlConfig, redisConfig, "muduo-im-jwt-secret-key");
    server.start();

    std::cout << "=== muduo-im ===" << std::endl;
    std::cout << "HTTP API: http://localhost:8080" << std::endl;
    std::cout << "WebSocket: ws://localhost:9090/ws?token=xxx" << std::endl;

    loop.loop();

    std::cout << "Server stopped." << std::endl;
    return 0;
}
```

### 编译验证

```bash
cd build && cmake .. && make -j$(nproc)
```

### Commit

```bash
git add src/server/ChatServer.h src/server/MessageService.h src/server/main.cpp
git commit -m "feat: add ChatServer with HTTP API + WebSocket messaging"
```

---

## Task 7: 最终编译验证 + 文档更新

### Step 1: 全量编译

```bash
cd /home/ly/workspaces/im/muduo-im/build && cmake .. && make -j$(nproc)
```

### Step 2: 更新 workspace CLAUDE.md

将阶段 3 标记为完成。

### Step 3: Commit

```bash
git add -A
git commit -m "docs: mark Phase 3 as completed"
```

---

## 总结

| Task | 文件 | 描述 |
|------|------|------|
| 1 | CMakeLists.txt, sql/init.sql, main.cpp | 项目脚手架 |
| 2 | JWT.h, Protocol.h | 认证 + 协议 |
| 3 | UserService.h | 注册/登录 |
| 4 | OnlineManager.h, FriendService.h, GroupService.h | 在线管理 + 好友 + 群组 |
| 5 | MessageService.h | 消息存储/历史 |
| 6 | ChatServer.h, main.cpp | 主服务组装 |
| 7 | 编译验证 + 文档 | 最终验证 |

**外部依赖:** MySQL server + Redis server（运行时需要）
**编译依赖:** libmysqlclient-dev, libhiredis-dev, OpenSSL, zlib, nlohmann/json（已安装）
