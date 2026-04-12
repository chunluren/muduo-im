/**
 * @file Protocol.h
 * @brief IM 消息协议定义，定义所有 WebSocket 消息类型和构造函数
 *
 * 本文件是 IM 系统的协议层核心，包含：
 * - 消息类型常量：定义客户端与服务端之间所有 WebSocket JSON 消息的 type 字段取值
 * - 消息构造函数：将业务参数封装为标准 JSON 字符串，供 WebSocket 发送
 * - 工具函数：UUID 生成、时间戳获取、密码哈希与验证
 *
 * 所有消息均采用 JSON 格式，通过 WebSocket 文本帧传输。
 * 每条消息必须包含 "type" 字段以标识消息类型。
 */
#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>
#include <openssl/evp.h>

using json = nlohmann::json;

/**
 * @namespace Protocol
 * @brief IM 消息协议命名空间
 *
 * 包含三类内容：
 * 1. 消息类型常量 — 定义 WebSocket JSON 消息中 "type" 字段的所有合法取值
 * 2. 消息构造函数 — 将业务参数组装为标准化的 JSON 字符串
 * 3. 工具函数 — UUID v4 生成、毫秒时间戳、SHA-256 哈希、密码存储与验证
 */
namespace Protocol {

/** @name 消息类型常量
 *  定义所有 WebSocket JSON 消息的 type 字段取值。
 *  方向说明：C = 客户端，S = 服务端
 *  @{
 */
inline const char* MSG         = "msg";        ///< 私聊消息（C->S 发送 / S->C 推送）
inline const char* GROUP_MSG   = "group_msg";  ///< 群聊消息（C->S 发送 / S->C 推送）
inline const char* ACK         = "ack";        ///< 消息送达确认（S->C，告知客户端消息已被服务端接收）
inline const char* ONLINE      = "online";     ///< 用户上线通知（S->C，通知好友某用户已上线）
inline const char* OFFLINE     = "offline";    ///< 用户离线通知（S->C，通知好友某用户已下线）
inline const char* ERROR_MSG   = "error";      ///< 错误消息（S->C，通知客户端操作失败及原因）
inline const char* FILE_MSG    = "file_msg";   ///< 文件消息（C->S 发送 / S->C 推送，包含文件 URL 和元信息）
inline const char* RECALL      = "recall";     ///< 消息撤回通知（C->S 请求撤回 / S->C 通知撤回）
inline const char* READ_ACK    = "read_ack";   ///< 已读回执（C->S 发送，标记消息已读位置）
inline const char* TYPING      = "typing";     ///< 正在输入提示（C->S 发送 / S->C 转发）
inline const char* UNREAD_SYNC = "unread_sync"; ///< 未读消息同步（S->C，用户上线时推送各好友未读计数）
/** @} */

/**
 * @brief 生成 UUID v4 格式的消息唯一标识
 *
 * 使用 std::random_device + mt19937 生成 128 位随机数，然后按照 RFC 4122 设置：
 * - version 位（第 13 位 nibble）设为 4，表示随机生成的 UUID
 * - variant 位（第 17 位的高 2 bit）设为 10，表示 RFC 4122 变体
 *
 * 输出格式：xxxxxxxx-xxxx-4xxx-{8|9|a|b}xxx-xxxxxxxxxxxx（36 字符含连字符）
 *
 * @return UUID v4 字符串，用作消息的唯一标识 (msgId)
 */
inline std::string generateMsgId() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<uint32_t> dist(0, 0xFFFFFFFF);

    uint32_t a = dist(gen), b = dist(gen), c = dist(gen), d = dist(gen);
    // 设置 version 4 和 variant bits
    b = (b & 0xFFFF0FFF) | 0x00004000;
    c = (c & 0x3FFFFFFF) | 0x80000000;

    char buf[37];
    snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%04x%08x",
             a, (b >> 16) & 0xFFFF, b & 0xFFFF,
             (c >> 16) & 0xFFFF, c & 0xFFFF, d);
    return buf;
}

/**
 * @brief 获取当前时间戳（毫秒级）
 *
 * 基于 system_clock 获取自 Unix Epoch (1970-01-01 00:00:00 UTC) 以来的毫秒数。
 * 用于消息的 timestamp 字段，实现消息排序和时间展示。
 *
 * @return 当前时间的毫秒级 Unix 时间戳
 */
inline int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

