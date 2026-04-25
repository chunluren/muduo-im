/**
 * @file DeviceService.h
 * @brief 用户设备管理服务（Phase 3.3）
 *
 * 主要职责：
 * - 设备注册：登录或显式调用 /api/device/register 时记录到 user_devices
 * - 推送 token 维护：APNs (iOS) / FCM (Android) token，离线推送依赖
 * - 列设备 / 远程下线：用户管理后台
 *
 * 与 OnlineManager 的差异：
 * - OnlineManager: 内存 + Redis，**当前在线**的连接（session 维度）
 * - DeviceService: MySQL 持久化，**所有曾经登录过**的设备及推送 token（账号维度）
 */
#pragma once

#include "pool/MySQLPool.h"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>

class DeviceService {
public:
    explicit DeviceService(std::shared_ptr<MySQLPool> db) : db_(std::move(db)) {}

    /**
     * @brief 注册或更新设备
     *
     * 幂等：(uid, device_id) UNIQUE，已存在则 UPDATE 元数据 + last_active=now
     */
    bool registerDevice(int64_t uid, const std::string& deviceId,
                          const std::string& deviceType,
                          const std::string& osVersion = "",
                          const std::string& appVersion = "",
                          const std::string& apnsToken = "",
                          const std::string& fcmToken = "") {
        if (uid <= 0 || deviceId.empty() || deviceId.size() > 64) return false;
        auto conn = db_->acquire(2000);
        if (!conn || !conn->valid()) return false;

        PreparedStatement stmt(conn,
            "INSERT INTO user_devices (uid, device_id, device_type, os_version, "
            "app_version, apns_token, fcm_token) VALUES (?, ?, ?, ?, ?, ?, ?) "
            "ON DUPLICATE KEY UPDATE device_type=VALUES(device_type), "
            "os_version=VALUES(os_version), app_version=VALUES(app_version), "
            "apns_token=VALUES(apns_token), fcm_token=VALUES(fcm_token), "
            "last_active=NOW()");
        if (!stmt.valid()) { db_->release(std::move(conn)); return false; }
        stmt.bindInt64(1, uid);
        stmt.bindString(2, deviceId);
        stmt.bindString(3, deviceType.empty() ? "web" : deviceType);
        stmt.bindString(4, osVersion);
        stmt.bindString(5, appVersion);
        stmt.bindString(6, apnsToken);
        stmt.bindString(7, fcmToken);
        bool ok = stmt.execute();
        db_->release(std::move(conn));
        return ok;
    }

    /**
     * @brief 列出用户所有设备（按 last_active 降序）
     */
    nlohmann::json listDevices(int64_t uid) {
        nlohmann::json arr = nlohmann::json::array();
        auto conn = db_->acquire(2000);
        if (!conn || !conn->valid()) return arr;
        std::string sql =
            "SELECT id, device_id, device_type, os_version, app_version, "
            "apns_token IS NOT NULL AS has_apns, fcm_token IS NOT NULL AS has_fcm, "
            "UNIX_TIMESTAMP(created_at)*1000 AS created_at, "
            "UNIX_TIMESTAMP(last_active)*1000 AS last_active "
            "FROM user_devices WHERE uid=" + std::to_string(uid)
            + " ORDER BY last_active DESC";
        auto res = conn->query(sql);
        if (res) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res.get()))) {
                nlohmann::json d;
                d["id"] = row[0] ? std::stoll(row[0]) : 0;
                d["device_id"] = row[1] ? row[1] : "";
                d["device_type"] = row[2] ? row[2] : "";
                d["os_version"] = row[3] ? row[3] : "";
                d["app_version"] = row[4] ? row[4] : "";
                d["has_apns"] = row[5] && std::string(row[5]) == "1";
                d["has_fcm"] = row[6] && std::string(row[6]) == "1";
                d["created_at"] = row[7] ? std::stoll(row[7]) : 0;
                d["last_active"] = row[8] ? std::stoll(row[8]) : 0;
                arr.push_back(d);
            }
        }
        db_->release(std::move(conn));
        return arr;
    }

    /// 获取该用户所有设备的推送 token（用于 #17 APNs/FCM 离线推送）
    struct PushTarget {
        std::string deviceId;
        std::string deviceType;  // "ios" / "android" / "web"
        std::string apnsToken;
        std::string fcmToken;
    };
    std::vector<PushTarget> getPushTargets(int64_t uid) {
        std::vector<PushTarget> targets;
        auto conn = db_->acquire(2000);
        if (!conn || !conn->valid()) return targets;
        auto res = conn->query(
            "SELECT device_id, device_type, IFNULL(apns_token,''), IFNULL(fcm_token,'') "
            "FROM user_devices WHERE uid=" + std::to_string(uid)
            + " AND (apns_token IS NOT NULL OR fcm_token IS NOT NULL)");
        if (res) {
            MYSQL_ROW row;
            while ((row = mysql_fetch_row(res.get()))) {
                targets.push_back({row[0] ? row[0] : "", row[1] ? row[1] : "",
                                    row[2] ? row[2] : "", row[3] ? row[3] : ""});
            }
        }
        db_->release(std::move(conn));
        return targets;
    }

    /// 删除指定设备（用户主动下线某端）
    bool removeDevice(int64_t uid, const std::string& deviceId) {
        auto conn = db_->acquire(2000);
        if (!conn || !conn->valid()) return false;
        PreparedStatement stmt(conn,
            "DELETE FROM user_devices WHERE uid=? AND device_id=?");
        if (!stmt.valid()) { db_->release(std::move(conn)); return false; }
        stmt.bindInt64(1, uid);
        stmt.bindString(2, deviceId);
        bool ok = stmt.execute() && stmt.affectedRows() > 0;
        db_->release(std::move(conn));
        return ok;
    }

    /// 标记 push token 失效（APNs/FCM 推送返回 invalid_token 时调用）
    bool invalidatePushToken(int64_t uid, const std::string& deviceId,
                              bool clearApns, bool clearFcm) {
        if (!clearApns && !clearFcm) return true;
        auto conn = db_->acquire(2000);
        if (!conn || !conn->valid()) return false;
        std::string sets;
        if (clearApns) sets += "apns_token=NULL";
        if (clearFcm) {
            if (!sets.empty()) sets += ", ";
            sets += "fcm_token=NULL";
        }
        PreparedStatement stmt(conn,
            std::string("UPDATE user_devices SET ") + sets +
            " WHERE uid=? AND device_id=?");
        if (!stmt.valid()) { db_->release(std::move(conn)); return false; }
        stmt.bindInt64(1, uid);
        stmt.bindString(2, deviceId);
        bool ok = stmt.execute();
        db_->release(std::move(conn));
        return ok;
    }

private:
    std::shared_ptr<MySQLPool> db_;
};
