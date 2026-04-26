/**
 * @file registry/main.cpp
 * @brief muduo-im-registry 服务发现进程（Phase 1.2 W3.D1-D2 骨架，单点）
 */
#include "RegistryServiceImpl.h"
#include <cstdlib>
#include <iostream>
#include <signal.h>
#include <string>

static std::unique_ptr<grpc::Server> g_server;
static void onSignal(int sig) {
    std::cerr << "[registry] signal " << sig << ", shutdown\n";
    if (g_server) g_server->Shutdown();
}

int main(int argc, char* argv[]) {
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    std::string addr = "0.0.0.0:9200";
    if (const char* e = std::getenv("MUDUO_IM_REGISTRY_ADDR")) addr = e;
    if (argc > 1) addr = argv[1];

    RegistryServiceImpl service;
    grpc::ServerBuilder b;
    b.AddListeningPort(addr, grpc::InsecureServerCredentials());
    b.RegisterService(&service);
    g_server = b.BuildAndStart();
    if (!g_server) {
        std::cerr << "[registry] failed to bind " << addr << "\n";
        return 1;
    }
    std::cerr << "[registry] listening on " << addr << "\n";
    g_server->Wait();
    return 0;
}
