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
#include "net/EventLoop.h"
#include "websocket/WebSocketServer.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <signal.h>
#include <string>

static EventLoop* g_loop = nullptr;
static void onSignal(int sig) {
    std::cerr << "[gateway] signal " << sig << ", quit loop\n";
    if (g_loop) g_loop->quit();
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

    // 两种模式：有 registry 走 pool（多 logic + 一致性 hash）；否则走单 logic
    std::unique_ptr<LogicClientPool> pool;
    std::unique_ptr<LogicClient> singleLogic;
    if (!registryAddr.empty()) {
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

    loop.loop();
    if (pool) pool->stop();
    if (singleLogic) singleLogic->stop();
    return 0;
}