/**
 * @brief 构造私聊消息 JSON（服务端 -> 客户端推送）
 *
 * 返回的 JSON 格式：
 * @code
 * {
 *   "type": "msg",
 *   "from": "发送者userId",
 *   "to": "接收者userId",
 *   "content": "消息内容",
 *   "msgId": "消息唯一标识",
 *   "timestamp": 毫秒时间戳
 * }
 * @endcode
 *
 * @param from 发送者的用户 ID
 * @param to 接收者的用户 ID
 * @param content 消息文本内容
 * @param msgId 消息唯一标识（由 generateMsgId() 生成）
 * @return JSON 序列化后的字符串
 */
inline std::string makePrivateMsg(int64_t from, int64_t to, const std::string& content, const std::string& msgId) {
    json j;
    j["type"] = MSG;
    j["from"] = std::to_string(from);
    j["to"] = std::to_string(to);
    j["content"] = content;
    j["msgId"] = msgId;
    j["timestamp"] = nowMs();
    return j.dump();
}

/**
 * @brief 构造群聊消息 JSON（服务端 -> 客户端推送）
 *
 * 返回的 JSON 格式：
 * @code
 * {
 *   "type": "group_msg",
 *   "from": "发送者userId",
 *   "to": "群组groupId",
 *   "content": "消息内容",
 *   "msgId": "消息唯一标识",
 *   "timestamp": 毫秒时间戳
 * }
 * @endcode
 *
 * @param from 发送者的用户 ID
 * @param groupId 目标群组 ID
 * @param content 消息文本内容
 * @param msgId 消息唯一标识
 * @return JSON 序列化后的字符串
 */
inline std::string makeGroupMsg(int64_t from, int64_t groupId, const std::string& content, const std::string& msgId) {
    json j;
    j["type"] = GROUP_MSG;
    j["from"] = std::to_string(from);
    j["to"] = std::to_string(groupId);
    j["content"] = content;
    j["msgId"] = msgId;
    j["timestamp"] = nowMs();
    return j.dump();
}

/**
 * @brief 构造消息送达确认 ACK（服务端 -> 客户端）
 *
 * 服务端收到客户端的消息后，返回 ACK 告知消息已被接收处理。
 * JSON 格式：{"type": "ack", "msgId": "对应的消息ID"}
 *
 * @param msgId 被确认的消息 ID
 * @return JSON 序列化后的字符串
 */
inline std::string makeAck(const std::string& msgId) {
    return json({{"type", ACK}, {"msgId", msgId}}).dump();
}

/**
 * @brief 构造用户上线通知（服务端 -> 客户端）
 *
 * 当用户上线时，服务端向其好友推送此通知。
 * JSON 格式：{"type": "online", "userId": "上线用户ID"}
 *
 * @param userId 上线用户的 ID
 * @return JSON 序列化后的字符串
 */
inline std::string makeOnline(int64_t userId) {
    return json({{"type", ONLINE}, {"userId", std::to_string(userId)}}).dump();
}

/**
 * @brief 构造用户离线通知（服务端 -> 客户端）
 *
 * 当用户离线时，服务端向其好友推送此通知。
 * JSON 格式：{"type": "offline", "userId": "离线用户ID"}
 *
 * @param userId 离线用户的 ID
 * @return JSON 序列化后的字符串
 */
inline std::string makeOffline(int64_t userId) {
    return json({{"type", OFFLINE}, {"userId", std::to_string(userId)}}).dump();
}

/**
 * @brief 构造错误消息（服务端 -> 客户端）
 *
 * 当服务端处理请求失败时，向客户端返回错误说明。
 * JSON 格式：{"type": "error", "message": "错误描述信息"}
 *
 * @param message 错误描述信息
 * @return JSON 序列化后的字符串
 */
inline std::string makeError(const std::string& message) {
    return json({{"type", ERROR_MSG}, {"message", message}}).dump();
}

/**
 * @brief 构造文件消息 JSON（服务端 -> 客户端推送）
 *
 * 返回的 JSON 格式：
 * @code
 * {
 *   "type": "file_msg",
 *   "from": "发送者userId",
 *   "to": "接收者userId",
 *   "url": "文件下载URL",
 *   "filename": "原始文件名",
 *   "fileSize": 文件大小(字节),
 *   "msgId": "消息唯一标识",
 *   "timestamp": 毫秒时间戳
 * }
 * @endcode
 *
 * @param from 发送者的用户 ID
 * @param to 接收者的用户 ID
 * @param url 文件的下载 URL（文件已上传到存储服务）
 * @param filename 原始文件名
 * @param fileSize 文件大小（字节）
 * @param msgId 消息唯一标识
 * @return JSON 序列化后的字符串
 */
