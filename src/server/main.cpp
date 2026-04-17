/**
 * @file main.cpp
 * @brief muduo-im 服务器入口，从配置文件加载参数并启动 ChatServer
 *
 * 启动流程：加载配置文件 -> MySQL 配置 -> Redis 配置 -> EventLoop 创建 -> ChatServer 初始化 -> 启动事件循环。
 * 通过注册 SIGINT/SIGTERM 信号处理函数实现优雅退出。
 * 配置文件路径可通过命令行参数指定，默认为 ../config.ini。
 */

#include "server/ChatServer.h"
#include "common/Config.h"
#include "common/Logging.h"
#include "net/EventLoop.h"
#include "asynclogger/AsyncLogger.h"
#include <cstdlib>
#include <iostream>
#include <signal.h>

/// 全局 ChatServer 指针，供信号处理函数触发优雅关闭（刷队列 + 关闭服务器）
ChatServer* g_server = nullptr;
/// 全局 EventLoop 指针，兜底退出路径
EventLoop* g_loop = nullptr;

/**
 * @brief 信号处理函数，实现优雅退出
 *
 * 捕获 SIGINT（Ctrl+C）和 SIGTERM（kill 命令）信号。
 * 优先调用 ChatServer::shutdown() 以便刷写 Redis 消息队列、
 * 向 WebSocket 客户端发送 Close 帧，再退出事件循环；
 * 若 ChatServer 尚未初始化，退回到 EventLoop::quit()。
 *
 * @param sig 信号编号
 */
void signalHandler(int sig) {
    LOG_INFO("Received signal %d, shutting down gracefully...", sig);
    if (g_server) {
        g_server->shutdown();  // graceful: flush queue + close servers
    } else if (g_loop) {
        g_loop->quit();  // fallback
    }
}

/**
 * @brief 服务器主函数
 *
 * 初始化流程：
 * 1. 注册信号处理函数（SIGINT/SIGTERM -> 优雅退出）
 * 2. 加载配置文件（默认 ../config.ini，可通过命令行参数覆盖）
 * 3. 配置 MySQL 连接池参数
 * 4. 配置 Redis 连接池参数
 * 5. 创建 EventLoop（Reactor 事件循环）
 * 6. 创建并启动 ChatServer（HTTP API + WebSocket 服务）
 * 7. 进入事件循环，阻塞直到收到退出信号
 */
int main(int argc, char* argv[]) {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // ---- 加载配置文件 ----
    std::string configFile = (argc > 1) ? argv[1] : "../config.ini";
    AppConfig config;
    if (!config.load(configFile)) {
        std::cerr << "Warning: config file not found: " << configFile << ", using defaults" << std::endl;
    }

    // ---- 启动异步日志（双缓冲，不阻塞业务线程）----
    {
        std::string logFile = config.get("server.log_file", "muduo-im.log");
        int logLevel = config.getInt("server.log_level", 1);  // 默认 INFO
        AsyncLogger::instance().setLogFile(logFile);
        AsyncLogger::instance().setLogLevel(static_cast<LogLevel>(logLevel));
        AsyncLogger::instance().start();
    }

    // ---- MySQL 连接池配置 ----
    MySQLPoolConfig mysqlConfig;
    mysqlConfig.host = config.get("mysql.host", "127.0.0.1");
    mysqlConfig.port = config.getInt("mysql.port", 3306);
    mysqlConfig.user = config.get("mysql.user", "root");
    mysqlConfig.password = config.get("mysql.password", "");
    mysqlConfig.database = config.get("mysql.database", "muduo_im");
    mysqlConfig.minSize = config.getInt("mysql.pool_min", 5);
    mysqlConfig.maxSize = config.getInt("mysql.pool_max", 20);

    // ---- Redis 连接池配置 ----
    RedisPoolConfig redisConfig;
    redisConfig.host = config.get("redis.host", "127.0.0.1");
    redisConfig.port = config.getInt("redis.port", 6379);
    redisConfig.password = config.get("redis.password", "");
    redisConfig.minSize = config.getInt("redis.pool_min", 3);
    redisConfig.maxSize = config.getInt("redis.pool_max", 10);

    uint16_t httpPort = config.getInt("server.http_port", 8080);
    uint16_t wsPort = config.getInt("server.ws_port", 9090);

    // JWT 密钥：优先级 ENV > config > 兜底默认值
    // 生产环境请务必通过 MUDUO_IM_JWT_SECRET 环境变量注入强随机密钥
    std::string jwtSecret;
    const char* envSecret = std::getenv("MUDUO_IM_JWT_SECRET");
    if (envSecret && *envSecret) {
        jwtSecret = envSecret;
    } else {
        jwtSecret = config.get("server.jwt_secret", "");
        if (jwtSecret.empty() || jwtSecret == "muduo-im-jwt-secret-key") {
            std::cerr << "WARNING: using default JWT secret — set MUDUO_IM_JWT_SECRET env var for production" << std::endl;
            jwtSecret = "muduo-im-jwt-secret-key-DO-NOT-USE-IN-PRODUCTION";
        }
    }

    // ---- 创建事件循环 ----
    EventLoop loop;
    g_loop = &loop;

    // ---- 创建并启动 ChatServer ----
    ChatServer server(&loop, httpPort, wsPort, mysqlConfig, redisConfig, jwtSecret);
    g_server = &server;  // 注册给信号处理函数，用于优雅关闭
    server.start();

    std::cout << "=== muduo-im ===" << std::endl;
    std::cout << "HTTP API: http://localhost:" << httpPort << std::endl;
    std::cout << "WebSocket: ws://localhost:" << wsPort << std::endl;
    std::cout << "Config: " << configFile << std::endl;

    LOG_INFO("muduo-im started: http=%u ws=%u config=%s",
             (unsigned)httpPort, (unsigned)wsPort, configFile.c_str());
    // 同步输出一条结构化事件日志，方便日志平台聚合启动事件
    LOG_EVENT("server_start",
              "http=" + std::to_string(httpPort) +
              " ws="  + std::to_string(wsPort)   +
              " config=" + configFile);

    loop.loop();

    LOG_INFO("muduo-im shutting down");
    LOG_EVENT("server_stop", "");
    AsyncLogger::instance().stop();

    std::cout << "Server stopped." << std::endl;
    return 0;
}
