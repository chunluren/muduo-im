/**
 * @file Logging.h
 * @brief 结构化 JSON 日志辅助工具
 *
 * 基于 mymuduo-http AsyncLogger 的包装层，输出 JSON Lines 格式日志，
 * 便于 Loki / ELK / Splunk 等日志平台解析字段。
 *
 * 使用示例:
 * @code
 *   LOG_EVENT("server_start", "http=8080 ws=9090");
 *   LOG_WARN_JSON("db_slow_query", "sql=SELECT ... duration_ms=512");
 *   LOG_ERROR_JSON("jwt_verify_failed", "reason=expired");
 * @endcode
 *
 * 输出示例:
 *   {"ts":"2026-04-17T12:34:56Z","level":"info","event":"server_start","detail":"http=8080 ws=9090"}
 */

#pragma once

#include "asynclogger/AsyncLogger.h"

#include <chrono>
#include <ctime>
#include <sstream>
#include <string>

namespace LogJson {

/// 生成 ISO-8601 UTC 时间戳 (YYYY-MM-DDTHH:MM:SSZ)
inline std::string nowIso8601() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    struct tm tm_result;
    gmtime_r(&t, &tm_result);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_result);
    std::string s(buf);
    s.push_back('Z');
    return s;
}

/// 对 JSON 字符串值做最小化转义
inline std::string escapeJson(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 2);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char hex[8];
                    std::snprintf(hex, sizeof(hex), "\\u%04x",
                                  static_cast<unsigned int>(c));
                    out += hex;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

/// 构造 JSON 行并写入 AsyncLogger。file/line 来自调用宏处。
inline void logKV(LogLevel level, const char* srcFile, int srcLine,
                  const char* levelText,
                  const std::string& event,
                  const std::string& detail) {
    std::ostringstream oss;
    oss << "{\"ts\":\"" << nowIso8601() << "\","
        << "\"level\":\"" << levelText << "\","
        << "\"event\":\"" << escapeJson(event) << "\"";
    if (!detail.empty()) {
        oss << ",\"detail\":\"" << escapeJson(detail) << "\"";
    }
    oss << "}";
    // AsyncLogger::log 接受 printf 格式，为避免 detail 中的 % 被当作格式符，
    // 使用 "%s" 固定格式把整条 JSON 作为参数传入。
    AsyncLogger::instance().log(level, srcFile, srcLine, "%s", oss.str().c_str());
}

}  // namespace LogJson

// ==================== 结构化日志宏 ====================

/// INFO 级结构化事件日志
#define LOG_EVENT(event, detail) \
    ::LogJson::logKV(LogLevel::INFO,  __FILE__, __LINE__, "info",  (event), (detail))

/// WARN 级结构化事件日志
#define LOG_WARN_JSON(event, detail) \
    ::LogJson::logKV(LogLevel::WARN,  __FILE__, __LINE__, "warn",  (event), (detail))

/// ERROR 级结构化事件日志
#define LOG_ERROR_JSON(event, detail) \
    ::LogJson::logKV(LogLevel::ERROR, __FILE__, __LINE__, "error", (event), (detail))
