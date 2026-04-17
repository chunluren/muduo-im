/**
 * @file GroupService.h
 * @brief 群组服务，管理群组创建、加入、成员查询
 */

#pragma once

#include "pool/MySQLPool.h"
#include <nlohmann/json.hpp>
#include <memory>
#include <vector>

using json = nlohmann::json;

/**
 * @class GroupService
 * @brief 群组管理服务
 *
 * 群组模型：
 * - groups 表：存储群组基本信息，包括 owner_id（创建者）和 name（群名称）。
 * - group_members 表：存储群组成员关系（group_id, user_id）。
 *
 * 创建群组时，创建者自动成为群成员。
 */
class GroupService {
public:
    explicit GroupService(std::shared_ptr<MySQLPool> db) : db_(db) {}

    /**
     * @brief 查询用户所在的所有群组
     *
     * 通过 JOIN group_members 和 groups 表，查询指定用户加入的全部群组。
     * 返回每个群组的 id、name 和 owner_id。
     *
     * @param userId 用户 ID
     * @return json 数组，每个元素包含 groupId、name、ownerId
     */
    json getUserGroups(int64_t userId) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return json::array();

        std::string sql = "SELECT g.id, g.name, g.owner_id, g.announcement FROM group_members gm "
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
                    {"ownerId", std::stoll(row[2])},
                    {"announcement", row[3] ? row[3] : ""}
                });
            }
        }
        return groups;
    }

    /**
     * @brief 创建群组
     *
     * 创建流程：
     * 1. INSERT 一条记录到 groups 表，获取自增 groupId
     * 2. 将创建者自动加入 group_members 表（创建者即为首个群成员）
     *
     * @param ownerId 创建者用户 ID
     * @param name    群组名称（不能为空，通过 conn->escape() 防 SQL 注入）
     * @return json 包含 success 和 groupId 字段
     */
    json createGroup(int64_t ownerId, const std::string& name) {
        if (name.empty()) return {{"success", false}, {"message", "name required"}};

        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {{"success", false}, {"message", "db error"}};

        // 使用 PreparedStatement 防 SQL 注入
        PreparedStatement stmt(conn, "INSERT INTO `groups` (name, owner_id) VALUES (?, ?)");
        if (!stmt.valid()) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "prepare failed"}};
        }
        stmt.bindString(1, name);
        stmt.bindInt64(2, ownerId);
        if (!stmt.execute()) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "create failed"}};
        }
        int64_t groupId = stmt.lastInsertId();

        // 创建者自动加入群（只有 int64 参数，字符串拼接安全）
        conn->execute("INSERT INTO group_members (group_id, user_id) VALUES ("
            + std::to_string(groupId) + "," + std::to_string(ownerId) + ")");
        db_->release(std::move(conn));

        return {{"success", true}, {"groupId", groupId}};
    }

    /**
     * @brief 加入群组
     *
     * 使用 INSERT IGNORE 防止重复加入：如果用户已经是群成员，则静默忽略，不报错。
     *
     * @param userId  要加入群组的用户 ID
     * @param groupId 目标群组 ID
     * @return json 包含 success 字段
     */
    json joinGroup(int64_t userId, int64_t groupId) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {{"success", false}, {"message", "db error"}};

        conn->execute("INSERT IGNORE INTO group_members (group_id, user_id) VALUES ("
            + std::to_string(groupId) + "," + std::to_string(userId) + ")");
        db_->release(std::move(conn));

        return {{"success", true}};
    }

    /**
     * @brief 获取群组成员详细信息
     *
     * 通过 JOIN users 表获取每个群成员的详细信息（id、username、nickname），
     * 而不仅仅是 user_id。适用于需要展示成员列表的场景。
     *
     * @param groupId 群组 ID
     * @return json 数组，每个元素包含 userId、username、nickname
     */
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

    /**
     * @brief 获取群组成员 ID 列表（纯 ID，不含详细信息）
     *
     * 仅返回 user_id 列表，不 JOIN users 表，用于群消息广播等只需要
     * 知道"消息该发给谁"而不需要展示用户详细信息的场景，查询更轻量。
     *
     * @param groupId 群组 ID
     * @return 群成员 user_id 的 vector
     */
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

    /// 退出群组
    json leaveGroup(int64_t userId, int64_t groupId) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {{"success", false}, {"message", "db error"}};

        // 群主不能退出，只能解散
        auto result = conn->query("SELECT owner_id FROM `groups` WHERE id=" + std::to_string(groupId));
        if (result) {
            MYSQL_ROW row = mysql_fetch_row(result.get());
            if (row && std::stoll(row[0]) == userId) {
                db_->release(std::move(conn));
                return {{"success", false}, {"message", "owner cannot leave, use delete instead"}};
            }
        }

        conn->execute("DELETE FROM group_members WHERE group_id=" + std::to_string(groupId)
            + " AND user_id=" + std::to_string(userId));
        db_->release(std::move(conn));
        return {{"success", true}};
    }

    /// 解散群组（仅群主）
    json deleteGroup(int64_t userId, int64_t groupId) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {{"success", false}, {"message", "db error"}};

        auto result = conn->query("SELECT owner_id FROM `groups` WHERE id=" + std::to_string(groupId));
        if (!result || mysql_num_rows(result.get()) == 0) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "group not found"}};
        }
        MYSQL_ROW row = mysql_fetch_row(result.get());
        if (std::stoll(row[0]) != userId) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "only owner can delete group"}};
        }

        TransactionGuard tx(conn);
        if (!tx.active()) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "failed to start transaction"}};
        }

        int r1 = conn->execute("DELETE FROM group_members WHERE group_id=" + std::to_string(groupId));
        int r2 = conn->execute("DELETE FROM `groups` WHERE id=" + std::to_string(groupId));

        bool success = (r1 >= 0 && r2 >= 0) && tx.commit();
        db_->release(std::move(conn));
        if (!success) return {{"success", false}, {"message", "delete failed"}};
        return {{"success", true}};
    }

    /// 设置群公告（仅群主）
    json setAnnouncement(int64_t userId, int64_t groupId, const std::string& announcement) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {{"success", false}, {"message", "db error"}};

        // 验证群主
        auto result = conn->query("SELECT owner_id FROM `groups` WHERE id=" + std::to_string(groupId));
        if (!result || mysql_num_rows(result.get()) == 0) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "group not found"}};
        }
        MYSQL_ROW row = mysql_fetch_row(result.get());
        if (std::stoll(row[0]) != userId) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "only owner can set announcement"}};
        }

        // 使用 PreparedStatement 防 SQL 注入
        PreparedStatement stmt(conn, "UPDATE `groups` SET announcement=? WHERE id=?");
        stmt.bindString(1, announcement);
        stmt.bindInt64(2, groupId);
        stmt.execute();
        db_->release(std::move(conn));
        return {{"success", true}};
    }

    /// 获取群公告
    std::string getAnnouncement(int64_t groupId) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return "";

        auto result = conn->query("SELECT announcement FROM `groups` WHERE id=" + std::to_string(groupId));
        db_->release(std::move(conn));
        if (!result || mysql_num_rows(result.get()) == 0) return "";
        MYSQL_ROW row = mysql_fetch_row(result.get());
        return row[0] ? row[0] : "";
    }

    /// 踢出群成员（仅群主）
    json kickMember(int64_t operatorId, int64_t groupId, int64_t targetUserId) {
        if (operatorId == targetUserId) return {{"success", false}, {"message", "cannot kick yourself"}};

        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {{"success", false}, {"message", "db error"}};

        // 验证群主
        auto result = conn->query("SELECT owner_id FROM `groups` WHERE id=" + std::to_string(groupId));
        if (!result || mysql_num_rows(result.get()) == 0) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "group not found"}};
        }
        MYSQL_ROW row = mysql_fetch_row(result.get());
        if (std::stoll(row[0]) != operatorId) {
            db_->release(std::move(conn));
            return {{"success", false}, {"message", "only owner can kick members"}};
        }

        conn->execute("DELETE FROM group_members WHERE group_id=" + std::to_string(groupId)
            + " AND user_id=" + std::to_string(targetUserId));
        db_->release(std::move(conn));
        return {{"success", true}};
    }

private:
    std::shared_ptr<MySQLPool> db_;
};
