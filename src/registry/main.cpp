/**
 * @file registry/main.cpp
 * @brief muduo-im-registry 服务发现进程（Phase 1.2 W3.D1-D2 骨架，单点）
 */
#include "RegistryServiceImpl.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <signal.h>
#include <string>
#include <thread>

static std::unique_ptr<grpc::Server> g_server;
static std::atomic<bool>             g_stop{false};
static int                           g_lastSig = 0;

// signal handler 只设 flag（async-signal-safe），watcher 线程做真正的 Shutdown
static void onSignal(int sig) {
    g_lastSig = sig;
    g_stop.store(true);
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
    service.startGc();   // 启动 TTL GC 线程（每 5s 扫，超 30s 没刷新视为死亡）
    std::cerr << "[registry] listening on " << addr
              << " (ttl=" << RegistryServiceImpl::kTtlSec
              << "s, gc=" << RegistryServiceImpl::kGcIntervalSec << "s)\n";
    std::thread shutdownWatcher([]() {
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::cerr << "[registry] signal " << g_lastSig << ", shutdown(deadline=5s)\n";
        if (g_server) {
            g_server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(5));
        }
    });

    g_server->Wait();
    g_stop.store(true);
    if (shutdownWatcher.joinable()) shutdownWatcher.join();
    service.stopGc();
    return 0;
}
