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
#include "net/EventLoop.h"
#include <iostream>
#include <signal.h>

/// 全局 EventLoop 指针，供信号处理函数访问以触发退出
EventLoop* g_loop = nullptr;

/**
 * @brief 信号处理函数，实现优雅退出
 *
 * 捕获 SIGINT（Ctrl+C）和 SIGTERM（kill 命令）信号，
 * 调用 EventLoop::quit() 使事件循环安全退出，而非直接终止进程。
 *
 * @param 信号编号（未使用）
 */
void signalHandler(int) {
    if (g_loop) g_loop->quit();
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
    std::string jwtSecret = config.get("server.jwt_secret", "muduo-im-jwt-secret-key");

    // ---- 创建事件循环 ----
    EventLoop loop;
    g_loop = &loop;

    // ---- 创建并启动 ChatServer ----
    ChatServer server(&loop, httpPort, wsPort, mysqlConfig, redisConfig, jwtSecret);
    server.start();

    std::cout << "=== muduo-im ===" << std::endl;
    std::cout << "HTTP API: http://localhost:" << httpPort << std::endl;
    std::cout << "WebSocket: ws://localhost:" << wsPort << std::endl;
    std::cout << "Config: " << configFile << std::endl;

    loop.loop();
    std::cout << "Server stopped." << std::endl;
    return 0;
}
