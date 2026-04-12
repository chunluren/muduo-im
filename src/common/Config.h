/**
 * @file Config.h
 * @brief 简单 key=value 配置文件解析器
 *
 * 支持功能：
 * - key = value 格式的配置项
 * - # 开头的行注释（行内注释也支持）
 * - 自动去除 key/value 前后的空白字符
 * - 缺失 key 时返回默认值
 */
#pragma once

#include <string>
#include <fstream>
#include <unordered_map>
#include <algorithm>

/// 简单 key=value 配置文件解析
class AppConfig {
public:
    /**
     * @brief 加载配置文件
     * @param filepath 配置文件路径
     * @return true 加载成功；false 文件打开失败
     */
    bool load(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) return false;

        std::string line;
        while (std::getline(file, line)) {
            // 去掉注释和空行
            auto commentPos = line.find('#');
            if (commentPos != std::string::npos) line = line.substr(0, commentPos);

            // trim
            auto start = line.find_first_not_of(" \t\r\n");
            if (start == std::string::npos) continue;
            line = line.substr(start);
            auto end = line.find_last_not_of(" \t\r\n");
            line = line.substr(0, end + 1);

            auto eq = line.find('=');
            if (eq == std::string::npos) continue;

            std::string key = line.substr(0, eq);
            std::string value = line.substr(eq + 1);

            // trim key and value
            key.erase(key.find_last_not_of(" \t") + 1);
            key.erase(0, key.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            value.erase(0, value.find_first_not_of(" \t"));

            config_[key] = value;
        }
        return true;
    }

    /**
     * @brief 获取字符串配置项
     * @param key 配置键名
     * @param defaultVal 默认值（key 不存在时返回）
     * @return 配置值或默认值
     */
    std::string get(const std::string& key, const std::string& defaultVal = "") const {
        auto it = config_.find(key);
        return it != config_.end() ? it->second : defaultVal;
    }

    /**
     * @brief 获取整数配置项
     * @param key 配置键名
     * @param defaultVal 默认值（key 不存在或转换失败时返回）
     * @return 配置值或默认值
     */
    int getInt(const std::string& key, int defaultVal = 0) const {
        auto it = config_.find(key);
        if (it == config_.end()) return defaultVal;
        try { return std::stoi(it->second); } catch (...) { return defaultVal; }
    }

private:
    std::unordered_map<std::string, std::string> config_;
};
