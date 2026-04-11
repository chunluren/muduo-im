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

    /// 从 token 获取 userId
    int64_t verifyToken(const std::string& token) {
        return jwt_.verify(token);
    }

    JWT& jwt() { return jwt_; }

private:
    std::shared_ptr<MySQLPool> db_;
    JWT jwt_;
};
