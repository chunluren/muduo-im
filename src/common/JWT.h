/**
 * @file JWT.h
 * @brief JWT (JSON Web Token) 认证模块，实现 HS256 签名的 Token 生成与验证
 *
 * 本模块提供基于 HMAC-SHA256 签名算法的 JWT 实现，用于 IM 系统的用户身份认证。
 * JWT 由三段 Base64URL 编码的字符串以 "." 连接而成：header.payload.signature。
 *
 * - header:  包含算法类型 (alg: HS256) 和令牌类型 (typ: JWT)
 * - payload: 包含用户标识 (userId)、签发时间 (iat)、过期时间 (exp)
 * - signature: 对 "header.payload" 使用 HMAC-SHA256 计算的签名，用于防篡改
 *
 * @note 安全说明：这是一个简化的 JWT 实现，使用对称密钥 (HS256) 签名。
 *       生产环境应考虑使用 RS256 等非对称签名算法，以避免密钥在多服务间共享带来的安全风险。
 *       此外，当前未实现 token 黑名单/撤销机制，登出后 token 仍在有效期内可用。
 */
#pragma once

#include <string>
#include <chrono>
#include <openssl/hmac.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

/**
 * @class JWT
 * @brief JWT 令牌生成与验证器
 *
 * 封装了 JWT 的完整生命周期管理：
 * - 生成：将用户信息编码为 JWT 三段结构（header.payload.signature），使用 HMAC-SHA256 签名
 * - 验证：拆分 token → 重新计算签名并比对 → 解析 payload → 检查是否过期
 *
 * 内部依赖 OpenSSL 的 HMAC 函数进行签名计算，使用自定义的 Base64URL 编解码。
 */
class JWT {
public:
    /**
     * @brief 构造 JWT 实例
     * @param secret HMAC-SHA256 签名所使用的密钥（对称密钥，需妥善保管）
     */
    explicit JWT(const std::string& secret) : secret_(secret) {}

    /**
     * @struct Claims
     * @brief JWT payload 解析后的完整 claims
     */
    struct Claims {
        int64_t userId = 0;   ///< 用户 ID
        std::string jti;       ///< JWT ID（UUID v4，用于黑名单吊销）
        int64_t iat = 0;       ///< 签发时间（unix 秒）
        int64_t exp = 0;       ///< 过期时间（unix 秒）

        /// 剩余有效时长（秒），若已过期返回 0
        int64_t remainingSeconds() const {
            int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            return exp > now ? (exp - now) : 0;
        }
    };

    /**
     * @brief 生成 JWT Token（向后兼容，jti 自动填空串）
     *
     * 根据用户 ID 和过期时间生成一个完整的 JWT 字符串。
     * Token 的 payload 部分包含以下字段：
     * - userId: 用户唯一标识
     * - iat (Issued At): 签发时间（Unix 时间戳，秒）
     * - exp (Expiration): 过期时间（Unix 时间戳，秒）
     *
     * @param userId 用户 ID，将编码到 token 的 payload 中
     * @param expireSeconds token 有效期（秒），默认 86400 秒（24 小时）
     * @return 格式为 "base64url(header).base64url(payload).base64url(signature)" 的 JWT 字符串
     */
    std::string generate(int64_t userId, int expireSeconds = 86400) {
        return generateWithJti(userId, "", expireSeconds);
    }

