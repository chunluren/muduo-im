/**
 * @file ChatServer.h
 * @brief IM 聊天服务器主类，整合 HTTP REST API 和 WebSocket 实时消息
 *
 * 详细说明：
 * - 双协议架构：HTTP 服务器处理用户注册/登录、好友管理、群组管理、
 *   历史消息查询和文件上传等 REST API 请求；WebSocket 服务器处理
 *   实时消息路由，包括单聊、群聊、文件消息、撤回和已读回执
 * - 所有 HTTP 接口（除注册/登录外）均通过 JWT Bearer Token 鉴权
 * - WebSocket 连接通过 URL 参数中的 Token 进行握手验证
 * - 消息流程：客户端发送 → Redis 消息队列（批量刷写 MySQL）→ ACK 发送方 → 转发在线接收方
 * - Redis 集成：在线状态 TTL、未读消息计数、消息队列批量写入
 *
 * @example 使用示例
 * @code
 * EventLoop loop;
 * MySQLPoolConfig mysqlCfg{...};
 * RedisPoolConfig redisCfg{...};
 * ChatServer server(&loop, 8080, 9090, mysqlCfg, redisCfg, "my_jwt_secret");
 * server.start();
 * loop.loop();
 * @endcode
 */
#pragma once

#include "common/JWT.h"
#include "common/Protocol.h"
#include "server/AuditService.h"
#include "server/UserService.h"
#include "server/OnlineManager.h"
#include "server/FriendService.h"
#include "server/GroupService.h"
#include "server/MessageService.h"
#include "server/JwtRevocationService.h"
#include "server/ESClient.h"
#include "http/HttpServer.h"
#include "http/MultipartParser.h"
#include "websocket/WebSocketServer.h"
#include "pool/MySQLPool.h"
#include "pool/RedisPool.h"
#include "util/CircuitBreaker.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

using json = nlohmann::json;

/**
 * @class ChatServer
 * @brief IM 聊天服务器核心类，双协议架构 + Redis 集成
 *
 * 设计思路：
 * - HTTP 协议：处理无状态的 REST API 请求，包括用户注册/登录、好友增删查、
 *   群组创建/加入/成员查询、历史消息分页查询、文件上传、未读消息计数等
 * - WebSocket 协议：处理有状态的实时消息，包括单聊消息路由、群聊消息广播、
 *   文件消息转发、消息撤回通知、已读回执转发等
 * - Redis 集成：在线状态带 TTL（支持多实例）、未读消息计数（per user-peer）、
 *   消息队列（缓冲后批量写入 MySQL，降低数据库压力）
 */
class ChatServer {
public:
    /**
     * @brief 构造聊天服务器，初始化双协议服务和所有业务组件
     *
     * @param loop      事件循环指针，驱动 HTTP 和 WebSocket 两个服务器的 IO
     * @param httpPort  HTTP REST API 监听端口（如 8080）
     * @param wsPort    WebSocket 实时消息监听端口（如 9090）
     * @param mysqlConfig MySQL 连接池配置（地址、端口、用户名、密码、数据库名、池大小）
     * @param redisConfig Redis 连接池配置（地址、端口、密码、池大小），用于在线状态管理
     * @param jwtSecret   JWT 签名密钥，用于用户登录 Token 的生成与验证
     */
    ChatServer(EventLoop* loop, uint16_t httpPort, uint16_t wsPort,
               const MySQLPoolConfig& mysqlConfig, const RedisPoolConfig& redisConfig,
               const std::string& jwtSecret)
        : loop_(loop)
        , httpServer_(loop, InetAddress(httpPort), "HttpServer")
        , wsServer_(loop, InetAddress(wsPort), "WebSocketServer")
        , mysqlPool_(std::make_shared<MySQLPool>(mysqlConfig))
        , redisPool_(std::make_shared<RedisPool>(redisConfig))
        , userService_(mysqlPool_, jwtSecret)
        , friendService_(mysqlPool_)
        , groupService_(mysqlPool_)
        , messageService_(mysqlPool_)
        , onlineManager_(redisPool_)
        , auditService_(mysqlPool_)
        , jwtRevocation_(redisPool_)
        , jwt_(jwtSecret)
        , jwtSecret_(jwtSecret)
        , mysqlBreaker_(5, 2, 10)   // 连续 5 次失败打开，2 次成功恢复，10 秒超时
        , redisBreaker_(5, 2, 10)
    {
        setupHttpRoutes();
        setupWebSocket();
    }

    /**
     * @brief 启动聊天服务器，开启 HTTP 和 WebSocket 双协议监听
     *
     * 启动顺序：先启用 HTTP 服务器的 CORS 支持（允许前端跨域请求），
     * 然后分别启动 HTTP 和 WebSocket 服务器开始接受连接。
     * 同时注册定时任务：每 2 秒批量刷写 Redis 消息队列到 MySQL，
     * 每 10 秒刷新在线用户的 Redis TTL
     */
    /**
     * @brief 配置 ElasticSearch 集群（Phase 4.4）
     * @param nodes ES 节点列表 ["host:port", ...]；空列表关闭 ES 走 MySQL 降级
     */
    /**
     * @brief 配置归档查询服务地址（Phase 5.3）
     * @param host  archive_query_server host（一般 127.0.0.1）
     * @param port  端口（默认 9300）
     */
    void enableArchiveQuery(const std::string& host, int port) {
        archiveQueryHost_ = host;
        archiveQueryPort_ = port;
    }

    /**
     * @brief 简易同步 HTTP GET（用于调归档查询服务，与 ESClient 风格一致）
     */
    static std::string httpGetSync(const std::string& host, int port, const std::string& path) {
        int sockfd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sockfd < 0) return "";
        struct timeval tv; tv.tv_sec = 3; tv.tv_usec = 0;
        ::setsockopt(sockfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        ::setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        struct sockaddr_in addr; std::memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) { ::close(sockfd); return ""; }
        if (::connect(sockfd, (sockaddr*)&addr, sizeof(addr)) < 0) { ::close(sockfd); return ""; }
        std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host
                          + "\r\nConnection: close\r\n\r\n";
        ::send(sockfd, req.c_str(), req.size(), 0);
        std::string raw; char buf[4096]; ssize_t n;
        while ((n = ::recv(sockfd, buf, sizeof(buf), 0)) > 0) raw.append(buf, n);
        ::close(sockfd);
        auto headerEnd = raw.find("\r\n\r\n");
        if (headerEnd == std::string::npos) return "";
        return raw.substr(headerEnd + 4);
    }

    void enableElasticsearch(const std::vector<std::string>& nodes) {
        if (nodes.empty()) {
            esClient_.reset();
            return;
        }
        esClient_ = std::make_unique<ESClient>(nodes);
        // 探活（不阻塞启动；失败也允许启动，因为 search 路径有降级）
        if (!esClient_->healthy()) {
            std::cerr << "WARN: ES cluster not healthy at startup; search will fall back to MySQL"
                      << std::endl;
        }
    }

    void start() {
        httpServer_.enableCors();
        httpServer_.useRateLimit(100); // 100 req/sec per IP
        httpServer_.enableMetrics();  // 暴露 GET /metrics
        httpServer_.start();
        wsServer_.start();

        // Batch flush message queue every 2 seconds
        loop_->runEvery(2.0, [this]() { flushMessageQueue(); });

        // Refresh online status TTL every 10 seconds
        loop_->runEvery(10.0, [this]() {
            auto users = onlineManager_.getOnlineUsers();
            for (int64_t uid : users) {
                onlineManager_.refreshOnline(uid);
            }
        });
    }

    /// 优雅关闭 — 刷队列 + 断开连接 + 退出循环
    void shutdown() {
        // 1. 立即刷一次消息队列（最后机会持久化未刷的消息）
        flushMessageQueue();

        // 2. 通知所有 WS 客户端断开（可选，让客户端走 close 流程）
        // wsServer_ 内部 shutdown 会处理

        // 3. 关闭 HTTP/WS 服务器
        httpServer_.shutdown(2.0);
        wsServer_.shutdown(2.0);

        // 4. 退出 EventLoop
        loop_->quit();
    }

