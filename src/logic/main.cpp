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
#include "http/HttpServer.h"
#include "net/EventLoop.h"
#include "pool/MySQLPool.h"
#include "util/EtcdClient.h"
#include "util/Metrics.h"
#include "util/Snowflake.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <signal.h>
#include <string>
#include <thread>

static std::unique_ptr<grpc::Server> g_server;
// signal handler only sets the flag (async-signal-safe)；后面的 watcher
// 线程在普通上下文里调 Shutdown(deadline)。直接在 handler 里调 Shutdown
// 在 bidi stream 没断时会 hang，且本来就不 signal-safe。
static std::atomic<bool> g_stop{false};
static int               g_lastSig = 0;

// 注册线程的协调状态：watcher 线程要在 SIGTERM 时立刻让 regThread 醒来
// 并 deregister，然后再让它进入 Shutdown 流程（先把 etcd key 拿掉，
// 给 gateway 1-2s 把流量切走，再 Shutdown 在飞 RPC）
static std::atomic<bool>     g_regRunning{true};
static std::mutex            g_regMu;
static std::condition_variable g_regCv;

static void onSignal(int sig) {
    g_lastSig = sig;
    g_stop.store(true);
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

    // 实例 id（http /health 标识 + 后面 etcd 注册都用）
    std::string instanceId = "logic-" + std::to_string(::getpid());
    if (const char* e = std::getenv("MUDUO_IM_LOGIC_INSTANCE_ID")) instanceId = e;

    // ---- HTTP /metrics + /health（独立 EventLoop + 后台线程）----
    int healthPort = 9110;
    if (const char* e = std::getenv("MUDUO_IM_LOGIC_HEALTH_PORT")) healthPort = std::atoi(e);
    EventLoop httpLoop;
    HttpServer httpServer(&httpLoop, InetAddress(healthPort), "muduo-im-logic-health");
    httpServer.setThreadNum(1);
    httpServer.GET("/health", [&registry, instanceId](const HttpRequest&, HttpResponse& resp) {
        size_t gw = registry ? registry->gatewayCount() : 0;
        resp.setStatusCode(HttpStatusCode::OK);
        resp.setContentType("application/json");
        resp.setBody("{\"status\":\"healthy\",\"id\":\"" + instanceId +
                     "\",\"gateway_streams\":" + std::to_string(gw) + "}");
    });
    httpServer.enableMetrics();   // /metrics
    httpServer.start();
    std::thread httpThread([&httpLoop]{ httpLoop.loop(); });
    std::cerr << "[logic] /health and /metrics on :" << healthPort << "\n";

    // 周期 gauge：当前 attached gateway 数
    std::atomic<bool> gaugeRunning{true};
    std::thread gaugeThread([&gaugeRunning, &registry]() {
        while (gaugeRunning.load()) {
            for (int i = 0; i < 50 && gaugeRunning.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (!gaugeRunning.load()) break;
            Metrics::instance().gauge("logic_attached_gateways",
                static_cast<int64_t>(registry ? registry->gatewayCount() : 0));
        }
    });

    // 自注册：默认走 RegistryService gRPC（W3.D1-D2）；
    // MUDUO_IM_USE_ETCD=1 时走 etcd lease+put 路径（Phase 3C）。
    // regRunning + cv 让 SIGTERM 来时 keepAlive sleep 立即被打断（Phase 4.1A）。
    std::thread regThread;
    std::string regAddr;
    if (const char* e = std::getenv("MUDUO_IM_REGISTRY_ADDR")) regAddr = e;
    // instanceId 在前面 http server 前已经定好

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

        std::string az;
        if (const char* e = std::getenv("MUDUO_IM_AZ")) az = e;
        regThread = std::thread([etcdHost, etcdPort, apiPrefix, advertised, instanceId, az]() {
            EtcdClient etcd(etcdHost, etcdPort, apiPrefix);
            std::string key = "services/logic/" + instanceId;
            nlohmann::json meta = {
                {"service",  "logic"},
                {"addr",     advertised},
                {"weight",   1},
                {"instance", instanceId},
            };
            if (!az.empty()) meta["az"] = az;  // Phase 5: AZ 标签
            std::string value = meta.dump();

            // 可中断 sleep：sleep_ms 同时盯 g_regRunning，shutdown 来时立即返回
            auto interruptibleSleep = [](std::chrono::milliseconds dur) {
                std::unique_lock<std::mutex> lk(g_regMu);
                g_regCv.wait_for(lk, dur, []{ return !g_regRunning.load(); });
            };

            int64_t leaseId = 0;
            int backoffSec = 1;
            // grant + put（带退避重试，等 etcd 起来）
            while (g_regRunning.load()) {
                leaseId = etcd.grantLease(15);
                if (leaseId > 0 && etcd.put(key, value, leaseId)) {
                    std::cerr << "[logic] etcd registered key=" << key
                              << " advertise=" << advertised
                              << " lease=" << leaseId << "\n";
                    break;
                }
                std::cerr << "[logic] etcd register failed, retry in " << backoffSec << "s\n";
                interruptibleSleep(std::chrono::seconds(backoffSec));
                backoffSec = std::min(backoffSec * 2, 30);
            }
            // KeepAlive 每 5s（lease TTL=15s，3 次容错）
            int keepFailures = 0;
            while (g_regRunning.load() && leaseId != 0) {
                interruptibleSleep(std::chrono::seconds(5));
                if (!g_regRunning.load()) break;
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
            if (leaseId != 0) etcd.del(key);
            std::cerr << "[logic] etcd deregistered\n";
        });
    } else if (!regAddr.empty()) {
        regThread = std::thread([regAddr, advertised, instanceId]() {
            auto ch = grpc::CreateChannel(regAddr, grpc::InsecureChannelCredentials());
            auto stub = im::RegistryService::NewStub(ch);

            auto interruptibleSleep = [](std::chrono::milliseconds dur) {
                std::unique_lock<std::mutex> lk(g_regMu);
                g_regCv.wait_for(lk, dur, []{ return !g_regRunning.load(); });
            };

            int64_t leaseId = 0;
            int backoffSec = 1;
            while (g_regRunning.load()) {
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
                std::cerr << "[logic] register failed, retry in " << backoffSec << "s\n";
                interruptibleSleep(std::chrono::seconds(backoffSec));
                backoffSec = std::min(backoffSec * 2, 30);
            }
            // KeepAlive 周期（写一下心跳，registry 当前不强校验 TTL）
            while (g_regRunning.load() && leaseId != 0) {
                interruptibleSleep(std::chrono::seconds(10));
                if (!g_regRunning.load()) break;
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

    // graceful 下线时序（Phase 4.1A）：
    //   1. 收到 SIGTERM（g_stop=true）
    //   2. 立刻让 regThread 醒来 → 它去 etcd.del / Deregister
    //   3. join regThread —— 此时本实例已经从服务发现里消失
    //   4. sleep gracePeriodMs —— 给 gateway 一次 pollOnce 的时间把流量切走
    //   5. Shutdown(deadline=3s) —— drain 在飞 RPC，不再进新流量
    int gracePeriodMs = 1500;
    if (const char* e = std::getenv("MUDUO_IM_DRAIN_GRACE_MS")) gracePeriodMs = std::atoi(e);
    std::thread shutdownWatcher([gracePeriodMs]() {
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::cerr << "[logic] signal " << g_lastSig
                  << ", graceful drain (gracePeriodMs=" << gracePeriodMs << ")\n";
        // (2) 通知注册线程退出，让 etcd key 立即消失
        {
            std::lock_guard<std::mutex> lk(g_regMu);
            g_regRunning.store(false);
        }
        g_regCv.notify_all();
        // (4) 注册线程被外面 join；这里只睡 grace
        std::this_thread::sleep_for(std::chrono::milliseconds(gracePeriodMs));
        // (5) 现在 gateway 已经把流量切走，剩下的是已经在飞的 RPC
        if (g_server) {
            std::cerr << "[logic] shutdown(deadline=3s)\n";
            g_server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(3));
        }
    });

    g_server->Wait();
    // Wait() 自然返回（非 signal-driven）时也得让两个线程下来
    g_stop.store(true);
    {
        std::lock_guard<std::mutex> lk(g_regMu);
        g_regRunning.store(false);
    }
    g_regCv.notify_all();
    if (shutdownWatcher.joinable()) shutdownWatcher.join();
    if (regThread.joinable()) regThread.join();
    // 停 http /metrics
    gaugeRunning.store(false);
    if (gaugeThread.joinable()) gaugeThread.join();
    httpLoop.quit();
    if (httpThread.joinable()) httpThread.join();
    std::cerr << "[logic] stopped\n";
    return 0;
}
