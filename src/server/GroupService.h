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
