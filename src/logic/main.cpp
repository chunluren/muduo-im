/**
 * @file logic/main.cpp
 * @brief muduo-im-logic 业务层进程入口（Phase 1.2 W1.D3 骨架）
 *
 * 默认监听 0.0.0.0:9100。后续 W2 起接真正的 MessageService / OnlineManager。
 */
#include "LogicServiceImpl.h"
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

    std::string addr = "0.0.0.0:9100";
    if (const char* env = std::getenv("MUDUO_IM_LOGIC_ADDR")) addr = env;
    if (argc > 1) addr = argv[1];

    LogicServiceImpl service;
    grpc::ServerBuilder builder;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    g_server = builder.BuildAndStart();
    if (!g_server) {
        std::cerr << "[logic] failed to bind " << addr << "\n";
        return 1;
    }
    std::cerr << "[logic] listening on " << addr << "\n";
    g_server->Wait();
    std::cerr << "[logic] stopped\n";
    return 0;
}
