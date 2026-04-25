/**
 * @file MessageService.h
 * @brief 消息服务，负责消息的持久化存储和历史查询
 *
 * 消息存储策略：
 * - 单聊消息采用写扩散：每条消息直接写入 private_messages 表，发送时即完成存储。
 * - 群聊消息采用读扩散：每条群消息只在 group_messages 表中存储一份，
 *   群成员查询历史时按 group_id 读取，避免为每个成员复制消息带来的存储膨胀。
 *
 * SQL 安全：所有用户输入的字符串参数均通过 conn->escape() 进行转义，防止 SQL 注入。
 */

#pragma once

#include "pool/MySQLPool.h"
#include <nlohmann/json.hpp>
#include <chrono>
#include <memory>
#include <string>

using json = nlohmann::json;

/**
 * @class MessageService
 * @brief 消息存储与查询服务
 *
 * 负责私聊消息和群聊消息的持久化、历史查询以及消息撤回。
 * - 单聊：写扩散，消息直接存入 private_messages 表。
 * - 群聊：读扩散，消息只在 group_messages 表中存一份，不为每个群成员各存一份。
 */
class MessageService {
public:
    explicit MessageService(std::shared_ptr<MySQLPool> db) : db_(db) {}

    /**
     * @brief 存储私聊消息
     *
     * 将一条单聊消息写入 private_messages 表（写扩散模式）。
     *
     * @param msgId     消息唯一 ID，用于去重（数据库主键/唯一索引）
     * @param from      发送方用户 ID
     * @param to        接收方用户 ID
     * @param content   消息正文内容（通过 conn->escape() 防 SQL 注入）
     * @param timestamp 消息时间戳，单位：毫秒（Unix epoch）
     * @return true 存储成功；false 数据库连接失败或插入失败（如 msgId 重复）
     */
    bool savePrivateMessage(const std::string& msgId, int64_t from, int64_t to,
                            const std::string& content, int64_t timestamp) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return false;

