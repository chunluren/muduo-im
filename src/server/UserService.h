/**
 * @file UserService.h
 * @brief 用户服务，处理注册、登录、Token 验证
 *
 * 本文件实现 IM 系统的用户认证全流程，包括：
 * - 用户注册：参数校验 -> 用户名查重 -> 密码哈希 -> 写入数据库
 * - 用户登录：查询用户 -> 验证密码 -> 生成 JWT Token
 * - Token 验证：解析并验证 JWT Token 的有效性
 *
 * 依赖组件：
 * - MySQLPool：数据库连接池，提供数据持久化能力
 * - JWT：Token 生成与验证
 * - Protocol：密码哈希与验证工具函数
 */
#pragma once

#include "common/JWT.h"
#include "common/Protocol.h"
#include "pool/MySQLPool.h"
#include <nlohmann/json.hpp>
#include <string>
#include <memory>

using json = nlohmann::json;

/**
 * @class UserService
 * @brief 用户认证服务，提供注册、登录和 Token 验证的完整流程
 *
 * 内部持有 MySQLPool 连接池（用于用户数据的 CRUD 操作）和 JWT 实例（用于 Token 管理）。
 * 所有数据库操作均使用 conn->escape() 对用户输入进行转义，防止 SQL 注入攻击。
 * 方法返回 nlohmann::json 对象，包含 success 字段指示操作结果。
 */
class UserService {
public:
    /**
     * @brief 构造用户服务实例
     * @param db MySQL 连接池的共享指针，用于数据库操作
     * @param jwtSecret JWT 签名密钥，传递给内部 JWT 实例
     */
    UserService(std::shared_ptr<MySQLPool> db, const std::string& jwtSecret)
        : db_(db), jwt_(jwtSecret) {}

    /**
     * @brief 用户注册
     *
     * 注册流程：
     * 1. 参数校验：用户名和密码不能为空，长度不超过 64 字符
     * 2. 从连接池获取数据库连接（超时 3000ms）
     * 3. 查重：通过 SELECT 查询检查用户名是否已存在
     * 4. 密码哈希：调用 Protocol::hashPassword() 对明文密码进行哈希
     * 5. 写入数据库：INSERT INTO users 表，nickname 为空时默认使用 username
     * 6. 返回新用户的 userId
     *
     * @note SQL 注入防护：所有用户输入（username、nickname）均通过 conn->escape() 转义后再拼入 SQL。
     *
     * @param username 用户名（唯一，最长 64 字符）
     * @param password 明文密码（最长 64 字符，存储时会进行哈希处理）
     * @param nickname 用户昵称（可选，为空时默认使用 username）
     * @return JSON 对象，成功时包含 {"success": true, "userId": ..., "message": "registered"}；
     *         失败时包含 {"success": false, "message": "错误原因"}
     */
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

    /**
     * @brief 用户登录
     *
     * 登录流程：
     * 1. 参数校验：用户名和密码不能为空
     * 2. 从连接池获取数据库连接（超时 3000ms）
     * 3. 查询用户：通过 username 查询 users 表，获取 id、password、nickname
     * 4. 验证密码：调用 Protocol::verifyPassword() 比对密码哈希
     * 5. 生成 Token：调用 JWT::generate() 生成包含 userId 的 JWT Token（默认 24h 有效期）
     * 6. 返回 token、userId 和 nickname
     *
     * @note SQL 注入防护：username 通过 conn->escape() 转义后再拼入 SQL。
     *
     * @param username 用户名
     * @param password 明文密码
     * @return JSON 对象，成功时包含 {"success": true, "token": "JWT", "userId": ..., "nickname": "..."}；
     *         失败时包含 {"success": false, "message": "错误原因"}
     */
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
        std::string nick = row[2] ? row[2] : "";

        if (!Protocol::verifyPassword(password, storedHash)) {
            return {{"success", false}, {"message", "wrong password"}};
        }

