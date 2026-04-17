/**
 * @file AuditService.h
 * @brief 审计日志服务 —— 记录登录、注册、改密、注销、群踢人等敏感操作
 *
 * 所有记录落 MySQL `audit_log` 表，不阻塞主流程：若连接池获取失败或
 * SQL 执行失败，静默丢弃该条日志（不抛异常、不返回错误）。
 * 字段设计：
 * - user_id：操作者（注册前可为空）
 * - action：操作类型（login / login_failed / register / change_password / delete_account / kick_member ...）
 * - target：操作目标（如被踢用户 ID、群 ID）
 * - ip：客户端 IP（优先从 X-Real-IP / X-Forwarded-For 头取）
 * - detail：自由格式补充信息
 */
#pragma once

#include "pool/MySQLPool.h"
#include <memory>
#include <string>

/**
 * @class AuditService
 * @brief 敏感操作审计日志写入器
 *
 * 持有 MySQLPool 共享指针，按需获取连接写入。写入失败时静默丢弃，
 * 不影响业务主流程。
 */
class AuditService {
public:
    /**
     * @brief 构造审计服务
     * @param db MySQL 连接池共享指针
     */
    explicit AuditService(std::shared_ptr<MySQLPool> db) : db_(std::move(db)) {}

    /**
     * @brief 记录一条审计日志（失败静默）
     *
     * @param userId 操作者用户 ID（<=0 时写入 NULL，适用于注册前、失败登录等场景）
     * @param action 操作类型（如 "login" / "register" / "delete_account"）
     * @param target 操作目标（可选，如被踢用户 ID / 群 ID）
     * @param ip     客户端 IP（可选）
     * @param detail 补充说明（可选）
     */
    void log(int64_t userId, const std::string& action, const std::string& target = "",
             const std::string& ip = "", const std::string& detail = "") {
        if (!db_) return;
        auto conn = db_->acquire(1000);
        if (!conn || !conn->valid()) return;  // 静默失败

        std::string sql = "INSERT INTO audit_log (user_id, action, target, ip, detail) VALUES ("
            + (userId > 0 ? std::to_string(userId) : std::string("NULL"))
            + ",'" + conn->escape(action) + "'"
            + ",'" + conn->escape(target) + "'"
            + ",'" + conn->escape(ip) + "'"
            + ",'" + conn->escape(detail) + "')";
        conn->execute(sql);
        db_->release(std::move(conn));
    }

private:
    std::shared_ptr<MySQLPool> db_;
};