        // 使用 PreparedStatement 防 SQL 注入
        PreparedStatement stmt(conn, "INSERT INTO private_messages (msg_id, from_user, to_user, content, timestamp) VALUES (?, ?, ?, ?, ?)");
        if (!stmt.valid()) {
            db_->release(std::move(conn));
            return false;
        }
        stmt.bindString(1, msgId);
        stmt.bindInt64(2, from);
        stmt.bindInt64(3, to);
        stmt.bindString(4, content);
        stmt.bindInt64(5, timestamp);
        bool ok = stmt.execute() && stmt.affectedRows() > 0;
        db_->release(std::move(conn));
        return ok;
    }

    /**
     * @brief 存储群聊消息（读扩散——只存一份）
     *
     * 群聊消息只在 group_messages 表中写入一条记录，不会为每个群成员各复制一份。
     * 群成员查询历史时通过 group_id 读取这条唯一记录（读扩散）。
     *
     * @param msgId     消息唯一 ID，用于去重
     * @param groupId   目标群组 ID
     * @param from      发送方用户 ID
     * @param content   消息正文内容（通过 conn->escape() 防 SQL 注入）
     * @param timestamp 消息时间戳，单位：毫秒（Unix epoch）
     * @return true 存储成功；false 失败
     */
    bool saveGroupMessage(const std::string& msgId, int64_t groupId, int64_t from,
                          const std::string& content, int64_t timestamp,
                          const std::string& mentionsJson = "") {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return false;

        // 使用 PreparedStatement 防 SQL 注入
        // mentions 列：传空串则插 NULL，否则插传入的 JSON 字符串（如 "[12345,67890]"）
        const char* sql = "INSERT INTO group_messages (msg_id, group_id, from_user, content, timestamp, mentions) "
                          "VALUES (?, ?, ?, ?, ?, CASE WHEN ? = '' THEN NULL ELSE CAST(? AS JSON) END)";
        PreparedStatement stmt(conn, sql);
        if (!stmt.valid()) {
            db_->release(std::move(conn));
            return false;
        }
        stmt.bindString(1, msgId);
        stmt.bindInt64(2, groupId);
        stmt.bindInt64(3, from);
        stmt.bindString(4, content);
        stmt.bindInt64(5, timestamp);
        stmt.bindString(6, mentionsJson);  // 用于 CASE 判断
        stmt.bindString(7, mentionsJson);  // 用于 CAST AS JSON
        bool ok = stmt.execute() && stmt.affectedRows() > 0;
        db_->release(std::move(conn));
        return ok;
    }

    /**
     * @brief 查询私聊历史消息（游标分页）
     *
     * 分页逻辑：使用 before 参数作为游标，查询 timestamp < before 的消息，
     * 按 timestamp 倒序返回最多 limit 条。首次查询 before 传 0 表示从最新消息开始。
     *
     * 双向查询：同时查询 A->B 和 B->A 两个方向的消息
     * （WHERE (from=userId AND to=peerId) OR (from=peerId AND to=userId)）。
     *
     * @param userId 当前用户 ID
     * @param peerId 对方用户 ID
     * @param limit  每页返回消息数量上限，默认 50
     * @param before 游标分页：只返回 timestamp < before 的消息；0 表示不限制（从最新开始）
     * @return json 数组，每个元素包含 msgId、from、to、content、timestamp
     */
    json getPrivateHistory(int64_t userId, int64_t peerId, int limit = 50, int64_t before = 0) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return json::array();

        std::string sql = "SELECT msg_id, from_user, to_user, content, timestamp FROM private_messages "
            "WHERE ((from_user=" + std::to_string(userId) + " AND to_user=" + std::to_string(peerId) + ") "
            "OR (from_user=" + std::to_string(peerId) + " AND to_user=" + std::to_string(userId) + ")) AND recalled=0";
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

    /**
     * @brief 查询群聊历史消息
     *
     * 按 group_id + timestamp 倒序查询群消息，使用 before 游标分页。
     * 群消息表中每条消息只存一份（读扩散），所有群成员共享同一份历史记录。
     *
     * @param groupId 群组 ID
     * @param limit   每页返回消息数量上限，默认 50
     * @param before  游标分页：只返回 timestamp < before 的消息；0 表示不限制
     * @return json 数组，每个元素包含 msgId、from、content、timestamp
     */
    json getGroupHistory(int64_t groupId, int limit = 50, int64_t before = 0) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return json::array();

        std::string sql = "SELECT msg_id, from_user, content, timestamp FROM group_messages "
            "WHERE group_id=" + std::to_string(groupId) + " AND recalled=0";
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

    /**
     * @brief 撤回消息
     *
     * 撤回限制：
     * - 只能撤回自己发送的消息（通过 from_user 校验）
     * - 只能撤回 2 分钟（120000 毫秒）以内的消息
     *
     * 查找策略：先尝试从 private_messages（私聊）表中删除，
     * 如果未找到匹配记录，再尝试从 group_messages（群聊）表中删除。
     *
     * @param msgId      要撤回的消息 ID（通过 conn->escape() 防 SQL 注入）
     * @param fromUserId 请求撤回的用户 ID（必须是消息的发送者）
     * @return true 撤回成功；false 消息不存在、非本人发送或已超过 2 分钟
     */
    bool recallMessage(const std::string& msgId, int64_t fromUserId) {
        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return false;

        int64_t twoMinAgo = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count() - 120000;

        // Soft delete: mark as recalled instead of deleting
        std::string sql = "UPDATE private_messages SET recalled=1 WHERE msg_id='" + conn->escape(msgId)
            + "' AND from_user=" + std::to_string(fromUserId)
            + " AND timestamp>" + std::to_string(twoMinAgo)
            + " AND recalled=0";
        int affected = conn->execute(sql);

        if (affected <= 0) {
            sql = "UPDATE group_messages SET recalled=1 WHERE msg_id='" + conn->escape(msgId)
                + "' AND from_user=" + std::to_string(fromUserId)
                + " AND timestamp>" + std::to_string(twoMinAgo)
                + " AND recalled=0";
            affected = conn->execute(sql);
        }

        db_->release(std::move(conn));
        return affected > 0;
    }

    /**
     * @brief 搜索消息（按关键词）
     *
     * 在用户参与的私聊和群聊消息中搜索包含指定关键词的消息。
     * 关键词最少 2 个字符，防止过于宽泛的搜索导致性能问题。
     *
     * @param userId  当前用户 ID（只搜索该用户参与的消息）
     * @param keyword 搜索关键词（最少 2 字符）
     * @param limit   返回结果数量上限，默认 50
     * @return json 包含 success 和 messages 数组
     */
    json searchMessages(int64_t userId, const std::string& keyword, int limit = 50) {
        if (keyword.empty() || keyword.size() < 2) {
            return {{"success", false}, {"message", "keyword too short"}};
        }

        auto conn = db_->acquire(3000);
        if (!conn || !conn->valid()) return {{"success", false}, {"message", "db error"}};

        std::string escaped = conn->escape(keyword);

        // 搜索私聊消息
        std::string sql = "SELECT msg_id, from_user, to_user, content, timestamp, 'private' as type FROM private_messages "
            "WHERE (from_user=" + std::to_string(userId) + " OR to_user=" + std::to_string(userId) + ") "
            "AND content LIKE '%" + escaped + "%' AND recalled=0 "
            "ORDER BY timestamp DESC LIMIT " + std::to_string(limit);

        auto result = conn->query(sql);
        json messages = json::array();
        if (result) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(result.get()))) {
                messages.push_back({
                    {"msgId", row[0]}, {"from", std::stoll(row[1])}, {"to", std::stoll(row[2])},
                    {"content", row[3]}, {"timestamp", std::stoll(row[4])}, {"chatType", row[5]}
                });
            }
        }

        // 搜索群聊消息
        sql = "SELECT gm.msg_id, gm.from_user, gm.group_id, gm.content, gm.timestamp, 'group' as type "
            "FROM group_messages gm JOIN group_members mb ON gm.group_id = mb.group_id "
            "WHERE mb.user_id=" + std::to_string(userId) + " "
            "AND gm.content LIKE '%" + escaped + "%' AND gm.recalled=0 "
            "ORDER BY gm.timestamp DESC LIMIT " + std::to_string(limit);

        result = conn->query(sql);
        if (result) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(result.get()))) {
                messages.push_back({
                    {"msgId", row[0]}, {"from", std::stoll(row[1])}, {"groupId", std::stoll(row[2])},
                    {"content", row[3]}, {"timestamp", std::stoll(row[4])}, {"chatType", row[5]}
                });
            }
        }

        db_->release(std::move(conn));
        return {{"success", true}, {"messages", messages}};
    }

private:
    std::shared_ptr<MySQLPool> db_;
};
