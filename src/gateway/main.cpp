/**
 * @file gateway/main.cpp
 * @brief muduo-im-gateway 接入层进程（Phase 1.2 W2.D1-D3 骨架）
 *
 * 默认 ws 监听 0.0.0.0:9091；每条 ws 消息通过 gRPC unary 转给 logic（默认
 * 127.0.0.1:9100），把 logic 的 inline_reply 写回客户端。
 *
 * 简化（W2.D5 起补）：
 *   - 不做 JWT，path query ?uid=&device= 直接拿
 *   - 不做 logic 反向流，所以 logic 主动推送的消息（来自其他用户）暂时收不到
 *   - 单 logic 实例写死地址；W3 起接 RegistryService 做一致性 hash
 */
#include "LogicClient.h"
#include "common/Logging.h"
#include "net/EventLoop.h"
#include "websocket/WebSocketServer.h"
#include <atomic>
#include <cstdlib>
#include <iostream>
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

int main(int argc, char* argv[]) {
    signal(SIGINT, onSignal);
    signal(SIGTERM, onSignal);

    std::string wsAddr    = "0.0.0.0";
    uint16_t    wsPort    = 9091;
    std::string logicAddr = "127.0.0.1:9100";

    if (const char* e = std::getenv("MUDUO_IM_GATEWAY_WS_PORT")) wsPort = (uint16_t)std::atoi(e);
    if (const char* e = std::getenv("MUDUO_IM_LOGIC_ADDR"))      logicAddr = e;
    if (argc > 1) wsPort = (uint16_t)std::atoi(argv[1]);
    if (argc > 2) logicAddr = argv[2];

    LogicClient logic(logicAddr);

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

    wsServer.setConnectionHandler([](const WsSessionPtr& session) {
        std::string path = session->getContext("path");
        auto kv = parseQuery(path);
        std::string uid = kv.count("uid") ? kv["uid"] : "0";
        std::string dev = kv.count("device") ? kv["device"] : "default";
        session->setContext("uid", uid);
        session->setContext("device", dev);
        std::cerr << "[gateway] open uid=" << uid << " dev=" << dev << "\n";
    });

    std::atomic<uint64_t> reqCount{0};

    wsServer.setMessageHandler([&logic, &reqCount](const WsSessionPtr& session, const WsMessage& msg) {
        if (!msg.isText()) return;
        std::string uidStr = session->getContext("uid", "0");
        std::string dev    = session->getContext("device", "default");
        int64_t uid = std::atoll(uidStr.c_str());
        // 用本次请求序号作为 conn_id 后缀（生产应当用 muduo conn 唯一 id）
        std::string connId = "g-" + uidStr + "-" + std::to_string(reqCount.fetch_add(1));

        std::string reply;
        auto st = logic.handleMessage(uid, dev, connId, msg.text(), &reply);
        if (!st.ok()) {
            std::cerr << "[gateway] logic RPC failed: " << st.error_code()
                      << " " << st.error_message() << "\n";
            session->sendText(R"({"type":"error","code":"logic_unavailable"})");
            return;
        }
        if (!reply.empty()) session->sendText(reply);
    });

    wsServer.setCloseHandler([](const WsSessionPtr& session) {
        std::cerr << "[gateway] close uid=" << session->getContext("uid", "0") << "\n";
    });

    wsServer.start();
    std::cout << "muduo-im-gateway: ws=" << wsAddr << ":" << wsPort
              << " logic=" << logicAddr << std::endl;
    LOG_EVENT("gateway_start",
              "ws_port=" + std::to_string(wsPort) + " logic=" + logicAddr);

    loop.loop();
    return 0;
}
