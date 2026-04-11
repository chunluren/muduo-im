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
