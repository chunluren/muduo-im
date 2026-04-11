/**
 * @file FriendService.h
 * @brief 好友关系服务，管理双向好友关系
 */

#pragma once

#include "pool/MySQLPool.h"
#include <nlohmann/json.hpp>
#include <memory>

using json = nlohmann::json;

/**
 * @class FriendService
 * @brief 好友关系管理服务
 *
 * 好友关系采用双向存储策略：当 A 添加 B 为好友时，在 friends 表中同时插入
 * 两条记录（A->B 和 B->A），确保双方都能在自己的好友列表中看到对方。
 * 删除好友时同样双向删除。
 */
class FriendService {
public:
    explicit FriendService(std::shared_ptr<MySQLPool> db) : db_(db) {}

    /**
     * @brief 获取用户的好友列表
     *
     * 通过 JOIN users 表获取好友的详细信息（id、username、nickname、avatar），
     * 而不仅仅是 friend_id。查询条件为 friends.user_id = userId。
     *
     * @param userId 当前用户 ID
     * @return json 数组，每个元素包含 userId、username、nickname、avatar
     */
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

    /**
     * @brief 添加好友（双向插入）
     *
     * 同时插入 userId->friendId 和 friendId->userId 两条记录，
     * 使用 INSERT IGNORE 防止重复添加（如果记录已存在则静默忽略，不报错）。
     * 不允许添加自己为好友。
     *
     * @param userId   发起添加的用户 ID
     * @param friendId 被添加的好友用户 ID
     * @return json 包含 success 字段；添加自己时返回错误信息
     */
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

    /**
     * @brief 删除好友（双向删除）
     *
     * 同时删除 userId->friendId 和 friendId->userId 两条记录，
     * 确保双方的好友列表中都移除对方。
     *
     * @param userId   发起删除的用户 ID
     * @param friendId 被删除的好友用户 ID
     * @return json 包含 success 字段
     */
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
