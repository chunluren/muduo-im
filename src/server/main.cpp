#include "server/ChatServer.h"
#include "net/EventLoop.h"
#include <iostream>
#include <signal.h>

EventLoop* g_loop = nullptr;

void signalHandler(int) {
    if (g_loop) g_loop->quit();
}

int main() {
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    MySQLPoolConfig mysqlConfig;
    mysqlConfig.host = "127.0.0.1";
    mysqlConfig.port = 3306;
    mysqlConfig.user = "root";
    mysqlConfig.password = "";
    mysqlConfig.database = "muduo_im";
    mysqlConfig.minSize = 5;
    mysqlConfig.maxSize = 20;

    RedisPoolConfig redisConfig;
    redisConfig.host = "127.0.0.1";
    redisConfig.port = 6379;
    redisConfig.minSize = 3;
    redisConfig.maxSize = 10;

    EventLoop loop;
    g_loop = &loop;

    ChatServer server(&loop, 8080, 9090, mysqlConfig, redisConfig, "muduo-im-jwt-secret-key");
    server.start();

    std::cout << "=== muduo-im ===" << std::endl;
    std::cout << "HTTP API: http://localhost:8080" << std::endl;
    std::cout << "WebSocket: ws://localhost:9090/ws?token=xxx" << std::endl;

    loop.loop();
    std::cout << "Server stopped." << std::endl;
    return 0;
}