        std::string token = jwt_.generate(userId);
        return {
            {"success", true},
            {"token", token},
            {"userId", userId},
            {"nickname", nick}
        };
    }

    /**
     * @brief 验证 JWT Token 并提取用户 ID
     *
     * 委托给内部 JWT 实例的 verify() 方法，执行签名验证和过期检查。
     * 此方法通常在 WebSocket 连接建立时调用，用于鉴权。
     *
     * @param token 客户端提供的 JWT Token 字符串
     * @return 验证成功返回 userId (>0)；验证失败（签名错误、已过期、格式异常）返回 -1
     */
    int64_t verifyToken(const std::string& token) {
        return jwt_.verify(token);
    }

    /// 获取用户信息
    json getProfile(int64_t userId) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {{"success", false}, {"message", "db error"}};

        auto result = conn->query("SELECT id, username, nickname, avatar, created_at FROM users WHERE id=" + std::to_string(userId));
        db_->release(std::move(conn));

        if (!result || mysql_num_rows(result.get()) == 0) {
            return {{"success", false}, {"message", "user not found"}};
        }

        MYSQL_ROW row = mysql_fetch_row(result.get());
        return {
            {"success", true},
            {"userId", std::stoll(row[0])},
            {"username", row[1]},
            {"nickname", row[2] ? row[2] : ""},
            {"avatar", row[3] ? row[3] : ""},
            {"createdAt", row[4] ? row[4] : ""}
        };
    }

    /// 修改用户信息
    json updateProfile(int64_t userId, const std::string& nickname, const std::string& avatar) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {{"success", false}, {"message", "db error"}};

        std::string sql = "UPDATE users SET";
        bool hasField = false;
        if (!nickname.empty()) {
            sql += " nickname='" + conn->escape(nickname) + "'";
            hasField = true;
        }
        if (!avatar.empty()) {
            if (hasField) sql += ",";
            sql += " avatar='" + conn->escape(avatar) + "'";
            hasField = true;
        }
        if (!hasField) return {{"success", false}, {"message", "nothing to update"}};

        sql += " WHERE id=" + std::to_string(userId);
        conn->execute(sql);
        db_->release(std::move(conn));
        return {{"success", true}};
    }

    /// 搜索用户（按用户名模糊搜索）
    json searchUsers(const std::string& keyword) {
        if (keyword.empty() || keyword.size() < 2) {
            return {{"success", false}, {"message", "keyword too short (min 2 chars)"}};
        }
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {{"success", false}, {"message", "db error"}};

        std::string escaped = conn->escape(keyword);
        auto result = conn->query("SELECT id, username, nickname, avatar FROM users WHERE username LIKE '%" + escaped + "%' OR nickname LIKE '%" + escaped + "%' LIMIT 20");
        db_->release(std::move(conn));

        json users = json::array();
        if (result) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(result.get()))) {
                users.push_back({
                    {"userId", std::stoll(row[0])},
                    {"username", row[1]},
                    {"nickname", row[2] ? row[2] : ""},
                    {"avatar", row[3] ? row[3] : ""}
                });
            }
        }
        return {{"success", true}, {"users", users}};
    }

    /// 查看其他用户资料（公开信息）
    json getPublicProfile(int64_t targetUserId) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {{"success", false}, {"message", "db error"}};

        auto result = conn->query("SELECT id, username, nickname, avatar FROM users WHERE id=" + std::to_string(targetUserId));
        db_->release(std::move(conn));

        if (!result || mysql_num_rows(result.get()) == 0) {
            return {{"success", false}, {"message", "user not found"}};
        }

        MYSQL_ROW row = mysql_fetch_row(result.get());
        return {
            {"success", true},
            {"userId", std::stoll(row[0])},
            {"username", row[1]},
            {"nickname", row[2] ? row[2] : ""},
            {"avatar", row[3] ? row[3] : ""}
        };
    }

    /// 修改密码
    json changePassword(int64_t userId, const std::string& oldPassword, const std::string& newPassword) {
        if (newPassword.empty() || newPassword.size() < 6) {
            return {{"success", false}, {"message", "new password must be at least 6 characters"}};
        }

        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {{"success", false}, {"message", "db error"}};

        // Verify old password
        auto result = conn->query("SELECT password FROM users WHERE id=" + std::to_string(userId));
        if (!result || mysql_num_rows(result.get()) == 0) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "user not found"}};
        }

        MYSQL_ROW row = mysql_fetch_row(result.get());
        if (!Protocol::verifyPassword(oldPassword, row[0])) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "wrong old password"}};
        }

        std::string newHash = Protocol::hashPassword(newPassword);
        conn->execute("UPDATE users SET password='" + newHash + "' WHERE id=" + std::to_string(userId));
        db_->release(std::move(conn));
        return {{"success", true}};
    }

    /// 注销账号
    json deleteAccount(int64_t userId, const std::string& password) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {{"success", false}, {"message", "db error"}};

        // Verify password
        auto result = conn->query("SELECT password FROM users WHERE id=" + std::to_string(userId));
        if (!result || mysql_num_rows(result.get()) == 0) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "user not found"}};
        }

        MYSQL_ROW row = mysql_fetch_row(result.get());
        if (!Protocol::verifyPassword(password, row[0])) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "wrong password"}};
        }

        // 事务：级联删除用户相关数据（注：有外键 CASCADE 时可简化为单条 DELETE FROM users）
        TransactionGuard tx(conn);
        if (!tx.active()) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "failed to start transaction"}};
        }

        std::string uid = std::to_string(userId);
        int r1 = conn->execute("DELETE FROM friends WHERE user_id=" + uid + " OR friend_id=" + uid);
        int r2 = conn->execute("DELETE FROM group_members WHERE user_id=" + uid);
        int r3 = conn->execute("DELETE FROM friend_requests WHERE from_user=" + uid + " OR to_user=" + uid);
        int r4 = conn->execute("DELETE FROM users WHERE id=" + uid);

        bool success = (r1 >= 0 && r2 >= 0 && r3 >= 0 && r4 >= 0) && tx.commit();

        db_->release(std::move(conn));
        if (!success) return {{"success", false}, {"message", "delete failed"}};
        return {{"success", true}};
    }

    /** @brief 获取内部 JWT 实例的引用，供外部直接使用 JWT 功能 */
    JWT& jwt() { return jwt_; }

private:
    std::shared_ptr<MySQLPool> db_;  ///< MySQL 连接池，提供数据库连接的获取与释放
    JWT jwt_;                        ///< JWT 实例，负责 Token 的生成与验证
};
