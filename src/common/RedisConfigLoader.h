/**
 * @file RedisConfigLoader.h
 * @brief 把 config.ini + 环境变量的 redis 配置（含 sentinel 模式）填到 RedisPoolConfig
 *
 * 共享给所有 redis 消费者（chat server / push_router / persister 等）使用，
 * 避免每个 main.cpp 重新实现一遍 sentinel 解析。
 *
 * 配置优先级：env > config.ini > 默认值。
 *
 * 直连模式（默认）：
 *   redis.host = 127.0.0.1
 *   redis.port = 6379
 *   redis.password = ...
 *
 * Sentinel 模式（设了 redis.sentinels 自动启用）：
 *   redis.sentinels = 127.0.0.1:26379,127.0.0.1:26380,127.0.0.1:26381
 *   redis.sentinel_master = im-master
 *   或环境变量：MUDUO_IM_REDIS_SENTINELS（逗号分隔）
 */
#pragma once

#include "Config.h"
#include "pool/RedisPool.h"
#include <cstdlib>
#include <iostream>
#include <string>

namespace im_common {

inline RedisPoolConfig loadRedisConfig(const AppConfig& config) {
    RedisPoolConfig cfg;
    cfg.host     = config.get("redis.host", "127.0.0.1");
    cfg.port     = config.getInt("redis.port", 6379);
    cfg.password = config.get("redis.password", "");
    cfg.minSize  = (size_t)config.getInt("redis.pool_min", 3);
    cfg.maxSize  = (size_t)config.getInt("redis.pool_max", 10);

    // Sentinel 模式：env > config
    std::string sentinelList = config.get("redis.sentinels", "");
    if (sentinelList.empty()) {
        if (const char* e = std::getenv("MUDUO_IM_REDIS_SENTINELS")) sentinelList = e;
    }
    if (!sentinelList.empty()) {
        size_t i = 0;
        while (i < sentinelList.size()) {
            size_t comma = sentinelList.find(',', i);
            if (comma == std::string::npos) comma = sentinelList.size();
            std::string s = sentinelList.substr(i, comma - i);
            // trim
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.erase(s.begin());
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))  s.pop_back();
            if (!s.empty()) cfg.sentinels.push_back(s);
            i = comma + 1;
        }
        cfg.sentinelMaster = config.get("redis.sentinel_master", "im-master");
        std::cerr << "[redis-config] sentinel mode: master='" << cfg.sentinelMaster
                  << "' sentinels=" << cfg.sentinels.size() << "\n";
    }
    return cfg;
}

}  // namespace im_common
