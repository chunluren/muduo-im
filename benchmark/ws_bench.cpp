// ws_bench.cpp — C++ WebSocket 压力测试客户端
// 用法: ./ws_bench <host> <port> <num_clients> <messages_per_client> <token>

#include <iostream>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <string>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <random>

struct Stats {
    std::atomic<int> connected{0};
    std::atomic<int> sent{0};
    std::atomic<int> errors{0};
    std::atomic<int64_t> totalLatencyUs{0};
};

// Simple WebSocket frame encoder (masked as required by RFC 6455 for client frames)
std::string encodeWsFrame(const std::string& payload) {
    std::string frame;
    frame += (char)0x81; // FIN + TEXT

    // Mask + length
    uint8_t mask_bit = 0x80; // masked (required by RFC for client)
    if (payload.size() <= 125) {
        frame += (char)(mask_bit | payload.size());
    } else if (payload.size() <= 65535) {
        frame += (char)(mask_bit | 126);
        frame += (char)((payload.size() >> 8) & 0xFF);
        frame += (char)(payload.size() & 0xFF);
    }

    // Masking key (simple)
    uint8_t maskKey[4] = {0x12, 0x34, 0x56, 0x78};
    frame.append((char*)maskKey, 4);

    // Masked payload
    for (size_t i = 0; i < payload.size(); ++i) {
        frame += (char)(payload[i] ^ maskKey[i % 4]);
    }

    return frame;
}

void clientThread(int id, const std::string& host, int port, const std::string& token,
                   int numMessages, Stats& stats) {
    // TCP connect
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { stats.errors++; return; }

    sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        stats.errors++;
        return;
    }

    // WebSocket handshake
    std::string handshake =
        "GET /ws?token=" + token + " HTTP/1.1\r\n"
        "Host: " + host + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    send(sock, handshake.c_str(), handshake.size(), 0);

    // Read handshake response
    char buf[4096];
    int n = recv(sock, buf, sizeof(buf), 0);
    if (n <= 0 || std::string(buf, n).find("101") == std::string::npos) {
        close(sock);
        stats.errors++;
        return;
    }

    stats.connected++;

    // Send messages
    for (int i = 0; i < numMessages; ++i) {
        std::string msg = "{\"type\":\"msg\",\"to\":\"1\",\"content\":\"bench-"
            + std::to_string(id) + "-" + std::to_string(i)
            + "\",\"msgId\":\"" + std::to_string(id) + "-" + std::to_string(i) + "\"}";

        std::string frame = encodeWsFrame(msg);

        auto t0 = std::chrono::steady_clock::now();
        if (::send(sock, frame.c_str(), frame.size(), 0) > 0) {
            stats.sent++;

            // Try to read ACK (non-blocking with short timeout)
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 100000; // 100ms
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            recv(sock, buf, sizeof(buf), 0);

            auto t1 = std::chrono::steady_clock::now();
            int64_t us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
            stats.totalLatencyUs += us;
        } else {
            stats.errors++;
        }
    }

    close(sock);
}

int main(int argc, char* argv[]) {
    if (argc < 6) {
        std::cerr << "用法: " << argv[0] << " <host> <port> <clients> <msg_per_client> <token>" << std::endl;
        return 1;
    }

    std::string host = argv[1];
    int port = std::stoi(argv[2]);
    int numClients = std::stoi(argv[3]);
    int msgsPerClient = std::stoi(argv[4]);
    std::string token = argv[5];

    Stats stats;

    std::cout << "=== C++ WebSocket Benchmark ===" << std::endl;
    std::cout << "Target: " << host << ":" << port << std::endl;
    std::cout << "Clients: " << numClients << ", Messages/client: " << msgsPerClient << std::endl;

    auto t0 = std::chrono::steady_clock::now();

    std::vector<std::thread> threads;
    for (int i = 0; i < numClients; ++i) {
        threads.emplace_back(clientThread, i, host, port, token, msgsPerClient, std::ref(stats));
    }

    for (auto& t : threads) t.join();

    auto t1 = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();

    int totalSent = stats.sent.load();
    int64_t totalLatency = stats.totalLatencyUs.load();

    std::cout << std::endl;
    std::cout << "======== 结果 ========" << std::endl;
    std::cout << "连接成功: " << stats.connected.load() << "/" << numClients << std::endl;
    std::cout << "消息发送: " << totalSent << std::endl;
    std::cout << "错误: " << stats.errors.load() << std::endl;
    std::cout << "耗时: " << elapsed << "s" << std::endl;
    std::cout << "QPS: " << (totalSent / elapsed) << std::endl;
    if (totalSent > 0) {
        std::cout << "平均延迟: " << (totalLatency / totalSent / 1000.0) << "ms" << std::endl;
    }

    return 0;
}
