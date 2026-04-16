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

        TransactionGuard tx(conn);
        if (!tx.active()) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "failed to start transaction"}};
        }

        int r1 = conn->execute("INSERT IGNORE INTO friends (user_id, friend_id) VALUES ("
            + std::to_string(userId) + "," + std::to_string(friendId) + ")");
        int r2 = conn->execute("INSERT IGNORE INTO friends (user_id, friend_id) VALUES ("
            + std::to_string(friendId) + "," + std::to_string(userId) + ")");

        bool success = (r1 >= 0 && r2 >= 0) && tx.commit();
        db_->release(std::move(conn));
        if (!success) return {{"success", false}, {"message", "add failed"}};
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

        TransactionGuard tx(conn);
        if (!tx.active()) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "failed to start transaction"}};
        }

        int r1 = conn->execute("DELETE FROM friends WHERE user_id=" + std::to_string(userId)
                      + " AND friend_id=" + std::to_string(friendId));
        int r2 = conn->execute("DELETE FROM friends WHERE user_id=" + std::to_string(friendId)
                      + " AND friend_id=" + std::to_string(userId));

        bool success = (r1 >= 0 && r2 >= 0) && tx.commit();
        db_->release(std::move(conn));
        if (!success) return {{"success", false}, {"message", "delete failed"}};
        return {{"success", true}};
    }

    /// 发送好友申请
    json sendRequest(int64_t fromUser, int64_t toUser) {
        if (fromUser == toUser) return {{"success", false}, {"message", "cannot add self"}};

        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {{"success", false}, {"message", "db error"}};

        // 检查是否已经是好友
        auto check = conn->query("SELECT 1 FROM friends WHERE user_id=" + std::to_string(fromUser)
            + " AND friend_id=" + std::to_string(toUser));
        if (check && mysql_num_rows(check.get()) > 0) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "already friends"}};
        }

        // 插入申请（IGNORE 避免重复）
        conn->execute("INSERT IGNORE INTO friend_requests (from_user, to_user, status) VALUES ("
            + std::to_string(fromUser) + "," + std::to_string(toUser) + ", 0)");
        db_->release(std::move(conn));

        return {{"success", true}, {"message", "request sent"}};
    }

    /// 获取收到的好友申请列表
    json getRequests(int64_t userId) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return json::array();

        auto result = conn->query(
            "SELECT fr.id, fr.from_user, u.username, u.nickname, u.avatar, fr.created_at "
            "FROM friend_requests fr JOIN users u ON fr.from_user = u.id "
            "WHERE fr.to_user=" + std::to_string(userId) + " AND fr.status=0 "
            "ORDER BY fr.created_at DESC");
        db_->release(std::move(conn));

        json requests = json::array();
        if (result) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(result.get()))) {
                requests.push_back({
                    {"requestId", std::stoll(row[0])},
                    {"fromUserId", std::stoll(row[1])},
                    {"username", row[2]},
                    {"nickname", row[3] ? row[3] : ""},
                    {"avatar", row[4] ? row[4] : ""},
                    {"createdAt", row[5] ? row[5] : ""}
                });
            }
        }
        return requests;
    }

    /// 处理好友申请（同意/拒绝）
    json handleRequest(int64_t userId, int64_t requestId, bool accept) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {{"success", false}, {"message", "db error"}};

        auto result = conn->query("SELECT from_user, to_user FROM friend_requests WHERE id="
            + std::to_string(requestId) + " AND to_user=" + std::to_string(userId) + " AND status=0");
        if (!result || mysql_num_rows(result.get()) == 0) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "request not found"}};
        }

        MYSQL_ROW row = mysql_fetch_row(result.get());
        int64_t fromUser = std::stoll(row[0]);
        int64_t toUser = std::stoll(row[1]);

        bool success = false;
        if (accept) {
            TransactionGuard tx(conn);
            if (!tx.active()) {
                db_->release(std::move(conn));
                return {{"success", false}, {"message", "failed to start transaction"}};
            }

            int r1 = conn->execute("UPDATE friend_requests SET status=1 WHERE id=" + std::to_string(requestId));
            int r2 = conn->execute("INSERT IGNORE INTO friends (user_id, friend_id) VALUES ("
                + std::to_string(fromUser) + "," + std::to_string(toUser) + ")");
            int r3 = conn->execute("INSERT IGNORE INTO friends (user_id, friend_id) VALUES ("
                + std::to_string(toUser) + "," + std::to_string(fromUser) + ")");

            if (r1 >= 0 && r2 >= 0 && r3 >= 0) {
                success = tx.commit();
            }
        } else {
            int r = conn->execute("UPDATE friend_requests SET status=2 WHERE id=" + std::to_string(requestId));
            success = (r > 0);
        }

        db_->release(std::move(conn));
        if (!success) return {{"success", false}, {"message", "operation failed"}};
        return {{"success", true}, {"accepted", accept}};
    }

private:
    std::shared_ptr<MySQLPool> db_;
};
