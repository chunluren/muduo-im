/**
 * @file main.cpp
 * @brief muduo-im 服务器入口，初始化配置并启动 ChatServer
 *
 * 启动流程：MySQL 配置 -> Redis 配置 -> EventLoop 创建 -> ChatServer 初始化 -> 启动事件循环。
 * 通过注册 SIGINT/SIGTERM 信号处理函数实现优雅退出。
 */

#include "server/ChatServer.h"
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
 * 2. 配置 MySQL 连接池参数
 * 3. 配置 Redis 连接池参数
 * 4. 创建 EventLoop（Reactor 事件循环）
 * 5. 创建并启动 ChatServer（HTTP API + WebSocket 服务）
 * 6. 进入事件循环，阻塞直到收到退出信号
 */
int main() {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    // ---- MySQL 连接池配置 ----
    MySQLPoolConfig mysqlConfig;
    mysqlConfig.host = "127.0.0.1";       ///< MySQL 主机地址，默认本机
    mysqlConfig.port = 3306;              ///< MySQL 端口，默认 3306
    mysqlConfig.user = "root";            ///< 数据库用户名
    mysqlConfig.password = "";            ///< 数据库密码（生产环境应从配置文件或环境变量读取）
    mysqlConfig.database = "muduo_im";    ///< 数据库名称
    mysqlConfig.minSize = 5;              ///< 连接池最小连接数（空闲时保持的连接数）
    mysqlConfig.maxSize = 20;             ///< 连接池最大连接数（并发高峰时的上限）

    // ---- Redis 连接池配置 ----
    RedisPoolConfig redisConfig;
    redisConfig.host = "127.0.0.1";       ///< Redis 主机地址，默认本机
    redisConfig.port = 6379;              ///< Redis 端口，默认 6379
    redisConfig.minSize = 3;              ///< 连接池最小连接数
    redisConfig.maxSize = 10;             ///< 连接池最大连接数

    // ---- 创建事件循环 ----
    EventLoop loop;
    g_loop = &loop;

    // ---- 创建并启动 ChatServer ----
    // 参数：EventLoop、HTTP 端口(8080)、WebSocket 端口(9090)、MySQL 配置、Redis 配置、JWT 密钥
    ChatServer server(&loop, 8080, 9090, mysqlConfig, redisConfig, "muduo-im-jwt-secret-key");
    server.start();

    std::cout << "=== muduo-im ===" << std::endl;
    std::cout << "HTTP API: http://localhost:8080" << std::endl;
    std::cout << "WebSocket: ws://localhost:9090/ws?token=xxx" << std::endl;

    loop.loop();
    std::cout << "Server stopped." << std::endl;
    return 0;
}
