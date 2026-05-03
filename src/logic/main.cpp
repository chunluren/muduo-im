/**
 * @file logic/main.cpp
 * @brief muduo-im-logic 业务层进程入口（Phase 1.2 W1.D3-D5）
 *
 * 默认监听 0.0.0.0:9100。从 config.ini 加载 mysql 池，将 MessageService 注入
 * LogicServiceImpl 后启动 gRPC server。
 */
#include "LogicServiceImpl.h"
#include "im/registry.grpc.pb.h"
#include "common/Config.h"
#include "pool/MySQLPool.h"
#include "util/EtcdClient.h"
#include "util/Snowflake.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <nlohmann/json.hpp>
#include <signal.h>
#include <string>
#include <thread>

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

    // 自注册：默认走 RegistryService gRPC（W3.D1-D2）；
    // MUDUO_IM_USE_ETCD=1 时走 etcd lease+put 路径（Phase 3C）。
    std::atomic<bool> regRunning{true};
    std::thread regThread;
    std::string regAddr;
    if (const char* e = std::getenv("MUDUO_IM_REGISTRY_ADDR")) regAddr = e;
    std::string instanceId = "logic-" + std::to_string(::getpid());
    if (const char* e = std::getenv("MUDUO_IM_LOGIC_INSTANCE_ID")) instanceId = e;

    // advertised host:port（替换 0.0.0.0 为可达地址）
    std::string advertised = addr;
    {
        std::string ad = "127.0.0.1";
        if (const char* e = std::getenv("MUDUO_IM_LOGIC_ADVERTISE_HOST")) ad = e;
        auto colon = addr.find(':');
        if (colon != std::string::npos) advertised = ad + addr.substr(colon);
    }

    bool useEtcd = false;
    if (const char* e = std::getenv("MUDUO_IM_USE_ETCD")) {
        useEtcd = (std::string(e) == "1" || std::string(e) == "true");
    }

    if (useEtcd) {
        std::string etcdHost = "127.0.0.1";
        int         etcdPort = 2379;
        std::string apiPrefix = "/v3beta";
        if (const char* e = std::getenv("MUDUO_IM_ETCD_HOST"))   etcdHost  = e;
        if (const char* e = std::getenv("MUDUO_IM_ETCD_PORT"))   etcdPort  = std::atoi(e);
        if (const char* e = std::getenv("MUDUO_IM_ETCD_PREFIX")) apiPrefix = e;

        regThread = std::thread([&regRunning, etcdHost, etcdPort, apiPrefix, advertised, instanceId]() {
            EtcdClient etcd(etcdHost, etcdPort, apiPrefix);
            std::string key = "services/logic/" + instanceId;
            nlohmann::json meta = {
                {"service",  "logic"},
                {"addr",     advertised},
                {"weight",   1},
                {"instance", instanceId},
            };
            std::string value = meta.dump();

            int64_t leaseId = 0;
            int backoff = 1;
            // grant + put（带退避重试，等 etcd 起来）
            while (regRunning.load()) {
                leaseId = etcd.grantLease(15);
                if (leaseId > 0 && etcd.put(key, value, leaseId)) {
                    std::cerr << "[logic] etcd registered key=" << key
                              << " advertise=" << advertised
                              << " lease=" << leaseId << "\n";
                    break;
                }
                std::cerr << "[logic] etcd register failed, retry in " << backoff << "s\n";
                std::this_thread::sleep_for(std::chrono::seconds(backoff));
                backoff = std::min(backoff * 2, 30);
            }
            // KeepAlive 每 5s（lease TTL=15s，3 次容错）
            int keepFailures = 0;
            while (regRunning.load() && leaseId != 0) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
                if (!regRunning.load()) break;
                int ttl = etcd.keepAlive(leaseId);
                if (ttl <= 0) {
                    if (++keepFailures >= 3) {
                        std::cerr << "[logic] etcd keepalive failed 3x, re-grant\n";
                        leaseId = etcd.grantLease(15);
                        if (leaseId > 0) etcd.put(key, value, leaseId);
                        keepFailures = 0;
                    }
                } else {
                    keepFailures = 0;
                }
            }
            // 退出前删 key（lease 也会过期，主动删一次为了即时下线）
            etcd.del(key);
            std::cerr << "[logic] etcd deregistered\n";
        });
    } else if (!regAddr.empty()) {
        regThread = std::thread([&regRunning, regAddr, advertised, instanceId]() {
            auto ch = grpc::CreateChannel(regAddr, grpc::InsecureChannelCredentials());
            auto stub = im::RegistryService::NewStub(ch);

            int64_t leaseId = 0;
            int backoff = 1;
            while (regRunning.load()) {
                im::RegisterRequest req;
                auto* ep = req.mutable_ep();
                ep->set_instance_id(instanceId);
                ep->set_service("logic");
                ep->set_addr(advertised);
                ep->set_weight(1);
                im::RegisterResponse resp;
                grpc::ClientContext ctx;
                ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
                auto st = stub->Register(&ctx, req, &resp);
                if (st.ok() && resp.status().code() == im::StatusCode::OK) {
                    leaseId = resp.lease_id();
                    std::cerr << "[logic] registered to " << regAddr
                              << " advertise=" << advertised
                              << " lease=" << leaseId << "\n";
                    break;
                }
                std::cerr << "[logic] register failed, retry in " << backoff << "s\n";
                std::this_thread::sleep_for(std::chrono::seconds(backoff));
                backoff = std::min(backoff * 2, 30);
            }
            // KeepAlive 周期（写一下心跳，registry 当前不强校验 TTL）
            while (regRunning.load() && leaseId != 0) {
                std::this_thread::sleep_for(std::chrono::seconds(10));
                im::KeepAliveRequest kreq;
                kreq.set_lease_id(leaseId);
                im::KeepAliveResponse kresp;
                grpc::ClientContext kctx;
                kctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
                stub->KeepAlive(&kctx, kreq, &kresp);
            }
            // 退出前注销
            im::DeregisterRequest dreq;
            dreq.set_instance_id(instanceId);
            im::DeregisterResponse dresp;
            grpc::ClientContext dctx;
            dctx.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(2));
            stub->Deregister(&dctx, dreq, &dresp);
            std::cerr << "[logic] deregistered\n";
        });
    }

    g_server->Wait();
    regRunning = false;
    if (regThread.joinable()) regThread.join();
    std::cerr << "[logic] stopped\n";
    return 0;
}