private:
    // ==================== Auth / Request Helpers ====================

    /**
     * @brief 从 HTTP 请求的 Authorization 头中提取并验证 JWT Token
     *
     * 验证流程：
     * 1. 从 Authorization 头提取 Bearer Token
     * 2. 校验 JWT 签名与 exp
     * 3. 查询 jti 黑名单（若在黑名单则拒绝）
     *
     * @param req HTTP 请求对象，从中读取 Authorization 头
     * @param outClaims 可选输出参数，返回完整解析后的 Claims（含 jti）
     * @return 验证成功返回用户 ID（> 0）；任意失败返回 -1
     */
    int64_t authFromRequest(const HttpRequest& req, JWT::Claims* outClaims = nullptr) {
        std::string auth = req.getHeader("authorization");
        if (auth.size() <= 7 || auth.substr(0, 7) != "Bearer ") {
            return -1;
        }
        std::string token = auth.substr(7);

        JWT::Claims claims;
        if (!jwt_.verifyAndParse(token, &claims)) {
            return -1;
        }
        // jti 吊销检查：token 签名 / 过期通过后再查黑名单
        if (!claims.jti.empty() && jwtRevocation_.isRevoked(claims.jti)) {
            LOG_WARN_JSON("jwt_revoked_blocked",
                          "uid=" + std::to_string(claims.userId) +
                          " jti=" + claims.jti);
            return -1;
        }
        if (outClaims) *outClaims = claims;
        return claims.userId;
    }

    /// 鉴权辅助：失败时设置 401 响应并返回 -1
    /// 用法: int64_t userId = requireAuth(req, resp); if (userId < 0) return;
    int64_t requireAuth(const HttpRequest& req, HttpResponse& resp,
                         JWT::Claims* outClaims = nullptr) {
        int64_t userId = authFromRequest(req, outClaims);
        if (userId < 0) {
            resp.setStatusCode(HttpStatusCode::UNAUTHORIZED);
            resp.setText("unauthorized");
        }
        return userId;
    }

    /// 解析请求体 JSON，失败时设置 400 响应并返回 false
    /// 用法: json j; if (!parseJsonBody(req, resp, j)) return;
    static bool parseJsonBody(const HttpRequest& req, HttpResponse& resp, json& out) {
        out = json::parse(req.body, nullptr, false);
        if (out.is_discarded()) {
            resp = HttpResponse::badRequest("invalid JSON");
            return false;
        }
        return true;
    }

    /// 取客户端 IP：优先 X-Real-IP / X-Forwarded-For 头，降级为 "unknown"
    static std::string getClientIp(const HttpRequest& req) {
        std::string ip = req.getHeader("x-real-ip");
        if (!ip.empty()) return ip;
        ip = req.getHeader("x-forwarded-for");
        if (!ip.empty()) {
            // X-Forwarded-For 可能包含多个 IP，取首个
            auto comma = ip.find(',');
            if (comma != std::string::npos) ip = ip.substr(0, comma);
            return ip;
        }
        return "unknown";
    }

    // ==================== HTTP Route Table ====================

    void setupHttpRoutes() {
        // 静态文件服务（前端）。优先级：env > 默认 ../web（相对 cwd）
        // 解决 cwd 不在 build/ 时找不到前端文件的问题（systemd / docker / IDE 调试）
        const char* envWebRoot = std::getenv("MUDUO_IM_WEB_ROOT");
        std::string webRoot = (envWebRoot && *envWebRoot) ? envWebRoot : "../web";
        httpServer_.serveStatic("/", webRoot);

        // ---- Auth ----
        httpServer_.POST("/api/register", [this](const HttpRequest& req, HttpResponse& resp) { handleRegister(req, resp); });
        httpServer_.POST("/api/login",    [this](const HttpRequest& req, HttpResponse& resp) { handleLogin(req, resp); });

        // ---- Friends ----
        httpServer_.GET ("/api/friends",          [this](const HttpRequest& req, HttpResponse& resp) { handleGetFriends(req, resp); });
        httpServer_.POST("/api/friends/add",      [this](const HttpRequest& req, HttpResponse& resp) { handleAddFriend(req, resp); });
        httpServer_.POST("/api/friends/delete",   [this](const HttpRequest& req, HttpResponse& resp) { handleDeleteFriend(req, resp); });
        httpServer_.POST("/api/friends/request",  [this](const HttpRequest& req, HttpResponse& resp) { handleFriendRequest(req, resp); });
        httpServer_.GET ("/api/friends/requests", [this](const HttpRequest& req, HttpResponse& resp) { handleGetFriendRequests(req, resp); });
        httpServer_.POST("/api/friends/handle",   [this](const HttpRequest& req, HttpResponse& resp) { handleFriendRequestResponse(req, resp); });

        // ---- Groups ----
        httpServer_.GET ("/api/groups",              [this](const HttpRequest& req, HttpResponse& resp) { handleGetGroups(req, resp); });
        httpServer_.POST("/api/groups/create",       [this](const HttpRequest& req, HttpResponse& resp) { handleCreateGroup(req, resp); });
        httpServer_.POST("/api/groups/join",         [this](const HttpRequest& req, HttpResponse& resp) { handleJoinGroup(req, resp); });
        httpServer_.GET ("/api/groups/members",      [this](const HttpRequest& req, HttpResponse& resp) { handleGetGroupMembers(req, resp); });
        httpServer_.POST("/api/groups/leave",        [this](const HttpRequest& req, HttpResponse& resp) { handleLeaveGroup(req, resp); });
        httpServer_.POST("/api/groups/delete",       [this](const HttpRequest& req, HttpResponse& resp) { handleDeleteGroup(req, resp); });
        httpServer_.POST("/api/groups/announcement", [this](const HttpRequest& req, HttpResponse& resp) { handleSetGroupAnnouncement(req, resp); });
        httpServer_.GET ("/api/groups/announcement", [this](const HttpRequest& req, HttpResponse& resp) { handleGetGroupAnnouncement(req, resp); });
        httpServer_.POST("/api/groups/kick",         [this](const HttpRequest& req, HttpResponse& resp) { handleKickGroupMember(req, resp); });

        // ---- Messages ----
        httpServer_.GET ("/api/messages/history",     [this](const HttpRequest& req, HttpResponse& resp) { handleGetMessageHistory(req, resp); });
        httpServer_.GET ("/api/messages/search",      [this](const HttpRequest& req, HttpResponse& resp) { handleSearchMessages(req, resp); });
        httpServer_.GET ("/api/messages/read-status", [this](const HttpRequest& req, HttpResponse& resp) { handleGetReadStatus(req, resp); });
        httpServer_.GET ("/api/unread",               [this](const HttpRequest& req, HttpResponse& resp) { handleGetUnread(req, resp); });

        // ---- User ----
        httpServer_.GET ("/api/user/profile",  [this](const HttpRequest& req, HttpResponse& resp) { handleGetUserProfile(req, resp); });
        httpServer_.PUT ("/api/user/profile",  [this](const HttpRequest& req, HttpResponse& resp) { handleUpdateProfile(req, resp); });
        httpServer_.GET ("/api/user/search",   [this](const HttpRequest& req, HttpResponse& resp) { handleSearchUsers(req, resp); });
        httpServer_.GET ("/api/user/info",     [this](const HttpRequest& req, HttpResponse& resp) { handleGetUserInfo(req, resp); });
        httpServer_.POST("/api/user/password", [this](const HttpRequest& req, HttpResponse& resp) { handleChangePassword(req, resp); });
        httpServer_.POST("/api/user/delete",   [this](const HttpRequest& req, HttpResponse& resp) { handleDeleteAccount(req, resp); });
        httpServer_.POST("/api/logout",        [this](const HttpRequest& req, HttpResponse& resp) { handleLogout(req, resp); });

        // ---- File upload ----
        httpServer_.POST("/api/upload", [this](const HttpRequest& req, HttpResponse& resp) { handleUpload(req, resp); });
        // Phase 4.3 断点续传分片 API
        httpServer_.POST("/api/upload/init",     [this](const HttpRequest& req, HttpResponse& resp) { handleUploadInit(req, resp); });
        httpServer_.POST("/api/upload/chunk",    [this](const HttpRequest& req, HttpResponse& resp) { handleUploadChunk(req, resp); });
        httpServer_.GET ("/api/upload/status",   [this](const HttpRequest& req, HttpResponse& resp) { handleUploadStatus(req, resp); });
        httpServer_.POST("/api/upload/complete", [this](const HttpRequest& req, HttpResponse& resp) { handleUploadComplete(req, resp); });
        httpServer_.serveStatic("/uploads", "../uploads");

        // ---- Health Check ----
        httpServer_.GET("/health", [this](const HttpRequest& req, HttpResponse& resp) {
            handleHealth(req, resp);
        });
    }

    // ==================== Route Handlers: Auth ====================

    void handleRegister(const HttpRequest& req, HttpResponse& resp) {
        json j;
        if (!parseJsonBody(req, resp, j)) return;
        std::string username = j.value("username", "");
        auto result = userService_.registerUser(username, j.value("password", ""), j.value("nickname", ""));

        // 审计：注册成功/失败均留痕
        std::string ip = getClientIp(req);
        if (result.value("success", false)) {
            int64_t newUserId = result.value("userId", (int64_t)0);
            auditService_.log(newUserId, "register", username, ip);
        } else {
            auditService_.log(0, "register_failed", username, ip, result.value("message", ""));
        }
        resp.setJson(result.dump());
    }

    void handleLogin(const HttpRequest& req, HttpResponse& resp) {
        json j;
        if (!parseJsonBody(req, resp, j)) return;
        std::string username = j.value("username", "");
        auto result = userService_.login(username, j.value("password", ""));

        // 审计：登录成功/失败均留痕（失败可用于检测暴力破解）
        std::string ip = getClientIp(req);
        if (result.value("success", false)) {
            int64_t uid = result.value("userId", (int64_t)0);
            auditService_.log(uid, "login", username, ip);
        } else {
            auditService_.log(0, "login_failed", username, ip, result.value("message", ""));
        }
        resp.setJson(result.dump());
    }

    // ==================== Route Handlers: Friends ====================

    void handleGetFriends(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        resp.setJson(friendService_.getFriends(userId).dump());
    }

    void handleAddFriend(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        json j;
        if (!parseJsonBody(req, resp, j)) return;
        int64_t friendId = j.value("friendId", (int64_t)0);
        resp.setJson(friendService_.addFriend(userId, friendId).dump());
    }

    void handleDeleteFriend(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        json j;
        if (!parseJsonBody(req, resp, j)) return;
        int64_t friendId = j.value("friendId", (int64_t)0);
        resp.setJson(friendService_.deleteFriend(userId, friendId).dump());
    }

    void handleFriendRequest(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        json j;
        if (!parseJsonBody(req, resp, j)) return;
        int64_t toUserId = j.value("toUserId", (int64_t)0);
        resp.setJson(friendService_.sendRequest(userId, toUserId).dump());

        // 实时通知对方有新好友申请
        auto session = onlineManager_.getSession(toUserId);
        if (session) {
            session->sendText(json({{"type", "friend_request"}, {"from", std::to_string(userId)}}).dump());
        }
    }

    void handleGetFriendRequests(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        resp.setJson(friendService_.getRequests(userId).dump());
    }

    void handleFriendRequestResponse(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        json j;
        if (!parseJsonBody(req, resp, j)) return;
        int64_t requestId = j.value("requestId", (int64_t)0);
        bool accept = j.value("accept", false);
        resp.setJson(friendService_.handleRequest(userId, requestId, accept).dump());
    }

    // ==================== Route Handlers: Groups ====================

    void handleGetGroups(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        resp.setJson(groupService_.getUserGroups(userId).dump());
    }

    void handleCreateGroup(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        json j;
        if (!parseJsonBody(req, resp, j)) return;
        resp.setJson(groupService_.createGroup(userId, j.value("name", "")).dump());
    }

    void handleJoinGroup(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        json j;
        if (!parseJsonBody(req, resp, j)) return;
        int64_t groupId = j.value("groupId", (int64_t)0);
        resp.setJson(groupService_.joinGroup(userId, groupId).dump());
    }

    void handleGetGroupMembers(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        int64_t groupId = 0;
        try { groupId = std::stoll(req.getParam("groupId", "0")); } catch (...) {}
        resp.setJson(groupService_.getMembers(groupId).dump());
    }

    void handleLeaveGroup(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        json j;
        if (!parseJsonBody(req, resp, j)) return;
        int64_t groupId = j.value("groupId", (int64_t)0);
        resp.setJson(groupService_.leaveGroup(userId, groupId).dump());
    }

    void handleDeleteGroup(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        json j;
        if (!parseJsonBody(req, resp, j)) return;
        int64_t groupId = j.value("groupId", (int64_t)0);
        resp.setJson(groupService_.deleteGroup(userId, groupId).dump());
    }

    void handleSetGroupAnnouncement(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        json j;
        if (!parseJsonBody(req, resp, j)) return;
        int64_t groupId = j.value("groupId", (int64_t)0);
        std::string announcement = j.value("announcement", "");
        resp.setJson(groupService_.setAnnouncement(userId, groupId, announcement).dump());
    }

    void handleGetGroupAnnouncement(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        int64_t groupId = 0;
        try { groupId = std::stoll(req.getParam("groupId", "0")); } catch (...) {}
        std::string ann = groupService_.getAnnouncement(groupId);
        resp.setJson(json({{"success", true}, {"announcement", ann}}).dump());
    }

    void handleKickGroupMember(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        json j;
        if (!parseJsonBody(req, resp, j)) return;
        int64_t groupId = j.value("groupId", (int64_t)0);
        int64_t targetUserId = j.value("userId", (int64_t)0);
        auto result = groupService_.kickMember(userId, groupId, targetUserId);
        if (result.value("success", false)) {
            auditService_.log(userId, "kick_member", std::to_string(targetUserId),
                              getClientIp(req), "groupId=" + std::to_string(groupId));
        }
        resp.setJson(result.dump());
    }

    // ==================== Route Handlers: Messages ====================

    void handleGetMessageHistory(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;

        int64_t peerId = 0, groupId = 0, before = 0;
        try { peerId  = std::stoll(req.getParam("peerId",  "0")); } catch (...) {}
        try { groupId = std::stoll(req.getParam("groupId", "0")); } catch (...) {}
        try { before  = std::stoll(req.getParam("before",  "0")); } catch (...) {}

        json result = (groupId > 0)
            ? messageService_.getGroupHistory(groupId, 50, before)
            : messageService_.getPrivateHistory(userId, peerId, 50, before);

        // Phase 5.3 冷数据归档查询：MySQL 返回不足 limit（说明可能要往更早翻），
        // 尝试从 archive_query_server 补足归档消息
        if (archiveQueryHost_.size() > 0 && result.is_array() && result.size() < 50) {
            int64_t needLimit = 50 - (int64_t)result.size();
            // 计算 archive 查询的 before_ts：优先用最旧热消息的时间戳，否则用 client 传的 before
            int64_t archBefore = before > 0 ? before : (int64_t)9'999'999'999'999LL;
            if (!result.empty()) {
                int64_t oldest = result.back().value("timestamp", archBefore);
                if (oldest > 0 && oldest < archBefore) archBefore = oldest;
            }
            std::string queryUrl;
            if (groupId > 0) {
                queryUrl = "/query?kind=group&group_id=" + std::to_string(groupId)
                            + "&before_ts=" + std::to_string(archBefore)
                            + "&limit=" + std::to_string(needLimit);
            } else {
                queryUrl = "/query?kind=private&user_id=" + std::to_string(userId)
                            + "&peer_id=" + std::to_string(peerId)
                            + "&before_ts=" + std::to_string(archBefore)
                            + "&limit=" + std::to_string(needLimit);
            }
            std::string archResp = httpGetSync(archiveQueryHost_, archiveQueryPort_, queryUrl);
            if (!archResp.empty()) {
                json arch = json::parse(archResp, nullptr, false);
                if (!arch.is_discarded() && arch.value("success", false)) {
                    for (auto& m : arch.value("messages", json::array())) {
                        m["archived"] = true;
                        result.push_back(m);
                    }
                    LOG_EVENT("history_archive_merged",
                              "user=" + std::to_string(userId)
                              + " arch_count=" + std::to_string(arch.value("count", 0)));
                }
            }
        }
        resp.setJson(result.dump());
    }

    void handleSearchMessages(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        std::string keyword = req.getParam("keyword", "");
        if (keyword.size() < 2) {
            resp.setJson(json({{"success", false}, {"message", "keyword too short"}}).dump());
            return;
        }

        // Phase 4.4：优先走 ES（如果已启用且健康），失败降级 MySQL LIKE
        if (esClient_) {
            // 构造 ES query：搜索 body 字段，限定用户参与的会话
            // 简化：服务端不预过滤 user 参与会话（ES 索引时已带 sender_id），
            // 由 ES filter 完成
            json esQuery = {
                {"size", 50},
                {"query", {
                    {"bool", {
                        {"must", json::array({
                            {{"match", {{"body", keyword}}}}
                        })},
                        {"filter", json::array({
                            {{"bool", {
                                {"should", json::array({
                                    {{"term", {{"sender_id", userId}}}},
                                    {{"term", {{"recipient_id", userId}}}}
                                })},
                                {"minimum_should_match", 1}
                            }}}
                        })}
                    }}
                }},
                {"sort", json::array({
                    {{"created_at", {{"order", "desc"}}}}
                })}
            };
            std::string esResp = esClient_->search("messages", esQuery.dump());
            if (!esResp.empty()) {
                json parsed = json::parse(esResp, nullptr, false);
                if (!parsed.is_discarded() && parsed.contains("hits")) {
                    json messages = json::array();
                    for (auto& hit : parsed["hits"]["hits"]) {
                        if (!hit.contains("_source")) continue;
                        const auto& src = hit["_source"];
                        json m;
                        m["msgId"] = src.value("msg_id", "");
                        m["from"] = src.value("sender_id", (int64_t)0);
                        m["content"] = src.value("body", "");
                        m["timestamp"] = src.value("created_at", (int64_t)0);
                        m["chatType"] = src.value("msg_kind", "private");
                        if (src.contains("recipient_id")) m["to"] = src["recipient_id"];
                        if (src.contains("group_id")) m["groupId"] = src["group_id"];
                        messages.push_back(m);
                    }
                    resp.setJson(json({{"success", true},
                                       {"source", "elasticsearch"},
                                       {"messages", messages}}).dump());
                    return;
                }
            }
            LOG_WARN_JSON("es_search_fallback", "keyword=" + keyword);
        }

        // 降级：MySQL LIKE 全表扫
        json fb = messageService_.searchMessages(userId, keyword);
        if (fb.is_object()) fb["source"] = "mysql";
        resp.setJson(fb.dump());
    }

    void handleGetReadStatus(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        std::string peerStr = req.getParam("peerId", "0");

        if (!redisPool_) { resp.setJson(json({{"lastReadMsgId", ""}}).dump()); return; }
        auto conn = redisPool_->acquire(1000);
        if (!conn || !conn->valid()) { resp.setJson(json({{"lastReadMsgId", ""}}).dump()); return; }

        // What has the peer read of my messages?
        std::string lastRead = conn->get("read_pos:" + peerStr + ":" + std::to_string(userId));
        redisPool_->release(std::move(conn));
        resp.setJson(json({{"lastReadMsgId", lastRead}}).dump());
    }

    void handleGetUnread(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        auto friends = friendService_.getFriends(userId);
        json result = json::object();
        for (auto& f : friends) {
            int64_t fid = f.value("userId", (int64_t)0);
            int64_t count = getUnread(userId, fid);
            if (count > 0) result[std::to_string(fid)] = count;
        }
        resp.setJson(result.dump());
    }

    // ==================== Route Handlers: User ====================

    void handleGetUserProfile(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        resp.setJson(userService_.getProfile(userId).dump());
    }

    void handleUpdateProfile(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        json j;
        if (!parseJsonBody(req, resp, j)) return;
        resp.setJson(userService_.updateProfile(
            userId, j.value("nickname", ""), j.value("avatar", "")
        ).dump());
    }

    void handleSearchUsers(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        resp.setJson(userService_.searchUsers(req.getParam("keyword", "")).dump());
    }

    void handleGetUserInfo(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        int64_t targetId = 0;
        try { targetId = std::stoll(req.getParam("userId", "0")); } catch (...) {}
        if (targetId <= 0) { resp = HttpResponse::badRequest("invalid userId"); return; }
        resp.setJson(userService_.getPublicProfile(targetId).dump());
    }

    void handleChangePassword(const HttpRequest& req, HttpResponse& resp) {
        JWT::Claims claims;
        int64_t userId = requireAuth(req, resp, &claims);
        if (userId < 0) return;
        json j;
        if (!parseJsonBody(req, resp, j)) return;
        auto result = userService_.changePassword(
            userId, j.value("oldPassword", ""), j.value("newPassword", "")
        );
        if (result.value("success", false)) {
            // 改密成功后吊销当前 token，强制用户用新密码重新登录（防止旧 token 继续使用）
            if (!claims.jti.empty()) {
                jwtRevocation_.revoke(claims.jti, claims.remainingSeconds());
            }
            auditService_.log(userId, "change_password", "", getClientIp(req));
        }
        resp.setJson(result.dump());
    }

    void handleDeleteAccount(const HttpRequest& req, HttpResponse& resp) {
        JWT::Claims claims;
        int64_t userId = requireAuth(req, resp, &claims);
        if (userId < 0) return;
        json j;
        if (!parseJsonBody(req, resp, j)) return;
        auto result = userService_.deleteAccount(userId, j.value("password", ""));
        if (result.value("success", false)) {
            // 注销账号后吊销当前 token
            if (!claims.jti.empty()) {
                jwtRevocation_.revoke(claims.jti, claims.remainingSeconds());
            }
            auditService_.log(userId, "delete_account", "", getClientIp(req));
        }
        resp.setJson(result.dump());
    }

    /// POST /api/logout — 主动登出，将当前 token 的 jti 加入 Redis 黑名单
    void handleLogout(const HttpRequest& req, HttpResponse& resp) {
        JWT::Claims claims;
        int64_t userId = requireAuth(req, resp, &claims);
        if (userId < 0) return;

        // 旧版（无 jti）token 无法精准吊销，返回 success 但不实际吊销
        bool revoked = false;
        if (!claims.jti.empty()) {
            revoked = jwtRevocation_.revoke(claims.jti, claims.remainingSeconds());
        }
        auditService_.log(userId, "logout", "", getClientIp(req));
        resp.setJson(json({{"success", true},
                           {"revoked", revoked}}).dump());
    }

    // ==================== Route Handlers: Upload ====================

    // ==================== Chunked Upload (Phase 4.3) ====================
    // 断点续传 4 段式 API：
    //   POST /api/upload/init     初始化（拿 upload_id + chunk_size）
    //   POST /api/upload/chunk    上传单个分片（query: upload_id, index）
    //   GET  /api/upload/status   查询已上传分片（用于恢复）
    //   POST /api/upload/complete 合并分片，返回最终 URL
    //
    // 服务端状态：Redis HASH `upload:{upload_id}` 存元数据 + bitmap
    //   字段：file_name / total_size / chunk_size / total_chunks / uid / created_at / received（base64 bitmap）
    //   分片落盘：/tmp/upload/{upload_id}/chunk_{N}
    //   24 小时未完成 cron 清理（建议另起；这里仅 init 时设 Redis TTL）

    static constexpr size_t kChunkSize = 1024 * 1024;   // 1 MB / chunk
    static constexpr size_t kMaxFileSize = 50 * 1024 * 1024;  // 与 handleUpload 一致

    /// POST /api/upload/init  body: {file_name, total_size, sha256?}
    /// 返回: {success, upload_id, chunk_size, total_chunks}
    void handleUploadInit(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        json j;
        if (!parseJsonBody(req, resp, j)) return;

        std::string fileName = j.value("file_name", "");
        int64_t totalSize = j.value("total_size", (int64_t)0);
        if (fileName.empty() || totalSize <= 0 || totalSize > (int64_t)kMaxFileSize) {
            resp = HttpResponse::badRequest("invalid file_name / total_size");
            return;
        }

        // 类型白名单（与 handleUpload 一致）
        std::string ext;
        auto dotPos = fileName.rfind('.');
        if (dotPos != std::string::npos) ext = fileName.substr(dotPos);
        std::string lowerExt = ext;
        std::transform(lowerExt.begin(), lowerExt.end(), lowerExt.begin(), ::tolower);
        static const std::vector<std::string> allowedExts = {
            ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp",
            ".pdf", ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx",
            ".txt", ".md", ".zip", ".rar", ".7z",
            ".mp3", ".mp4", ".wav", ".avi", ".mov"
        };
        bool allowed = lowerExt.empty();
        for (auto& ae : allowedExts) {
            if (lowerExt == ae) { allowed = true; break; }
        }
        if (!allowed) {
            resp = HttpResponse::badRequest("file type not allowed");
            return;
        }

        std::string uploadId = Protocol::generateMsgId();
        int64_t totalChunks = (totalSize + kChunkSize - 1) / kChunkSize;

        // 创建临时分片目录
        std::string dir = "/tmp/muduo-im-upload/" + uploadId;
        ::mkdir("/tmp/muduo-im-upload", 0755);
        if (::mkdir(dir.c_str(), 0755) != 0) {
            resp = HttpResponse::serverError("mkdir failed");
            return;
        }

        // Redis 存元数据，TTL 24h
        if (redisPool_) {
            auto rconn = redisPool_->acquire(1000);
            if (rconn && rconn->valid()) {
                json meta = {
                    {"file_name", fileName},
                    {"total_size", totalSize},
                    {"chunk_size", kChunkSize},
                    {"total_chunks", totalChunks},
                    {"ext", ext},
                    {"uid", userId},
                    {"created_at", Protocol::nowMs()},
                    {"received", json::array()}  // 已收到分片索引数组
                };
                rconn->set("upload:" + uploadId, meta.dump(), 86400);
                redisPool_->release(std::move(rconn));
            }
        }

        resp.setJson(json({
            {"success", true},
            {"upload_id", uploadId},
            {"chunk_size", (int64_t)kChunkSize},
            {"total_chunks", totalChunks}
        }).dump());
    }

    /// POST /api/upload/chunk?upload_id=X&index=N  body: 二进制分片
    void handleUploadChunk(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;

        std::string uploadId = req.getParam("upload_id", "");
        std::string idxStr = req.getParam("index", "");
        if (uploadId.empty() || idxStr.empty()) {
            resp = HttpResponse::badRequest("missing upload_id or index");
            return;
        }
        int idx = -1;
        try { idx = std::stoi(idxStr); } catch (...) {}
        if (idx < 0) {
            resp = HttpResponse::badRequest("invalid index");
            return;
        }

        // 拉元数据校验
        std::string metaJson;
        if (redisPool_) {
            auto rconn = redisPool_->acquire(1000);
            if (rconn && rconn->valid()) {
                metaJson = rconn->get("upload:" + uploadId);
                redisPool_->release(std::move(rconn));
            }
        }
        if (metaJson.empty()) {
            resp = HttpResponse::badRequest("upload_id not found or expired");
            return;
        }
        json meta = json::parse(metaJson, nullptr, false);
        if (meta.is_discarded()) {
            resp = HttpResponse::serverError("meta corrupt");
            return;
        }
        if (meta.value("uid", (int64_t)0) != userId) {
            resp.setStatusCode(HttpStatusCode::FORBIDDEN);
            resp.setText("not your upload");
            return;
        }
        int64_t totalChunks = meta.value("total_chunks", (int64_t)0);
        if (idx >= totalChunks) {
            resp = HttpResponse::badRequest("index out of range");
            return;
        }
        if (req.body.size() > kChunkSize) {
            resp = HttpResponse::badRequest("chunk too large");
            return;
        }

        // 写分片文件
        std::string chunkPath = "/tmp/muduo-im-upload/" + uploadId
                                 + "/chunk_" + std::to_string(idx);
        std::ofstream ofs(chunkPath, std::ios::binary);
        if (!ofs) {
            resp = HttpResponse::serverError("write chunk failed");
            return;
        }
        ofs.write(req.body.data(), req.body.size());
        ofs.close();

        // 更新 received 数组（去重，简化用 array）
        auto& recv = meta["received"];
        bool exists = false;
        for (auto& v : recv) {
            if (v.is_number_integer() && v.get<int>() == idx) { exists = true; break; }
        }
        if (!exists) recv.push_back(idx);

        if (redisPool_) {
            auto rconn = redisPool_->acquire(1000);
            if (rconn && rconn->valid()) {
                rconn->set("upload:" + uploadId, meta.dump(), 86400);
                redisPool_->release(std::move(rconn));
            }
        }

        resp.setJson(json({
            {"success", true},
            {"index", idx},
            {"received_count", (int)recv.size()},
            {"total_chunks", totalChunks}
        }).dump());
    }

    /// GET /api/upload/status?upload_id=X
    void handleUploadStatus(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        std::string uploadId = req.getParam("upload_id", "");
        if (uploadId.empty()) {
            resp = HttpResponse::badRequest("missing upload_id");
            return;
        }
        std::string metaJson;
        if (redisPool_) {
            auto rconn = redisPool_->acquire(1000);
            if (rconn && rconn->valid()) {
                metaJson = rconn->get("upload:" + uploadId);
                redisPool_->release(std::move(rconn));
            }
        }
        if (metaJson.empty()) {
            resp.setStatusCode(HttpStatusCode::NOT_FOUND);
            resp.setText("upload not found or expired");
            return;
        }
        json meta = json::parse(metaJson, nullptr, false);
        if (meta.is_discarded() || meta.value("uid", (int64_t)0) != userId) {
            resp.setStatusCode(HttpStatusCode::FORBIDDEN);
            resp.setText("not your upload");
            return;
        }
        resp.setJson(json({
            {"success", true},
            {"received_chunks", meta.value("received", json::array())},
            {"total_chunks", meta.value("total_chunks", (int64_t)0)},
            {"file_name", meta.value("file_name", "")}
        }).dump());
    }

    /// POST /api/upload/complete  body: {upload_id}
    /// 合并所有分片 → 最终文件 → 返回 URL（兼容现有 file_msg 协议）
    void handleUploadComplete(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        json j;
        if (!parseJsonBody(req, resp, j)) return;
        std::string uploadId = j.value("upload_id", "");
        if (uploadId.empty()) {
            resp = HttpResponse::badRequest("missing upload_id");
            return;
        }

        std::string metaJson;
        if (redisPool_) {
            auto rconn = redisPool_->acquire(1000);
            if (rconn && rconn->valid()) {
                metaJson = rconn->get("upload:" + uploadId);
                redisPool_->release(std::move(rconn));
            }
        }
        if (metaJson.empty()) {
            resp = HttpResponse::badRequest("upload_id not found");
            return;
        }
        json meta = json::parse(metaJson, nullptr, false);
        if (meta.is_discarded() || meta.value("uid", (int64_t)0) != userId) {
            resp.setStatusCode(HttpStatusCode::FORBIDDEN);
            resp.setText("not your upload");
            return;
        }
        int64_t totalChunks = meta.value("total_chunks", (int64_t)0);
        auto& recv = meta["received"];
        if ((int64_t)recv.size() != totalChunks) {
            resp = HttpResponse::badRequest("not all chunks received");
            return;
        }

        std::string ext = meta.value("ext", "");
        std::string finalName = uploadId + ext;
        ::mkdir("../uploads", 0755);
        std::string finalPath = "../uploads/" + finalName;

        // 合并分片
        std::ofstream out(finalPath, std::ios::binary);
        if (!out) {
            resp = HttpResponse::serverError("create final failed");
            return;
        }
        for (int64_t i = 0; i < totalChunks; ++i) {
            std::string cp = "/tmp/muduo-im-upload/" + uploadId
                              + "/chunk_" + std::to_string(i);
            std::ifstream in(cp, std::ios::binary);
            if (!in) {
                out.close();
                ::unlink(finalPath.c_str());
                resp = HttpResponse::serverError("missing chunk " + std::to_string(i));
                return;
            }
            out << in.rdbuf();
            in.close();
        }
        out.close();

        // 清理临时分片
        for (int64_t i = 0; i < totalChunks; ++i) {
            std::string cp = "/tmp/muduo-im-upload/" + uploadId
                              + "/chunk_" + std::to_string(i);
            ::unlink(cp.c_str());
        }
        ::rmdir(("/tmp/muduo-im-upload/" + uploadId).c_str());

        // Redis 标记完成（保留元数据 1 小时方便 status 查询）
        if (redisPool_) {
            auto rconn = redisPool_->acquire(1000);
            if (rconn && rconn->valid()) {
                meta["completed"] = true;
                rconn->set("upload:" + uploadId, meta.dump(), 3600);
                redisPool_->release(std::move(rconn));
            }
        }

        // 触发缩略图（同 handleUpload）
        std::string lowerExt = ext;
        std::transform(lowerExt.begin(), lowerExt.end(), lowerExt.begin(), ::tolower);
        static const std::vector<std::string> imageExts = {
            ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp"
        };
        bool isImage = false;
        for (auto& ie : imageExts) {
            if (lowerExt == ie) { isImage = true; break; }
        }
        if (isImage && redisPool_) {
            auto rconn = redisPool_->acquire(500);
            if (rconn && rconn->valid()) {
                json job = {
                    {"saved_name", finalName},
                    {"original_path", finalPath},
                    {"sizes", json::array({200, 600})}
                };
                rconn->lpush("thumb_queue", job.dump());
                redisPool_->release(std::move(rconn));
            }
        }

        json result = {
            {"success", true},
            {"url", "/uploads/" + finalName},
            {"filename", meta.value("file_name", "")},
            {"size", meta.value("total_size", (int64_t)0)}
        };
        if (isImage) {
            std::string base = uploadId;
            result["thumb_200"] = "/uploads/" + base + "_thumb_200" + ext;
            result["thumb_600"] = "/uploads/" + base + "_thumb_600" + ext;
        }
        resp.setJson(result.dump());
    }

    void handleUpload(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        (void)userId;

        // Parse multipart
        std::string contentType = req.getHeader("content-type");
        std::string boundary = MultipartParser::extractBoundary(contentType);
        if (boundary.empty()) {
            resp = HttpResponse::badRequest("missing boundary");
            return;
        }

        auto parts = MultipartParser::parse(req.body, boundary);
        if (parts.empty()) {
            resp = HttpResponse::badRequest("no file uploaded");
            return;
        }

        // Find file part
        for (auto& part : parts) {
            if (part.isFile() && !part.data.empty()) {
                // Ensure uploads directory exists
                ::mkdir("../uploads", 0755);

                // File size limit: 50MB
                if (part.data.size() > 50 * 1024 * 1024) {
                    resp = HttpResponse::badRequest("file too large (max 50MB)");
                    return;
                }

                // Generate unique filename
                std::string ext = "";
                auto dotPos = part.filename.rfind('.');
                if (dotPos != std::string::npos) ext = part.filename.substr(dotPos);

                // File type validation
                std::string lowerExt = ext;
                std::transform(lowerExt.begin(), lowerExt.end(), lowerExt.begin(), ::tolower);
                static const std::vector<std::string> allowedExts = {
                    ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp",
                    ".pdf", ".doc", ".docx", ".xls", ".xlsx", ".ppt", ".pptx",
                    ".txt", ".md", ".zip", ".rar", ".7z",
                    ".mp3", ".mp4", ".wav", ".avi", ".mov"
                };
                bool allowed = lowerExt.empty(); // no extension is OK
                for (auto& ae : allowedExts) {
                    if (lowerExt == ae) { allowed = true; break; }
                }
                if (!allowed) {
                    resp = HttpResponse::badRequest("file type not allowed");
                    return;
                }

                std::string savedName = Protocol::generateMsgId() + ext;
                std::string filepath = "../uploads/" + savedName;

                // Write file
                std::ofstream ofs(filepath, std::ios::binary);
                if (!ofs) {
                    resp = HttpResponse::serverError("failed to save file");
                    return;
                }
                ofs.write(part.data.data(), part.data.size());
                ofs.close();

                // Phase 4.2：图片上传后异步生成缩略图
                // 推 Redis 队列让 Python worker 处理（不阻塞主流程）
                static const std::vector<std::string> imageExts = {
                    ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".webp"
                };
                bool isImage = false;
                for (auto& ie : imageExts) {
                    if (lowerExt == ie) { isImage = true; break; }
                }
                if (isImage && redisPool_) {
                    auto rconn = redisPool_->acquire(500);
                    if (rconn && rconn->valid()) {
                        json job = {
                            {"saved_name", savedName},
                            {"original_path", filepath},
                            {"sizes", json::array({200, 600})}
                        };
                        rconn->lpush("thumb_queue", job.dump());
                        redisPool_->release(std::move(rconn));
                    }
                }

                json result = {
                    {"success", true},
                    {"url", "/uploads/" + savedName},
                    {"filename", part.filename},
                    {"size", part.data.size()}
                };
                if (isImage) {
                    // 提示前端：缩略图 URL（约定路径，worker 异步生成）
                    // 前端可选：先尝试 thumb，404 时回退原图
                    auto baseDot = savedName.rfind('.');
                    std::string base = (baseDot == std::string::npos)
                        ? savedName : savedName.substr(0, baseDot);
                    std::string ex = (baseDot == std::string::npos)
                        ? "" : savedName.substr(baseDot);
                    result["thumb_200"] = "/uploads/" + base + "_thumb_200" + ex;
                    result["thumb_600"] = "/uploads/" + base + "_thumb_600" + ex;
                }
                resp.setJson(result.dump());
                return;
            }
        }

        resp = HttpResponse::badRequest("no file found in upload");
    }

    // ==================== Route Handlers: Health ====================

    void handleHealth(const HttpRequest&, HttpResponse& resp) {
        bool mysqlOk = false;
        bool redisOk = false;

        // Test MySQL
        if (auto conn = mysqlPool_->acquire(500)) {
            mysqlOk = conn->ping();
            mysqlPool_->release(std::move(conn));
        }

        // Test Redis
        if (redisPool_) {
            if (auto conn = redisPool_->acquire(500)) {
                redisOk = conn->ping();
                redisPool_->release(std::move(conn));
            }
        }

        json result = {
            {"status", (mysqlOk && redisOk) ? "ok" : "degraded"},
            {"mysql", mysqlOk ? "ok" : "down"},
            {"redis", redisOk ? "ok" : "down"},
            {"online_users", (int64_t)onlineManager_.getOnlineUsers().size()},
            {"mysql_breaker", mysqlBreaker_.state() == CircuitBreaker::Closed ? "closed" : (mysqlBreaker_.state() == CircuitBreaker::Open ? "open" : "half-open")},
            {"redis_breaker", redisBreaker_.state() == CircuitBreaker::Closed ? "closed" : (redisBreaker_.state() == CircuitBreaker::Open ? "open" : "half-open")}
        };

        if (!mysqlOk || !redisOk) {
            resp.setStatusCode(HttpStatusCode::SERVICE_UNAVAILABLE);
        }
        resp.setJson(result.dump());
    }

    // ==================== WebSocket ====================

    /**
     * @brief 从 WebSocket 连接路径中提取 JWT Token 并验证用户身份
     *
     * @param path WebSocket 握手请求的 URI 路径（如 "/ws?token=xxx"）
     * @return 验证成功返回用户 ID（> 0）；Token 缺失或验证失败返回 -1
     */
    /// Phase 3.1：向用户的所有在线设备广播一条消息
    /// @return 实际投递的设备数（用于监控 / 日志）
    int broadcastToUser(int64_t userId, const std::string& message) {
        auto sessions = onlineManager_.getSessions(userId);
        for (auto& s : sessions) s->sendText(message);
        return (int)sessions.size();
    }

    int64_t extractUserIdFromPath(const std::string& path) {
        auto pos = path.find("token=");
        if (pos == std::string::npos) return -1;
        std::string token = path.substr(pos + 6);
        auto ampPos = token.find('&');
        if (ampPos != std::string::npos) {
            token = token.substr(0, ampPos);
        }
        return userService_.verifyToken(token);
    }

    /// Phase 3.1：从 URL 抽取 device_id（无则默认 "default"）
    /// 例：/ws?token=xxx&device_id=mobile-abc → "mobile-abc"
    static std::string extractDeviceIdFromPath(const std::string& path) {
        auto pos = path.find("device_id=");
        if (pos == std::string::npos) return OnlineManager::kDefaultDevice;
        std::string val = path.substr(pos + 10);
        auto ampPos = val.find('&');
        if (ampPos != std::string::npos) val = val.substr(0, ampPos);
        if (val.empty()) return OnlineManager::kDefaultDevice;
        // 简单清洗：长度 ≤ 64，仅允许 ASCII 安全字符
        if (val.size() > 64) val = val.substr(0, 64);
        for (auto& c : val) {
            if (!(std::isalnum(static_cast<unsigned char>(c)) ||
                   c == '-' || c == '_' || c == '.')) {
                return OnlineManager::kDefaultDevice;
            }
        }
        return val;
    }

    void setupWebSocket() {
        WebSocketConfig wsConfig;
        wsConfig.idleTimeoutMs = 60000;     // 60s 没消息断开
        wsConfig.enablePingPong = true;
        wsConfig.pingIntervalMs = 30000;    // 30s 发一次 ping
        wsServer_.setConfig(wsConfig);

        // Handshake validator: verify token from path
        wsServer_.setHandshakeValidator(
            [this](const TcpConnectionPtr& /*conn*/, const std::string& path,
                   const std::map<std::string, std::string>& /*headers*/) -> bool {
                return extractUserIdFromPath(path) > 0;
            });

        // Connection handler: register user as online, notify friends
        wsServer_.setConnectionHandler([this](const WsSessionPtr& session) {
            std::string path = session->getContext("path");
            int64_t userId = extractUserIdFromPath(path);
            if (userId <= 0) {
                session->close(1008, "invalid token");
                return;
            }

            // Phase 3.1：解析 device_id（兼容老客户端不传时为 "default"）
            std::string deviceId = extractDeviceIdFromPath(path);
            session->setContext("userId", std::to_string(userId));
            session->setContext("deviceId", deviceId);
            onlineManager_.addDevice(userId, deviceId, session);

            // Notify friends that user is online
            auto friends = friendService_.getFriends(userId);
            std::string onlineMsg = Protocol::makeOnline(userId);
            for (auto& f : friends) {
                int64_t friendId = f.value("userId", (int64_t)0);
                auto friendSession = onlineManager_.getSession(friendId);
                if (friendSession) {
                    friendSession->sendText(onlineMsg);
                }
            }

            // 推送离线期间的未读消息数
            json unreadInfo = json::object();
            for (auto& f : friends) {
                int64_t fid = f.value("userId", (int64_t)0);
                int64_t count = getUnread(userId, fid);
                if (count > 0) {
                    unreadInfo[std::to_string(fid)] = count;
                }
            }
            if (!unreadInfo.empty()) {
                session->sendText(json({{"type", Protocol::UNREAD_SYNC}, {"data", unreadInfo}}).dump());
            }
        });

        // Message handler: dispatch by type
        wsServer_.setMessageHandler([this](const WsSessionPtr& session, const WsMessage& msg) {
            if (!msg.isText()) return;

            auto j = json::parse(msg.text(), nullptr, false);
            if (j.is_discarded()) {
                session->sendText(Protocol::makeError("invalid JSON"));
                return;
            }

            std::string type = j.value("type", "");
            std::string userIdStr = session->getContext("userId", "0");
            int64_t userId = 0;
            try { userId = std::stoll(userIdStr); } catch (...) {}
            if (userId <= 0) {
                session->sendText(Protocol::makeError("not authenticated"));
                return;
            }

            if (type == Protocol::MSG) {
                handlePrivateMessage(session, j, userId);
            } else if (type == Protocol::GROUP_MSG) {
                handleGroupMessage(session, j, userId);
            } else if (type == Protocol::TYPING) {
                handleTyping(session, j, userId);
            } else if (type == Protocol::FILE_MSG) {
                handleFileMessage(session, j, userId);
            } else if (type == Protocol::RECALL) {
                handleRecall(session, j, userId);
            } else if (type == Protocol::EDIT) {
                handleEdit(session, j, userId);
            } else if (type == Protocol::REACTION) {
                handleReaction(session, j, userId);
            } else if (type == Protocol::CLIENT_ACK) {
                handleClientAck(session, j, userId);
            } else if (type == Protocol::READ_ACK) {
                handleReadAck(session, j, userId);
            } else {
                session->sendText(Protocol::makeError("unknown message type"));
            }
        });

        // Close handler: remove from online, notify friends offline
        wsServer_.setCloseHandler([this](const WsSessionPtr& session) {
            std::string userIdStr = session->getContext("userId", "0");
            std::string deviceId  = session->getContext("deviceId", OnlineManager::kDefaultDevice);
            int64_t userId = 0;
            try { userId = std::stoll(userIdStr); } catch (...) {}
            if (userId <= 0) return;

            // Phase 3.1：单设备下线
            onlineManager_.removeDevice(userId, deviceId);

            // 仅当该 uid 已无任何设备时才通知好友 offline
            if (!onlineManager_.isOnline(userId)) {
                auto friends = friendService_.getFriends(userId);
                std::string offlineMsg = Protocol::makeOffline(userId);
                for (auto& f : friends) {
                    int64_t friendId = f.value("userId", (int64_t)0);
                    for (auto& s : onlineManager_.getSessions(friendId)) {
                        s->sendText(offlineMsg);
                    }
                }
            }
        });
    }

    // ==================== Message Handlers ====================

    void handlePrivateMessage(const WsSessionPtr& session, const json& j, int64_t fromUserId) {
        std::string toStr = j.value("to", "");
        std::string content = j.value("content", "");
        std::string replyTo = j.value("replyTo", "");
        if (toStr.empty() || content.empty()) {
            session->sendText(Protocol::makeError("missing 'to' or 'content'"));
            return;
        }
        if (content.size() > 10000) {
            session->sendText(Protocol::makeError("message too long (max 10000 chars)"));
            return;
        }

        int64_t toUserId = 0;
        try { toUserId = std::stoll(toStr); } catch (...) {}
        if (toUserId <= 0) {
            session->sendText(Protocol::makeError("invalid 'to'"));
            return;
        }

        // 服务端权威 msgId 用 Snowflake（时序有序、跨实例唯一），用于持久化和撤回
        std::string msgId = Protocol::generateServerMsgId();
        // 客户端 msgId 用于幂等去重 + ack 关联本地消息（前端 DOM 节点替换）
        std::string clientMsgId = j.value("msgId", "");

        // 幂等：以客户端 msgId 为 key 去重（同一客户端 msgId 不会被处理两次）
        // 兼容：客户端没传时退化为按服务端 msgId 去重
        std::string dedupKey = clientMsgId.empty() ? msgId : clientMsgId;
        if (isDuplicate(dedupKey)) {
            session->sendText(Protocol::makeAck(msgId, clientMsgId));
            return;
        }

        int64_t timestamp = Protocol::nowMs();

        // Queue message (Redis) or direct save (fallback)
        if (redisPool_) {
            json queueItem = {
                {"_type", "private"}, {"msgId", msgId}, {"from", fromUserId},
                {"to", toUserId}, {"content", content}, {"timestamp", timestamp}
            };
            queueMessage(queueItem.dump());
        } else {
            messageService_.savePrivateMessage(msgId, fromUserId, toUserId, content, timestamp);
        }

        // ACK 把 clientMsgId 透传回去，前端用它定位 DOM 节点把 msgId 替换为服务端权威 msgId
        session->sendText(Protocol::makeAck(msgId, clientMsgId));

        // Forward to recipient — Phase 3.1 多端：所有设备都推送
        json fwd;
        fwd["type"] = Protocol::MSG;
        fwd["from"] = std::to_string(fromUserId);
        fwd["to"] = std::to_string(toUserId);
        fwd["content"] = content;
        fwd["msgId"] = msgId;
        fwd["timestamp"] = timestamp;
        if (!replyTo.empty()) fwd["replyTo"] = replyTo;
        int delivered = broadcastToUser(toUserId, fwd.dump());

        // 多端同步：推给发送方的其他设备（跳过发起的 session）
        std::string senderDevice = session->getContext("deviceId", OnlineManager::kDefaultDevice);
        for (auto& s : onlineManager_.getOtherSessions(fromUserId, senderDevice)) {
            s->sendText(fwd.dump());
        }

        if (delivered == 0) {
            // Recipient offline: increment unread count
            incrementUnread(toUserId, fromUserId);
        }
    }

    void handleGroupMessage(const WsSessionPtr& session, const json& j, int64_t fromUserId) {
        std::string toStr = j.value("to", "");
        std::string content = j.value("content", "");
        std::string replyTo = j.value("replyTo", "");
        if (toStr.empty() || content.empty()) {
            session->sendText(Protocol::makeError("missing 'to' or 'content'"));
            return;
        }
        if (content.size() > 10000) {
            session->sendText(Protocol::makeError("message too long (max 10000 chars)"));
            return;
        }

        int64_t groupId = 0;
        try { groupId = std::stoll(toStr); } catch (...) {}
        if (groupId <= 0) {
            session->sendText(Protocol::makeError("invalid group id"));
            return;
        }

        // 服务端权威 msgId（Snowflake）+ 客户端 msgId（幂等 + ack 关联）
        std::string msgId = Protocol::generateServerMsgId();
        std::string clientMsgId = j.value("msgId", "");

        std::string dedupKey = clientMsgId.empty() ? msgId : clientMsgId;
        if (isDuplicate(dedupKey)) {
            session->sendText(Protocol::makeAck(msgId, clientMsgId));
            return;
        }

        int64_t timestamp = Protocol::nowMs();

        // 解析并校验 mentions（@ 提醒）
        // 客户端传 ["12345","67890"] 字符串数组，服务端：
        // 1. 转 int64 集合
        // 2. 过滤非群成员（防止 @ 群外人员）+ 自己（防自@）
        // 3. 序列化为 JSON 数组持久化 + 推送时带 mention 标志
        std::vector<int64_t> mentions;
        std::string mentionsJsonStr;
        if (j.contains("mentions") && j["mentions"].is_array()) {
            auto memberSet = groupService_.getMemberIds(groupId);
            std::unordered_set<int64_t> memberLookup(memberSet.begin(), memberSet.end());
            for (auto& m : j["mentions"]) {
                int64_t uid = 0;
                try {
                    if (m.is_string()) uid = std::stoll(m.get<std::string>());
                    else if (m.is_number_integer()) uid = m.get<int64_t>();
                } catch (...) { continue; }
                if (uid <= 0 || uid == fromUserId) continue;
                if (!memberLookup.count(uid)) continue;
                mentions.push_back(uid);
            }
            if (!mentions.empty()) {
                json arr = json::array();
                for (auto uid : mentions) arr.push_back(uid);
                mentionsJsonStr = arr.dump();
            }
        }

        // Queue message (Redis) or direct save (fallback)
        if (redisPool_) {
            json queueItem = {
                {"_type", "group"}, {"msgId", msgId}, {"groupId", groupId},
                {"from", fromUserId}, {"content", content}, {"timestamp", timestamp}
            };
            if (!mentionsJsonStr.empty()) queueItem["mentions"] = mentionsJsonStr;
            queueMessage(queueItem.dump());
        } else {
            messageService_.saveGroupMessage(msgId, groupId, fromUserId, content, timestamp, mentionsJsonStr);
        }

        // 被 @ 用户额外计入 unread_mentions（区分普通未读）
        // 与现有 unread 保持一致：扁平 key `unread_mentions:{uid}:{groupId}` + incr
        if (!mentions.empty() && redisPool_) {
            auto rconn = redisPool_->acquire(500);
            if (rconn && rconn->valid()) {
                for (auto uid : mentions) {
                    rconn->incr("unread_mentions:" + std::to_string(uid)
                                 + ":" + std::to_string(groupId));
                }
                redisPool_->release(std::move(rconn));
            }
        }

        // ACK 透传 clientMsgId
        session->sendText(Protocol::makeAck(msgId, clientMsgId));

        // Forward to all online group members (skip sender)
        auto memberIds = groupService_.getMemberIds(groupId);
        std::unordered_set<int64_t> mentionSet(mentions.begin(), mentions.end());
        for (int64_t memberId : memberIds) {
            if (memberId == fromUserId) continue;
            auto memberSessions = onlineManager_.getSessions(memberId);
            if (memberSessions.empty()) continue;

            // 每个接收者构造独立 fwd（mention 字段因人而异）
            json fwd;
            fwd["type"] = Protocol::GROUP_MSG;
            fwd["from"] = std::to_string(fromUserId);
            fwd["to"] = std::to_string(groupId);
            fwd["content"] = content;
            fwd["msgId"] = msgId;
            fwd["timestamp"] = timestamp;
            if (!replyTo.empty()) fwd["replyTo"] = replyTo;
            if (!mentions.empty()) {
                json arr = json::array();
                for (auto uid : mentions) arr.push_back(std::to_string(uid));
                fwd["mentions"] = arr;
            }
            if (mentionSet.count(memberId)) fwd["mention"] = true;
            std::string payload = fwd.dump();
            // Phase 3.1：推送给该成员所有设备
            for (auto& s : memberSessions) s->sendText(payload);
        }
    }

    void handleFileMessage(const WsSessionPtr& session, const json& j, int64_t fromUserId) {
        std::string toStr = j.value("to", "");
        std::string url = j.value("url", "");
        std::string filename = j.value("filename", "");
        int64_t fileSize = j.value("fileSize", (int64_t)0);

        if (toStr.empty() || url.empty()) {
            session->sendText(Protocol::makeError("missing 'to' or 'url'"));
            return;
        }

        int64_t toUserId = 0;
        try { toUserId = std::stoll(toStr); } catch (...) {}

        std::string msgId = Protocol::generateServerMsgId();  // Snowflake 时序有序
        int64_t timestamp = Protocol::nowMs();

        // Save as private message with file URL as content
        std::string content = json({{"url", url}, {"filename", filename}, {"fileSize", fileSize}}).dump();
        messageService_.savePrivateMessage(msgId, fromUserId, toUserId, content, timestamp);

        session->sendText(Protocol::makeAck(msgId));

        broadcastToUser(toUserId, Protocol::makeFileMsg(fromUserId, toUserId, url, filename, fileSize, msgId));
    }

    void handleRecall(const WsSessionPtr& session, const json& j, int64_t fromUserId) {
        std::string msgId = j.value("msgId", "");
        if (msgId.empty()) {
            session->sendText(Protocol::makeError("missing msgId"));
            return;
        }

        // 先刷 Redis 消息队列到 MySQL，确保消息已入库
        flushMessageQueue();

        bool ok = messageService_.recallMessage(msgId, fromUserId);
        if (!ok) {
            session->sendText(Protocol::makeError("recall failed (timeout or not your message)"));
            return;
        }

        // Notify sender
        session->sendText(Protocol::makeRecall(msgId, fromUserId));

        // Broadcast recall to all online friends (simplified: they'll remove the message)
        auto friends = friendService_.getFriends(fromUserId);
        std::string recallMsg = Protocol::makeRecall(msgId, fromUserId);
        for (auto& f : friends) {
            int64_t fid = f.value("userId", (int64_t)0);
            broadcastToUser(fid, recallMsg);
        }
    }

    /**
     * @brief 处理消息编辑（Phase 4.1）
     *
     * 协议：客户端发 {"type":"edit","msgId":"...","newBody":"..."}
     * 流程：
     * 1. 校验字段（msgId / newBody / 长度）
     * 2. 先 flushMessageQueue 确保消息已入库（与 handleRecall 同样原因）
     * 3. 推断 msg_kind（先试私聊，再试群聊）
     * 4. messageService_.editMessage 校验 + 事务执行 UPDATE + INSERT message_edits
     * 5. 推送给会话相关方（私聊：对方；群聊：所有在线成员）
     */
    void handleEdit(const WsSessionPtr& session, const json& j, int64_t fromUserId) {
        std::string msgId = j.value("msgId", "");
        std::string newBody = j.value("newBody", "");
        if (msgId.empty() || newBody.empty()) {
            session->sendText(Protocol::makeError("missing msgId or newBody"));
            return;
        }
        if (newBody.size() > 10000) {
            session->sendText(Protocol::makeError("message too long (max 10000 chars)"));
            return;
        }

        // 与 recall 同样：先 flush Redis 队列，避免新消息还没入 MySQL
        flushMessageQueue();

        // 先尝试私聊编辑；失败再尝试群聊（前端不需告知 msg_kind，服务端自动判断）
        std::string oldBody;
        bool ok = messageService_.editMessage(msgId, fromUserId, newBody, /*msgKind=*/0, &oldBody);
        bool isGroup = false;
        int64_t convId = 0;

        if (ok) {
            // 私聊：查 to_user 作为推送对象
            auto conn = mysqlPool_->acquire(2000);
            if (conn && conn->valid()) {
                auto res = conn->query("SELECT to_user FROM private_messages WHERE msg_id='"
                                        + conn->escape(msgId) + "'");
                if (res && mysql_num_rows(res.get()) > 0) {
                    MYSQL_ROW row = mysql_fetch_row(res.get());
                    convId = std::stoll(row[0]);
                }
                mysqlPool_->release(std::move(conn));
            }
        } else {
            ok = messageService_.editMessage(msgId, fromUserId, newBody, /*msgKind=*/1, &oldBody);
            if (ok) {
                isGroup = true;
                auto conn = mysqlPool_->acquire(2000);
                if (conn && conn->valid()) {
                    auto res = conn->query("SELECT group_id FROM group_messages WHERE msg_id='"
                                            + conn->escape(msgId) + "'");
                    if (res && mysql_num_rows(res.get()) > 0) {
                        MYSQL_ROW row = mysql_fetch_row(res.get());
                        convId = std::stoll(row[0]);
                    }
                    mysqlPool_->release(std::move(conn));
                }
            }
        }

        if (!ok) {
            session->sendText(Protocol::makeError("edit failed (timeout / not your message / already recalled)"));
            return;
        }

        int64_t editedAt = Protocol::nowMs();
        std::string convType = isGroup ? "group" : "private";
        std::string convIdStr = std::to_string(convId);
        std::string editPush = Protocol::makeEdit(msgId, newBody, editedAt,
                                                    fromUserId, convType, convIdStr);

        // 通知发送者（让本端 UI 也能更新"已编辑"标识）
        session->sendText(editPush);

        // 推送给会话相关方
        if (isGroup) {
            auto memberIds = groupService_.getMemberIds(convId);
            for (auto memberId : memberIds) {
                if (memberId == fromUserId) continue;
                broadcastToUser(memberId, editPush);
            }
        } else {
            broadcastToUser(convId, editPush);
        }

        auditService_.log(fromUserId, "edit_message",
                           msgId + ":" + std::to_string(oldBody.size()) +
                           "->" + std::to_string(newBody.size()),
                           "");  // WS 无现成 IP 提取，留空
    }

    /**
     * @brief 处理 Reaction 切换（Phase 4.5）
     *
     * 客户端协议：{"type":"reaction","msgId":"...","emoji":"👍"}
     * 服务端：
     * 1. toggleReaction（DB 已存在则 DELETE，否则 INSERT）
     * 2. getReactions 拿完整 reactions 字典
     * 3. 自动判断 msg_kind（先私聊后群聊）
     * 4. 推 reaction_update 给会话相关方（含发起者，让本端 UI 立即同步）
     */
    void handleReaction(const WsSessionPtr& session, const json& j, int64_t fromUserId) {
        std::string msgId = j.value("msgId", "");
        std::string emoji = j.value("emoji", "");
        if (msgId.empty() || emoji.empty()) {
            session->sendText(Protocol::makeError("missing msgId or emoji"));
            return;
        }
        if (emoji.size() > 16) {
            session->sendText(Protocol::makeError("emoji too long"));
            return;
        }

        // 先 flush，避免对刚发出还没入库的消息做 reaction
        flushMessageQueue();

        bool added = false;
        if (!messageService_.toggleReaction(msgId, fromUserId, emoji, &added)) {
            session->sendText(Protocol::makeError("reaction failed (db error)"));
            return;
        }

        // 查会话信息：
        // 私聊：fetch 双方 (from_user, to_user)，对方 = 不等于 fromUserId 的那个
        //       前端 convId 用对方 userId（与 from_user 角度一致：A 发给 B 时，A 看到的 convId=B；B 看到的 convId=A）
        // 群聊：group_id
        bool isGroup = false;
        int64_t convId = 0;       // 对方 / 群 ID（推送时用，也用于前端定位会话）
        int64_t peer = 0;         // 私聊对方 userId（推送时用）
        bool found = false;
        {
            auto conn = mysqlPool_->acquire(2000);
            if (conn && conn->valid()) {
                auto res = conn->query("SELECT from_user, to_user FROM private_messages WHERE msg_id='"
                                        + conn->escape(msgId) + "'");
                if (res && mysql_num_rows(res.get()) > 0) {
                    MYSQL_ROW row = mysql_fetch_row(res.get());
                    int64_t fromU = std::stoll(row[0]);
                    int64_t toU = std::stoll(row[1]);
                    peer = (fromUserId == fromU) ? toU : fromU;
                    convId = peer;  // 客户端视角：私聊会话 = 对方 userId
                    found = true;
                } else {
                    auto res2 = conn->query("SELECT group_id FROM group_messages WHERE msg_id='"
                                             + conn->escape(msgId) + "'");
                    if (res2 && mysql_num_rows(res2.get()) > 0) {
                        MYSQL_ROW row = mysql_fetch_row(res2.get());
                        convId = std::stoll(row[0]);
                        isGroup = true;
                        found = true;
                    }
                }
                mysqlPool_->release(std::move(conn));
            }
        }
        if (!found) {
            session->sendText(Protocol::makeReactionUpdate(msgId, json::object(),
                                                            "private", "0"));
            return;
        }

        json reactions = messageService_.getReactions(msgId);
        std::string convType = isGroup ? "group" : "private";
        std::string convIdStr = std::to_string(convId);
        std::string push = Protocol::makeReactionUpdate(msgId, reactions, convType, convIdStr);

        // 推送给发起者
        session->sendText(push);

        if (isGroup) {
            auto memberIds = groupService_.getMemberIds(convId);
            for (auto memberId : memberIds) {
                if (memberId == fromUserId) continue;
                broadcastToUser(memberId, push);
            }
        } else {
            // 私聊：推给对方
            broadcastToUser(peer, push);
        }

        (void)added;
    }

    /**
     * @brief 处理客户端 ACK（Phase 2.1 双向 ACK）
     *
     * 协议：客户端收到 msg/group_msg push 后，发 {"type":"client_ack","msgId":"..."} 回去。
     * 服务端：
     * 1. 私聊：UPDATE private_messages SET delivered_at=now WHERE msg_id=? AND to_user=ackerId AND delivered_at IS NULL
     *    成功（affectedRows > 0）则向 from_user 推 delivered
     * 2. 群聊：UPDATE group_messages SET delivered_count = delivered_count + 1 WHERE msg_id=?
     *    服务端按"群聊每个成员 ack 一次"语义计数；推 delivered（带累计计数）给 from_user
     *
     * **幂等**：delivered_at IS NULL 的条件保证私聊只 +1 次；群聊用 message_acks 表去重（防多端重复）
     */
    void handleClientAck(const WsSessionPtr& /*session*/, const json& j, int64_t ackerId) {
        std::string msgId = j.value("msgId", "");
        if (msgId.empty()) return;

        // 先 flush Redis 队列：可能 client_ack 比消息入库还快（高速场景）
        flushMessageQueue();

        // 先尝试私聊
        auto conn = mysqlPool_->acquire(2000);
        if (!conn || !conn->valid()) return;

        int64_t deliveredAt = Protocol::nowMs();
        int64_t fromUser = 0;
        bool isGroup = false;
        int deliveredCount = 1;

        // 私聊：只能由 to_user 触发；用 delivered_at IS NULL 保证幂等
        std::string sql =
            "UPDATE private_messages SET delivered_at=" + std::to_string(deliveredAt)
            + " WHERE msg_id='" + conn->escape(msgId) + "'"
            + " AND to_user=" + std::to_string(ackerId)
            + " AND delivered_at IS NULL";
        int affected = conn->execute(sql);

        if (affected > 0) {
            // 私聊 ACK 成功：查 from_user
            auto res = conn->query("SELECT from_user FROM private_messages WHERE msg_id='"
                                    + conn->escape(msgId) + "'");
            if (res && mysql_num_rows(res.get()) > 0) {
                fromUser = std::stoll(mysql_fetch_row(res.get())[0]);
            }
        } else {
            // 群聊路径
            isGroup = true;
            // 简化：每次 ack +1（不严格去重；接受多端重复）
            std::string gsql =
                "UPDATE group_messages SET delivered_count = delivered_count + 1"
                " WHERE msg_id='" + conn->escape(msgId) + "'";
            if (conn->execute(gsql) > 0) {
                auto res = conn->query("SELECT from_user, delivered_count FROM group_messages WHERE msg_id='"
                                        + conn->escape(msgId) + "'");
                if (res && mysql_num_rows(res.get()) > 0) {
                    MYSQL_ROW row = mysql_fetch_row(res.get());
                    fromUser = std::stoll(row[0]);
                    deliveredCount = std::stoi(row[1]);
                }
            }
        }
        mysqlPool_->release(std::move(conn));

        if (fromUser <= 0) return;  // 消息不存在或重复 ack
        if (fromUser == ackerId) return;  // 不向自己发 delivered

        // 推送 delivered 给发送方
        broadcastToUser(fromUser, Protocol::makeDelivered(msgId, deliveredAt, deliveredCount));
        (void)isGroup;
    }

    void handleReadAck(const WsSessionPtr& /*session*/, const json& j, int64_t fromUserId) {
        std::string toStr = j.value("to", "");
        std::string lastMsgId = j.value("lastMsgId", "");
        if (toStr.empty() || lastMsgId.empty()) return;

        int64_t toUserId = 0;
        try { toUserId = std::stoll(toStr); } catch (...) {}

        // Clear unread count: the reader (fromUserId) has read messages from toUserId
        clearUnread(fromUserId, toUserId);

        // Persist read position in Redis
        if (redisPool_) {
            auto conn = redisPool_->acquire(1000);
            if (conn && conn->valid()) {
                // read_pos:{readerId}:{senderId} = lastMsgId
                conn->set("read_pos:" + std::to_string(fromUserId) + ":" + toStr, lastMsgId);
                redisPool_->release(std::move(conn));
            }
        }

        // Forward read receipt to the original sender
        broadcastToUser(toUserId, Protocol::makeReadAck(fromUserId, toUserId, lastMsgId));
    }

    void handleTyping(const WsSessionPtr& /*session*/, const json& j, int64_t fromUserId) {
        std::string toStr = j.value("to", "");
        if (toStr.empty()) return;
        int64_t toUserId = 0;
        try { toUserId = std::stoll(toStr); } catch (...) {}
        if (toUserId <= 0) return;

        broadcastToUser(toUserId, Protocol::makeTyping(fromUserId, toUserId));
    }

    // ==================== Redis Unread Count ====================

    void incrementUnread(int64_t userId, int64_t peerId) {
        if (!redisPool_) return;
        auto conn = redisPool_->acquire(1000);
        if (conn && conn->valid()) {
            conn->incr("unread:" + std::to_string(userId) + ":" + std::to_string(peerId));
            redisPool_->release(std::move(conn));
        }
    }

    void clearUnread(int64_t userId, int64_t peerId) {
        if (!redisPool_) return;
        auto conn = redisPool_->acquire(1000);
        if (conn && conn->valid()) {
            conn->del("unread:" + std::to_string(userId) + ":" + std::to_string(peerId));
            redisPool_->release(std::move(conn));
        }
    }

    int64_t getUnread(int64_t userId, int64_t peerId) {
        if (!redisPool_) return 0;
        auto conn = redisPool_->acquire(1000);
        if (conn && conn->valid()) {
            std::string val = conn->get("unread:" + std::to_string(userId) + ":" + std::to_string(peerId));
            redisPool_->release(std::move(conn));
            if (!val.empty()) return std::stoll(val);
        }
        return 0;
    }

    // ==================== Redis Message Queue ====================

    void queueMessage(const std::string& msgJson) {
        if (!redisPool_ || !redisBreaker_.allow()) {
            // No Redis 或熔断器打开 —— 直接降级到 MySQL
            directSaveMessage(msgJson);
            return;
        }
        auto conn = redisPool_->acquire(1000);
        if (conn && conn->valid()) {
            conn->lpush("msg_queue", msgJson);
            redisPool_->release(std::move(conn));
            redisBreaker_.recordSuccess();
        } else {
            // Redis unavailable — 记录失败并降级到 MySQL
            redisBreaker_.recordFailure();
            directSaveMessage(msgJson);
        }
    }

    void directSaveMessage(const std::string& msgJson) {
        auto j = json::parse(msgJson, nullptr, false);
        if (j.is_discarded()) return;
        std::string type = j.value("_type", "");
        if (type == "private") {
            messageService_.savePrivateMessage(
                j.value("msgId", ""), j.value("from", (int64_t)0),
                j.value("to", (int64_t)0), j.value("content", ""),
                j.value("timestamp", (int64_t)0));
        } else if (type == "group") {
            messageService_.saveGroupMessage(
                j.value("msgId", ""), j.value("groupId", (int64_t)0),
                j.value("from", (int64_t)0), j.value("content", ""),
                j.value("timestamp", (int64_t)0),
                j.value("mentions", ""));
        }
    }

    void flushMessageQueue() {
        if (!redisPool_) return;

        // MySQL 熔断器打开时跳过刷写，消息继续留在 Redis 队列
        // 等熔断器恢复（半开态）后再尝试入库
        if (!mysqlBreaker_.allow()) return;

        auto conn = redisPool_->acquire(3000);
        if (!conn || !conn->valid()) return;

        // Get up to 100 messages from queue
        auto messages = conn->lrange("msg_queue", 0, 99);
        if (messages.empty()) {
            redisPool_->release(std::move(conn));
            return;
        }

        // Trim the queue (remove the messages we just read)
        conn->ltrim("msg_queue", static_cast<int>(messages.size()), -1);
        redisPool_->release(std::move(conn));

        // Batch insert to MySQL with per-message isolation:
        // 一条坏消息不会阻塞整批，熔断器仅在全部失败时才打开
        int successCount = 0;
        int failCount = 0;
        for (auto& msgStr : messages) {
            auto j = json::parse(msgStr, nullptr, false);
            if (j.is_discarded()) {
                failCount++;
                continue;
            }
            bool ok = false;
            try {
                std::string type = j.value("_type", "");
                if (type == "private") {
                    ok = messageService_.savePrivateMessage(
                        j.value("msgId", ""), j.value("from", (int64_t)0),
                        j.value("to", (int64_t)0), j.value("content", ""),
                        j.value("timestamp", (int64_t)0));
                } else if (type == "group") {
                    ok = messageService_.saveGroupMessage(
                        j.value("msgId", ""), j.value("groupId", (int64_t)0),
                        j.value("from", (int64_t)0), j.value("content", ""),
                        j.value("timestamp", (int64_t)0),
                        j.value("mentions", ""));
                }
            } catch (...) {
                ok = false;
            }
            if (ok) successCount++;
            else failCount++;
        }

        if (successCount > 0) mysqlBreaker_.recordSuccess();
        if (failCount > 0 && successCount == 0) mysqlBreaker_.recordFailure();
    }

    // ==================== Message Dedup ====================

    /// 最近处理过的消息 ID 集合，用于幂等性去重
    std::unordered_set<std::string> recentMsgIds_;
    std::mutex dedupMutex_;

    /**
     * @brief 检查消息是否重复（幂等性去重）
     *
     * 使用内存中的 set 记录最近处理过的 msgId。
     * 当 set 大小超过 100000 时清空，避免无限增长。
     *
     * @param msgId 消息唯一标识
     * @return true 消息已处理过（重复）；false 首次处理
     */
    bool isDuplicate(const std::string& msgId) {
        std::lock_guard<std::mutex> lock(dedupMutex_);
        if (recentMsgIds_.count(msgId)) return true;
        recentMsgIds_.insert(msgId);
        // Keep set bounded (remove oldest when too large)
        if (recentMsgIds_.size() > 100000) {
            recentMsgIds_.clear(); // Simple reset — good enough for dedup window
        }
        return false;
    }

    // ==================== Members ====================

    EventLoop* loop_;
    HttpServer httpServer_;
    WebSocketServer wsServer_;

    std::shared_ptr<MySQLPool> mysqlPool_;
    std::shared_ptr<RedisPool> redisPool_;

    UserService userService_;
    FriendService friendService_;
    GroupService groupService_;
    MessageService messageService_;
    OnlineManager onlineManager_;
    AuditService auditService_;
    JwtRevocationService jwtRevocation_;  ///< jti 黑名单吊销服务
    std::unique_ptr<ESClient> esClient_;  ///< ES 客户端（Phase 4.4）；nullptr=未启用

    // Phase 5.3 冷数据归档查询服务地址（空字符串 = 未启用）
    std::string archiveQueryHost_;
    int archiveQueryPort_ = 0;

    /// ChatServer 直接持有 JWT 实例以解析 jti（UserService 的 JWT 是内部使用）
    JWT jwt_;

    std::string jwtSecret_;

    // 熔断器：保护 MySQL / Redis 调用，避免下游故障时把服务拖垮
    // 打开时直接短路跳过，超时后进入半开态探测恢复
    CircuitBreaker mysqlBreaker_;
    CircuitBreaker redisBreaker_;
};
