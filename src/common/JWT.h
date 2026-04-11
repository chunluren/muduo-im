#pragma once

#include <string>
#include <chrono>
#include <openssl/hmac.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class JWT {
public:
    explicit JWT(const std::string& secret) : secret_(secret) {}

    /// 生成 token
    std::string generate(int64_t userId, int expireSeconds = 86400) {
        int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        json header = {{"alg", "HS256"}, {"typ", "JWT"}};
        json payload = {
            {"userId", userId},
            {"iat", now},
            {"exp", now + expireSeconds}
        };

        std::string headerB64 = base64UrlEncode(header.dump());
        std::string payloadB64 = base64UrlEncode(payload.dump());
        std::string message = headerB64 + "." + payloadB64;
        std::string signature = base64UrlEncode(hmacSha256(message));

        return message + "." + signature;
    }

    /// 验证 token，返回 userId，失败返回 -1
    int64_t verify(const std::string& token) {
        // 拆分三部分
        auto dot1 = token.find('.');
        if (dot1 == std::string::npos) return -1;
        auto dot2 = token.find('.', dot1 + 1);
        if (dot2 == std::string::npos) return -1;

        std::string message = token.substr(0, dot2);
        std::string signature = token.substr(dot2 + 1);

        // 验证签名
        std::string expected = base64UrlEncode(hmacSha256(message));
        if (signature != expected) return -1;

        // 解析 payload
        std::string payloadStr = base64UrlDecode(token.substr(dot1 + 1, dot2 - dot1 - 1));
        try {
            json payload = json::parse(payloadStr);
            int64_t exp = payload["exp"].get<int64_t>();
            int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (now > exp) return -1;  // 过期
            return payload["userId"].get<int64_t>();
        } catch (...) {
            return -1;
        }
    }

private:
    std::string hmacSha256(const std::string& data) {
        unsigned char result[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        HMAC(EVP_sha256(), secret_.c_str(), secret_.size(),
             reinterpret_cast<const unsigned char*>(data.c_str()), data.size(),
             result, &len);
        return std::string(reinterpret_cast<char*>(result), len);
    }

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

    std::string secret_;
};