inline std::string makeFileMsg(int64_t from, int64_t to, const std::string& url,
                                const std::string& filename, int64_t fileSize, const std::string& msgId) {
    json j;
    j["type"] = FILE_MSG;
    j["from"] = std::to_string(from);
    j["to"] = std::to_string(to);
    j["url"] = url;
    j["filename"] = filename;
    j["fileSize"] = fileSize;
    j["msgId"] = msgId;
    j["timestamp"] = nowMs();
    return j.dump();
}

/**
 * @brief 构造消息撤回通知（服务端 -> 客户端）
 *
 * 通知客户端某条消息已被撤回，客户端应将该消息替换为"已撤回"提示。
 * JSON 格式：{"type": "recall", "msgId": "被撤回的消息ID", "from": "撤回操作者userId"}
 *
 * @param msgId 被撤回的消息 ID
 * @param fromUserId 执行撤回操作的用户 ID
 * @return JSON 序列化后的字符串
 */
inline std::string makeRecall(const std::string& msgId, int64_t fromUserId) {
    return json({{"type", RECALL}, {"msgId", msgId}, {"from", std::to_string(fromUserId)}}).dump();
}

/**
 * @brief 构造已读回执（客户端 -> 服务端）
 *
 * 客户端告知服务端：fromUserId 已读了 toUserId 发来的消息，已读位置到 lastMsgId。
 * JSON 格式：{"type": "read_ack", "from": "阅读者userId", "to": "消息发送者userId", "lastMsgId": "最后已读消息ID"}
 *
 * @param fromUserId 发送已读回执的用户 ID（即阅读者）
 * @param toUserId 消息的原始发送者 ID
 * @param lastMsgId 已读的最后一条消息 ID（该 ID 及之前的消息均标记为已读）
 * @return JSON 序列化后的字符串
 */
inline std::string makeReadAck(int64_t fromUserId, int64_t toUserId, const std::string& lastMsgId) {
    return json({{"type", READ_ACK}, {"from", std::to_string(fromUserId)},
                 {"to", std::to_string(toUserId)}, {"lastMsgId", lastMsgId}}).dump();
}

/// 构造正在输入通知
inline std::string makeTyping(int64_t fromUserId, int64_t toUserId) {
    return json({{"type", TYPING}, {"from", std::to_string(fromUserId)}, {"to", std::to_string(toUserId)}}).dump();
}

/**
 * @brief 计算 SHA-256 哈希值
 *
 * 使用 OpenSSL EVP (Envelope) 高层接口计算 SHA-256 摘要。
 * EVP 接口是 OpenSSL 推荐的方式，相比直接调用 SHA256() 更灵活，支持运行时切换算法。
 *
 * 计算流程：EVP_DigestInit_ex -> EVP_DigestUpdate -> EVP_DigestFinal_ex
 * 输出为 64 字符的十六进制小写字符串（32 字节哈希值的 hex 表示）。
 *
 * @param input 待哈希的原始字符串
 * @return 64 字符的十六进制 SHA-256 哈希值
 */
inline std::string sha256(const std::string& input) {
    unsigned char hash[32];
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, input.c_str(), input.size());
    EVP_DigestFinal_ex(ctx, hash, nullptr);
    EVP_MD_CTX_free(ctx);

    std::ostringstream oss;
    for (int i = 0; i < 32; ++i)
        oss << std::hex << std::setfill('0') << std::setw(2) << (int)hash[i];
    return oss.str();
}

/**
 * @brief 密码哈希（固定 salt + SHA-256）
 *
 * 对明文密码进行哈希处理后再存入数据库，防止数据库泄露时明文密码暴露。
 * 当前策略：固定 salt "muduo-im-salt-v1" 拼接在密码前面，再做 SHA-256。
 *
 * @note 安全说明：固定 salt + 单次 SHA-256 的安全性较弱，容易受彩虹表攻击。
 *       生产环境应使用 bcrypt、scrypt 或 Argon2 等专用密码哈希算法，
 *       它们内置随机 salt 和自适应迭代次数，能有效抵抗暴力破解。
 *
 * @param password 用户输入的明文密码
 * @return 哈希后的密码字符串（64 字符 hex）
 */
inline std::string hashPassword(const std::string& password) {
    std::string salt = "muduo-im-salt-v1";  // 简化: 固定 salt
    return sha256(salt + password);
}

/**
 * @brief 验证密码是否匹配
 *
 * 将用户输入的明文密码经过相同的哈希流程（salt + SHA-256），
 * 与数据库中存储的哈希值进行比较。
 *
 * @param password 用户输入的明文密码
 * @param hashed 数据库中存储的密码哈希值
 * @return true 密码匹配；false 密码不匹配
 */
inline bool verifyPassword(const std::string& password, const std::string& hashed) {
    return hashPassword(password) == hashed;
}

}  // namespace Protocol
