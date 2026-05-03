/**
 * @file gateway/main.cpp
 * @brief muduo-im-gateway 接入层进程（Phase 1.2 W2.D1-D5）
 *
 * 监听 ws，把上行消息走 gRPC unary 转 logic；同时维持一条 RegisterGateway 双向流，
 * 接 logic 的 PushCommand 反向推送给本地 ws session。
 */
#include "LogicClient.h"
#include "LogicClientPool.h"
#include "common/Logging.h"
#include "http/HttpServer.h"
#include "net/EventLoop.h"
#include "websocket/WebSocketServer.h"
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <signal.h>
#include <string>
#include <thread>

static EventLoop*        g_loop = nullptr;
static std::atomic<bool> g_stop{false};
static std::atomic<bool> g_draining{false};   // Phase 5b: /health 切 503，让 LB 先把流量摘走
static int               g_lastSig = 0;

// signal handler 只设 flag —— 不直接调 g_loop->quit()。
// 因为我们要先在另一个线程里发 ws close frame 给所有客户端，
// 等几百毫秒让客户端切到别的 gateway，再让 loop 退出。
static void onSignal(int sig) {
    g_lastSig = sig;
    g_stop.store(true);
}

static std::map<std::string, std::string> parseQuery(const std::string& path) {
    std::map<std::string, std::string> kv;
    auto qpos = path.find('?');
    if (qpos == std::string::npos) return kv;
    std::string q = path.substr(qpos + 1);
    size_t i = 0;
    while (i < q.size()) {
        size_t amp = q.find('&', i);
        if (amp == std::string::npos) amp = q.size();
        std::string seg = q.substr(i, amp - i);
        auto eq = seg.find('=');
        if (eq != std::string::npos) {
            kv[seg.substr(0, eq)] = seg.substr(eq + 1);
        }
        i = amp + 1;
    }
    return kv;
}

// 本地 uid → 一组 ws session（多 device 同 gateway）
class LocalPresence {
public:
    void add(int64_t uid, WsSessionPtr session) {
        std::lock_guard<std::mutex> lk(mu_);
        sessions_[uid].insert(session);
    }
    void remove(int64_t uid, const WsSessionPtr& session) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sessions_.find(uid);
        if (it == sessions_.end()) return;
        it->second.erase(session);
        if (it->second.empty()) sessions_.erase(it);
    }
    /// 返回 false 表示本地无该 uid
    bool sendToUser(int64_t uid, const std::string& payload) {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = sessions_.find(uid);
        if (it == sessions_.end()) return false;
        for (auto& s : it->second) s->sendText(payload);
        return true;
    }
private:
    std::mutex mu_;
    std::map<int64_t, std::set<WsSessionPtr>> sessions_;
};

static std::string makeGatewayId() {
    if (const char* e = std::getenv("MUDUO_IM_GATEWAY_ID")) return e;
    std::random_device rd;
    std::mt19937 g(rd());
    std::uniform_int_distribution<int> d(1000, 9999);
    return "gw-" + std::to_string(d(g));
}

