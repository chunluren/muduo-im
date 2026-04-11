#pragma once

#include "pool/MySQLPool.h"
#include <nlohmann/json.hpp>
#include <chrono>
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

    /// 撤回消息（只能撤回自己的、2分钟内的消息）
    bool recallMessage(const std::string& msgId, int64_t fromUserId) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return false;

        int64_t twoMinAgo = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() - 120000;

        // Try private messages first
        std::string sql = "DELETE FROM private_messages WHERE msg_id='" + conn->escape(msgId)
            + "' AND from_user=" + std::to_string(fromUserId)
            + " AND timestamp>" + std::to_string(twoMinAgo);
        int affected = conn->execute(sql);

        if (affected <= 0) {
            // Try group messages
            sql = "DELETE FROM group_messages WHERE msg_id='" + conn->escape(msgId)
                + "' AND from_user=" + std::to_string(fromUserId)
                + " AND timestamp>" + std::to_string(twoMinAgo);
            affected = conn->execute(sql);
        }

        db_->release(std::move(conn));
        return affected > 0;
    }

private:
    std::shared_ptr<MySQLPool> db_;
};