    /**
     * @brief 生成带 jti 的 JWT Token（支持主动吊销）
     *
     * payload 额外包含 jti 字段：
     * - jti (JWT ID)：UUID v4 格式的唯一标识，用于 Redis 黑名单精准吊销
     *
     * 调用方负责生成 jti（通常用 `Protocol::generateMsgId()`）并保存到持久层
     * （如"当前有效 token 表"），登出 / 改密 / 封号时把该 jti 写入 Redis 黑名单。
     *
     * @param userId 用户 ID
     * @param jti UUID v4 字符串；传空串则 payload 中不包含 jti 字段（向后兼容）
     * @param expireSeconds token 有效期（秒）
     * @return JWT 字符串
     */
    std::string generateWithJti(int64_t userId, const std::string& jti,
                                int expireSeconds = 86400) {
        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        json header = {{"alg", "HS256"}, {"typ", "JWT"}};
        json payload = {
            {"userId", userId},
            {"iat", now},
            {"exp", now + expireSeconds}
        };
        if (!jti.empty()) payload["jti"] = jti;

        std::string headerB64 = base64UrlEncode(header.dump());
        std::string payloadB64 = base64UrlEncode(payload.dump());
        std::string message = headerB64 + "." + payloadB64;
        std::string signature = base64UrlEncode(hmacSha256(message));

        return message + "." + signature;
    }

    /**
     * @brief 验证 JWT Token 并提取用户 ID
     *
     * 验证流程：
     * 1. 按 "." 拆分 token 为 header、payload、signature 三部分
     * 2. 对 "header.payload" 重新计算 HMAC-SHA256 签名，与 token 中的 signature 比对
     * 3. Base64URL 解码 payload，解析 JSON 获取字段
     * 4. 检查 exp 字段是否已过期（与当前系统时间比较）
     * 5. 全部通过后返回 payload 中的 userId
     *
     * @param token 待验证的 JWT 字符串
     * @return 验证成功返回 userId (>0)；验证失败（签名不匹配、已过期、格式错误）返回 -1
     */
    int64_t verify(const std::string& token) {
        Claims claims;
        return verifyAndParse(token, &claims) ? claims.userId : -1;
    }

