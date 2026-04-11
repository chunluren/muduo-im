#pragma once

#include <string>
#include <nlohmann/json.hpp>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>
#include <openssl/evp.h>

using json = nlohmann::json;

namespace Protocol {

// 消息类型
inline const char* MSG         = "msg";
inline const char* GROUP_MSG   = "group_msg";
inline const char* ACK         = "ack";
inline const char* ONLINE      = "online";
inline const char* OFFLINE     = "offline";
inline const char* ERROR_MSG   = "error";
inline const char* FILE_MSG    = "file_msg";
inline const char* RECALL      = "recall";
inline const char* READ_ACK    = "read_ack";

/// 生成 UUID v4
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

/// 当前时间戳（毫秒）
inline int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

/// 构造私聊消息 (服务端->客户端)
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

/// 构造群聊消息 (服务端->客户端)
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

/// 构造 ACK
inline std::string makeAck(const std::string& msgId) {
    return json({{"type", ACK}, {"msgId", msgId}}).dump();
}

/// 构造在线通知
inline std::string makeOnline(int64_t userId) {
    return json({{"type", ONLINE}, {"userId", std::to_string(userId)}}).dump();
}

/// 构造离线通知
inline std::string makeOffline(int64_t userId) {
    return json({{"type", OFFLINE}, {"userId", std::to_string(userId)}}).dump();
}

/// 构造错误消息
inline std::string makeError(const std::string& message) {
    return json({{"type", ERROR_MSG}, {"message", message}}).dump();
}

/// 构造文件消息
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

/// 构造撤回通知
inline std::string makeRecall(const std::string& msgId, int64_t fromUserId) {
    return json({{"type", RECALL}, {"msgId", msgId}, {"from", std::to_string(fromUserId)}}).dump();
}

/// 构造已读回执
inline std::string makeReadAck(int64_t fromUserId, int64_t toUserId, const std::string& lastMsgId) {
    return json({{"type", READ_ACK}, {"from", std::to_string(fromUserId)},
                 {"to", std::to_string(toUserId)}, {"lastMsgId", lastMsgId}}).dump();
}

/// SHA256 哈希（密码存储用）
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

/// 密码哈希（salt + SHA256）
inline std::string hashPassword(const std::string& password) {
    std::string salt = "muduo-im-salt-v1";  // 简化: 固定 salt
    return sha256(salt + password);
}

/// 验证密码
inline bool verifyPassword(const std::string& password, const std::string& hashed) {
    return hashPassword(password) == hashed;
}

}  // namespace Protocol
