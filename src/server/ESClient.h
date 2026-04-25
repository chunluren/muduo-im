/**
 * @file ESClient.h
 * @brief ElasticSearch HTTP 客户端（Phase 4.4）
 *
 * 设计要点：
 * - 同步 HTTP 调用（阻塞当前线程）—— 用于 search 和 index 的请求
 * - 多节点支持：构造时传 vector<host:port>，请求时按节点轮询 + 故障切换
 * - 不依赖 EventLoop（在业务请求线程内运行）
 * - 简化实现：基于原生 socket（与 RegistryClient 内的 SimpleHttpClient 风格一致）
 *
 * 不实现内容：
 * - 连接池（每次请求新建 socket，ES 调用频率低，不必池化）
 * - bulk pipeline（单次请求最多 100 条，足够用）
 * - 异步：搜索路径无需异步（阻塞在 HTTP handler 线程）
 *
 * @example
 * @code
 * std::vector<std::string> nodes = {"127.0.0.1:9200"};
 * ESClient es(nodes);
 * if (es.indexDoc("messages", "doc-1", "{\"body\":\"hello\"}")) ...
 * std::string searchResp = es.search("messages", "{\"query\":{\"match\":{\"body\":\"hello\"}}}");
 * @endcode
 */
#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

class ESClient {
public:
    /**
     * @param nodes ES 节点地址列表 ["host1:port1", "host2:port2", ...]
     */
    explicit ESClient(const std::vector<std::string>& nodes)
        : nodes_(nodes), nextIdx_(0) {}

    /**
     * @brief 索引（写）单个文档
     *
     * PUT /<index>/_doc/<id>
     *
     * @return true 成功（200/201）；false HTTP 错误或网络异常
     */
    bool indexDoc(const std::string& index, const std::string& docId,
                   const std::string& jsonBody) {
        std::string path = "/" + index + "/_doc/" + urlEncode(docId);
        Response r = request("PUT", path, jsonBody);
        return r.success && r.statusCode >= 200 && r.statusCode < 300;
    }

    /**
     * @brief 删除单个文档
     *
     * DELETE /<index>/_doc/<id>
     */
    bool deleteDoc(const std::string& index, const std::string& docId) {
        Response r = request("DELETE", "/" + index + "/_doc/" + urlEncode(docId), "");
        // 404 在 ES 表示不存在，视为成功（幂等删除）
        return r.success && (r.statusCode < 300 || r.statusCode == 404);
    }

    /**
     * @brief 全文搜索
     *
     * POST /<index>/_search
     *
     * @param queryJson 完整 query body
     *                   例：{"query":{"match":{"body":"hello"}},"size":50}
     * @return ES 原始 JSON 响应（调用方自行解析）；失败返回空串
     */
    std::string search(const std::string& index, const std::string& queryJson) {
        Response r = request("POST", "/" + index + "/_search", queryJson);
        if (!r.success || r.statusCode >= 300) return "";
        return r.body;
    }

    /**
     * @brief 健康检查（探活）
     *
     * GET /_cluster/health
     */
    bool healthy() {
        Response r = request("GET", "/_cluster/health", "");
        return r.success && r.statusCode == 200;
    }

private:
    struct Response {
        bool success = false;
        int statusCode = 0;
        std::string body;
    };

    /**
     * @brief 发送 HTTP 请求，按节点轮询 + 故障切换
     */
    Response request(const std::string& method, const std::string& path,
                      const std::string& body) {
        Response resp;
        if (nodes_.empty()) return resp;

        // 轮询起点
        size_t startIdx = nextIdx_.fetch_add(1) % nodes_.size();
        for (size_t off = 0; off < nodes_.size(); ++off) {
            const std::string& node = nodes_[(startIdx + off) % nodes_.size()];
            resp = doRequest(node, method, path, body);
            if (resp.success && resp.statusCode > 0) return resp;
            // 失败则尝试下一节点
        }
        return resp;
    }

    Response doRequest(const std::string& node, const std::string& method,
                        const std::string& path, const std::string& body) {
        Response resp;
        // 解析 host:port
        auto pos = node.find(':');
        if (pos == std::string::npos) return resp;
        std::string host = node.substr(0, pos);
        int port = std::atoi(node.substr(pos + 1).c_str());

        int sockfd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) return resp;

        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;
        ::setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        ::setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        struct sockaddr_in addr;
        std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
            ::close(sockfd);
            return resp;
        }
        if (::connect(sockfd, (sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(sockfd);
            return resp;
        }

        // 构造 HTTP 请求
        std::string req;
        req += method + " " + path + " HTTP/1.1\r\n";
        req += "Host: " + host + "\r\n";
        req += "Content-Type: application/json\r\n";
        req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
        req += "Connection: close\r\n";
        req += "\r\n";
        req += body;

        size_t sent = 0;
        while (sent < req.size()) {
            ssize_t n = ::send(sockfd, req.c_str() + sent, req.size() - sent, 0);
            if (n <= 0) { ::close(sockfd); return resp; }
            sent += n;
        }

        // 接收（简单全量读到 close）
        std::string raw;
        char buf[4096];
        ssize_t n;
        while ((n = ::recv(sockfd, buf, sizeof(buf), 0)) > 0) {
            raw.append(buf, n);
        }
        ::close(sockfd);

        // 解析 status line + body
        auto headerEnd = raw.find("\r\n\r\n");
        if (headerEnd == std::string::npos) return resp;
        std::string statusLine = raw.substr(0, raw.find("\r\n"));
        if (std::sscanf(statusLine.c_str(), "HTTP/1.%*d %d", &resp.statusCode) != 1) {
            return resp;
        }
        resp.body = raw.substr(headerEnd + 4);
        resp.success = true;
        return resp;
    }

    static std::string urlEncode(const std::string& s) {
        std::string out;
        char buf[4];
        for (unsigned char c : s) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                out += c;
            } else {
                std::snprintf(buf, sizeof(buf), "%%%02X", c);
                out += buf;
            }
        }
        return out;
    }

    std::vector<std::string> nodes_;
    std::atomic<size_t> nextIdx_;
};