    /**
     * @brief 验证 JWT 并解析出完整 Claims
     *
     * 相比 verify()：
     * - 多返回 jti（用于黑名单查询）
     * - 多返回 exp / iat（用于设置黑名单 TTL）
     *
     * @param token 待验证的 JWT
     * @param outClaims 输出参数，验证成功时填充
     * @return true 验证通过且未过期；false 任意失败原因
     */
    bool verifyAndParse(const std::string& token, Claims* outClaims) {
        if (!outClaims) return false;
        *outClaims = Claims{};

        auto dot1 = token.find('.');
        if (dot1 == std::string::npos) return false;
        auto dot2 = token.find('.', dot1 + 1);
        if (dot2 == std::string::npos) return false;

        std::string message = token.substr(0, dot2);
        std::string signature = token.substr(dot2 + 1);

        // 验证签名
        std::string expected = base64UrlEncode(hmacSha256(message));
        if (signature != expected) return false;

        std::string payloadStr = base64UrlDecode(
            token.substr(dot1 + 1, dot2 - dot1 - 1));
        try {
            json payload = json::parse(payloadStr);
            outClaims->userId = payload.value("userId", int64_t{0});
            outClaims->exp = payload.value("exp", int64_t{0});
            outClaims->iat = payload.value("iat", int64_t{0});
            outClaims->jti = payload.value("jti", std::string{});

            int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (outClaims->exp == 0 || now > outClaims->exp) return false;
            if (outClaims->userId <= 0) return false;

            return true;
        } catch (...) {
            return false;
        }
    }

private:
    /**
     * @brief 使用 HMAC-SHA256 计算消息认证码
     *
     * 调用 OpenSSL 的 HMAC() 函数，以成员变量 secret_ 作为密钥，
     * 对输入数据计算 HMAC-SHA256 签名。输出为 32 字节的原始二进制数据。
     *
     * @param data 待签名的数据（通常为 "base64url(header).base64url(payload)"）
     * @return 32 字节的 HMAC-SHA256 签名结果（原始二进制）
     */
    std::string hmacSha256(const std::string& data) {
        unsigned char result[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        HMAC(EVP_sha256(), secret_.c_str(), secret_.size(),
             reinterpret_cast<const unsigned char*>(data.c_str()), data.size(),
             result, &len);
        return std::string(reinterpret_cast<char*>(result), len);
    }

    /**
     * @brief Base64URL 编码
     *
     * 将二进制数据编码为 JWT 专用的 Base64URL 格式：
     * - 标准 Base64 中的 '+' 替换为 '-'
     * - 标准 Base64 中的 '/' 替换为 '_'
     * - 不添加 '=' 填充字符
     *
     * 这些替换确保编码结果可以安全地用于 URL 和 HTTP Header 中。
     *
     * @param input 待编码的原始数据
     * @return Base64URL 编码后的字符串（无 padding）
     */
    static std::string base64UrlEncode(const std::string& input) {
        static const char* chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        int i = 0;
        unsigned char arr3[3], arr4[4];
        int inLen = input.size();
        const unsigned char* bytesToEncode = reinterpret_cast<const unsigned char*>(input.c_str());

        while (inLen--) {
            arr3[i++] = *(bytesToEncode++);
            if (i == 3) {
                arr4[0] = (arr3[0] & 0xfc) >> 2;
                arr4[1] = ((arr3[0] & 0x03) << 4) + ((arr3[1] & 0xf0) >> 4);
                arr4[2] = ((arr3[1] & 0x0f) << 2) + ((arr3[2] & 0xc0) >> 6);
                arr4[3] = arr3[2] & 0x3f;
                for (i = 0; i < 4; i++) result += chars[arr4[i]];
                i = 0;
            }
        }
        if (i) {
            for (int j = i; j < 3; j++) arr3[j] = '\0';
            arr4[0] = (arr3[0] & 0xfc) >> 2;
            arr4[1] = ((arr3[0] & 0x03) << 4) + ((arr3[1] & 0xf0) >> 4);
            arr4[2] = ((arr3[1] & 0x0f) << 2) + ((arr3[2] & 0xc0) >> 6);
            for (int j = 0; j < i + 1; j++) result += chars[arr4[j]];
        }
        // URL-safe: + -> -, / -> _, no padding
        for (auto& c : result) {
            if (c == '+') c = '-';
            else if (c == '/') c = '_';
        }
        return result;
    }

    /**
     * @brief Base64URL 解码
     *
     * Base64URL 编码的反向操作：
     * 1. 将 '-' 还原为 '+'，'_' 还原为 '/'
     * 2. 补齐 '=' 填充使长度为 4 的倍数
     * 3. 按标准 Base64 算法解码为原始二进制数据
     *
     * @param input Base64URL 编码的字符串
     * @return 解码后的原始数据
     */
    static std::string base64UrlDecode(const std::string& input) {
        std::string s = input;
        for (auto& c : s) {
            if (c == '-') c = '+';
            else if (c == '_') c = '/';
        }
        while (s.size() % 4) s += '=';

        static const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string result;
        int i = 0;
        unsigned char arr4[4], arr3[3];

        for (char c : s) {
            if (c == '=') break;
            auto pos = chars.find(c);
            if (pos == std::string::npos) continue;
            arr4[i++] = pos;
            if (i == 4) {
                arr3[0] = (arr4[0] << 2) + ((arr4[1] & 0x30) >> 4);
                arr3[1] = ((arr4[1] & 0xf) << 4) + ((arr4[2] & 0x3c) >> 2);
                arr3[2] = ((arr4[2] & 0x3) << 6) + arr4[3];
                for (i = 0; i < 3; i++) result += arr3[i];
                i = 0;
            }
        }
        if (i) {
            for (int j = i; j < 4; j++) arr4[j] = 0;
            arr3[0] = (arr4[0] << 2) + ((arr4[1] & 0x30) >> 4);
            arr3[1] = ((arr4[1] & 0xf) << 4) + ((arr4[2] & 0x3c) >> 2);
            for (int j = 0; j < i - 1; j++) result += arr3[j];
        }
        return result;
    }

    std::string secret_;  ///< HMAC-SHA256 签名密钥（对称密钥）
};
