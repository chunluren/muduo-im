/**
 * @file logic/main.cpp
 * @brief muduo-im-logic 业务层进程入口（Phase 1.2 W1.D3-D5）
 *
 * 默认监听 0.0.0.0:9100。从 config.ini 加载 mysql 池，将 MessageService 注入
 * LogicServiceImpl 后启动 gRPC server。
 */
#include "LogicServiceImpl.h"
#include "common/Config.h"
#include "pool/MySQLPool.h"
#include "util/Snowflake.h"
#include <cstdlib>
#include <iostream>
#include <signal.h>
#include <string>

static std::unique_ptr<grpc::Server> g_server;

static void onSignal(int sig) {
    std::cerr << "[logic] signal " << sig << ", shutdown\n";
    if (g_server) g_server->Shutdown();
}

int main(int argc, char* argv[]) {
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    // ---- 配置 ----
    std::string configFile = (argc > 2) ? argv[2] : "../config.ini";
    AppConfig config;
    if (!config.load(configFile)) {
        std::cerr << "[logic] WARN: config not found: " << configFile << " (using defaults)\n";
    }

    std::string addr = "0.0.0.0:9100";
    if (const char* env = std::getenv("MUDUO_IM_LOGIC_ADDR")) addr = env;
    if (argc > 1) addr = argv[1];

    // ---- Snowflake ----
    try {
        mymuduo::Snowflake::instance().initFromEnv("MUDUO_IM_WORKER_ID");
    } catch (const std::exception& e) {
        std::cerr << "[logic] FATAL Snowflake init: " << e.what() << "\n";
        return 1;
    }

    // ---- MySQL 池 + MessageService ----
    MySQLPoolConfig dbCfg;
    dbCfg.host     = config.get("mysql.host", "127.0.0.1");
    dbCfg.port     = config.getInt("mysql.port", 3306);
    dbCfg.user     = config.get("mysql.user", "root");
    dbCfg.password = config.get("mysql.password", "");
    dbCfg.database = config.get("mysql.database", "muduo_im");
    dbCfg.minSize  = config.getInt("mysql.pool_min", 3);
    dbCfg.maxSize  = config.getInt("mysql.pool_max", 10);
    auto db = std::make_shared<MySQLPool>(dbCfg);
    auto msgSvc = std::make_shared<MessageService>(db);
    auto registry = std::make_shared<GatewayRegistry>();

    LogicServiceImpl service(msgSvc, registry);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    g_server = builder.BuildAndStart();
    if (!g_server) {
        std::cerr << "[logic] failed to bind " << addr << "\n";
        return 1;
    }
    std::cerr << "[logic] listening on " << addr << " mysql=" << dbCfg.host << ":" << dbCfg.port << "\n";
    g_server->Wait();
    std::cerr << "[logic] stopped\n";
    return 0;
}