int main(int argc, char* argv[]) {
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    std::string wsAddr    = "0.0.0.0";
    uint16_t    wsPort    = 9091;
    std::string logicAddr = "127.0.0.1:9100";
    std::string registryAddr;

    if (const char* e = std::getenv("MUDUO_IM_GATEWAY_WS_PORT")) wsPort = (uint16_t)std::atoi(e);
    if (const char* e = std::getenv("MUDUO_IM_LOGIC_ADDR"))      logicAddr = e;
    if (const char* e = std::getenv("MUDUO_IM_REGISTRY_ADDR"))   registryAddr = e;
    if (argc > 1) wsPort = (uint16_t)std::atoi(argv[1]);
    if (argc > 2) logicAddr = argv[2];

    std::string gatewayId = makeGatewayId();
    LocalPresence presence;
    auto pushCb = [&presence](int64_t uid, const std::string& payload) {
        bool ok = presence.sendToUser(uid, payload);
        std::cerr << "[gateway] push uid=" << uid
                  << (ok ? " delivered" : " no_local_session") << "\n";
    };

    // 三种模式：
    //   1. MUDUO_IM_USE_ETCD=1 → pool + etcd 服务发现（Phase 3D）
    //   2. MUDUO_IM_REGISTRY_ADDR → pool + gRPC RegistryService（W3）
    //   3. 否则 → 单 logic 直连
    std::unique_ptr<LogicClientPool> pool;
    std::unique_ptr<LogicClient> singleLogic;
    bool useEtcd = false;
    if (const char* e = std::getenv("MUDUO_IM_USE_ETCD")) {
        useEtcd = (std::string(e) == "1" || std::string(e) == "true");
    }
    if (useEtcd) {
        std::string etcdHost = "127.0.0.1";
        int         etcdPort = 2379;
        std::string apiPrefix = "/v3beta";
        std::string prefix    = "services/logic/";
        if (const char* e = std::getenv("MUDUO_IM_ETCD_HOST"))   etcdHost  = e;
        if (const char* e = std::getenv("MUDUO_IM_ETCD_PORT"))   etcdPort  = std::atoi(e);
        if (const char* e = std::getenv("MUDUO_IM_ETCD_PREFIX")) apiPrefix = e;
        if (const char* e = std::getenv("MUDUO_IM_LOGIC_KEY_PREFIX")) prefix = e;
        // registryAddr 此处不需要，但 pool ctor 仍要一个值；传空字符串
        pool.reset(new LogicClientPool("", gatewayId, pushCb));
        // Phase 5: 本 gateway AZ → same-AZ 优先路由
        if (const char* e = std::getenv("MUDUO_IM_AZ")) {
            pool->setLocalAz(e);
        }
        pool->enableEtcd(etcdHost, etcdPort, apiPrefix, prefix);
        pool->start();
        std::cerr << "[gateway] using etcd " << etcdHost << ":" << etcdPort
                  << " prefix=" << prefix
                  << " localAz=" << (std::getenv("MUDUO_IM_AZ") ?: "(unset)") << "\n";
    } else if (!registryAddr.empty()) {
        pool.reset(new LogicClientPool(registryAddr, gatewayId, pushCb));
        pool->start();
        std::cerr << "[gateway] using registry " << registryAddr << "\n";
    } else {
        singleLogic.reset(new LogicClient(logicAddr, gatewayId));
        singleLogic->setPushCallback(pushCb);
        singleLogic->start();
        std::cerr << "[gateway] using single logic " << logicAddr << "\n";
    }

    EventLoop loop;
    g_loop = &loop;
    WebSocketServer wsServer(&loop, InetAddress(wsPort), "muduo-im-gateway");

    // Phase 5b: HTTP /health 端点，给 haproxy http-check 探活
    uint16_t healthPort = 9081;
    if (const char* e = std::getenv("MUDUO_IM_GATEWAY_HEALTH_PORT")) healthPort = (uint16_t)std::atoi(e);
    HttpServer healthServer(&loop, InetAddress(healthPort), "muduo-im-gateway-health");
    healthServer.setThreadNum(1);
    healthServer.GET("/health", [&wsServer, &gatewayId](const HttpRequest&, HttpResponse& resp) {
        if (g_draining.load()) {
            resp.setStatusCode(HttpStatusCode::SERVICE_UNAVAILABLE);
            resp.setContentType("application/json");
            resp.setBody("{\"status\":\"draining\",\"id\":\"" + gatewayId + "\"}");
            return;
        }
        size_t n = wsServer.sessionCount();
        resp.setStatusCode(HttpStatusCode::OK);
        resp.setContentType("application/json");
        resp.setBody("{\"status\":\"healthy\",\"id\":\"" + gatewayId +
                     "\",\"ws_sessions\":" + std::to_string(n) + "}");
    });
    healthServer.start();
    std::cerr << "[gateway] /health on :" << healthPort << "\n";

    WebSocketConfig wsCfg;
    wsCfg.idleTimeoutMs = 60000;
    wsCfg.enablePingPong = true;
    wsCfg.pingIntervalMs = 30000;
    wsServer.setConfig(wsCfg);

    wsServer.setHandshakeValidator(
        [](const TcpConnectionPtr&, const std::string& path,
           const std::map<std::string, std::string>&) -> bool {
            auto kv = parseQuery(path);
            auto it = kv.find("uid");
            return it != kv.end() && std::atoll(it->second.c_str()) > 0;
        });

    auto notifyOpen = [&](int64_t uid, const std::string& dev) {
        if (pool) pool->notifyConnOpen(uid, dev);
        else singleLogic->notifyConnOpen(uid, dev);
    };
    auto notifyClose = [&](int64_t uid, const std::string& dev) {
        if (pool) pool->notifyConnClose(uid, dev);
        else singleLogic->notifyConnClose(uid, dev);
    };
    auto callHandleMessage = [&](int64_t uid, const std::string& dev,
                                 const std::string& connId, const std::string& payload,
                                 std::string* reply) {
        if (pool) {
            return pool->handleMessage(uid, dev, connId, std::to_string(uid), payload, reply);
        }
        return singleLogic->handleMessage(uid, dev, connId, payload, reply);
    };

    wsServer.setConnectionHandler([&](const WsSessionPtr& session) {
        std::string path = session->getContext("path");
        auto kv = parseQuery(path);
        std::string uidStr = kv.count("uid") ? kv["uid"] : "0";
        std::string dev    = kv.count("device") ? kv["device"] : "default";
        int64_t uid = std::atoll(uidStr.c_str());
        session->setContext("uid", uidStr);
        session->setContext("device", dev);
        presence.add(uid, session);
        notifyOpen(uid, dev);
        std::cerr << "[gateway] open uid=" << uid << " dev=" << dev << "\n";
    });

    std::atomic<uint64_t> reqCount{0};

    wsServer.setMessageHandler([&](const WsSessionPtr& session, const WsMessage& msg) {
        if (!msg.isText()) return;
        std::string uidStr = session->getContext("uid", "0");
        std::string dev    = session->getContext("device", "default");
        int64_t uid = std::atoll(uidStr.c_str());
        std::string connId = "g-" + uidStr + "-" + std::to_string(reqCount.fetch_add(1));

        std::string reply;
        auto st = callHandleMessage(uid, dev, connId, msg.text(), &reply);
        if (!st.ok()) {
            std::cerr << "[gateway] logic RPC failed: " << st.error_code()
                      << " " << st.error_message() << "\n";
            session->sendText(R"({"type":"error","code":"logic_unavailable"})");
            return;
        }
        if (!reply.empty()) session->sendText(reply);
    });

    wsServer.setCloseHandler([&](const WsSessionPtr& session) {
        std::string uidStr = session->getContext("uid", "0");
        std::string dev    = session->getContext("device", "default");
        int64_t uid = std::atoll(uidStr.c_str());
        presence.remove(uid, session);
        notifyClose(uid, dev);
        std::cerr << "[gateway] close uid=" << uid << "\n";
    });

    wsServer.start();
    std::cout << "muduo-im-gateway: id=" << gatewayId
              << " ws=" << wsAddr << ":" << wsPort
              << " logic=" << logicAddr << std::endl;
    LOG_EVENT("gateway_start",
              "id=" + gatewayId + " ws_port=" + std::to_string(wsPort) +
              " logic=" + logicAddr);

    // graceful 下线时序（Phase 4.1B + 5b）：
    //   1. SIGTERM → g_stop=true
    //   2. /health 切 503，让 haproxy 在下个 check 周期（默认 2s × 3 = 6s）摘掉本节点
    //   3. 等 lbDrainMs（默认 7s），让 LB 把新连接停止打过来
    //   4. 给所有 ws session 发 close(1001 going_away) — 客户端去重连别的 gateway
    //   5. 等 wsGraceMs（默认 800ms），客户端断开 + 切 gateway
    //   6. quit loop → 退出主线
    int wsGraceMs = 800;
    int lbDrainMs = 7000;
    if (const char* e = std::getenv("MUDUO_IM_DRAIN_GRACE_MS"))   wsGraceMs = std::atoi(e);
    if (const char* e = std::getenv("MUDUO_IM_LB_DRAIN_MS"))      lbDrainMs = std::atoi(e);
    std::thread shutdownWatcher([&wsServer, &loop, wsGraceMs, lbDrainMs]() {
        while (!g_stop.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        // 2) 先翻 draining flag → /health 立刻返 503
        g_draining.store(true);
        std::cerr << "[gateway] signal " << g_lastSig
                  << ", /health → 503 (lbDrainMs=" << lbDrainMs << ")\n";
        // 3) 等 LB 把流量摘走
        std::this_thread::sleep_for(std::chrono::milliseconds(lbDrainMs));

        size_t n = wsServer.sessionCount();
        std::cerr << "[gateway] graceful drain " << n << " ws sessions"
                  << " (wsGraceMs=" << wsGraceMs << ")\n";
        // 4) 在 loop 线程里发 close frame —— ws 帧只能从 loop 线程 send
        loop.runInLoop([&wsServer]() {
            for (auto& s : wsServer.getAllSessions()) {
                s->close(1001, "going_away");
            }
        });
        // 5) 给客户端一次往返时间断开 + 切 gateway
        std::this_thread::sleep_for(std::chrono::milliseconds(wsGraceMs));
        // 6) 退主循环
        loop.quit();
    });

    loop.loop();
    g_stop.store(true);  // 让 watcher 在自然退出场景也下来
    if (shutdownWatcher.joinable()) shutdownWatcher.join();
    if (pool) pool->stop();
    if (singleLogic) singleLogic->stop();
    std::cerr << "[gateway] stopped\n";
    return 0;
}
