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
#include "server/UserService.h"
#include "server/OnlineManager.h"
#include "server/FriendService.h"
#include "server/GroupService.h"
#include "server/MessageService.h"
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
     * @param req HTTP 请求对象，从中读取 Authorization 头
     * @return 验证成功返回用户 ID（> 0）；Token 缺失、格式错误或验证失败返回 -1
     */
    int64_t authFromRequest(const HttpRequest& req) {
        std::string auth = req.getHeader("authorization");
        if (auth.size() <= 7 || auth.substr(0, 7) != "Bearer ") {
            return -1;
        }
        std::string token = auth.substr(7);
        return userService_.verifyToken(token);
    }

    /// 鉴权辅助：失败时设置 401 响应并返回 -1
    /// 用法: int64_t userId = requireAuth(req, resp); if (userId < 0) return;
    int64_t requireAuth(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = authFromRequest(req);
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

    // ==================== HTTP Route Table ====================

    void setupHttpRoutes() {
        // 静态文件服务（前端）
        httpServer_.serveStatic("/", "../web");

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

        // ---- File upload ----
        httpServer_.POST("/api/upload", [this](const HttpRequest& req, HttpResponse& resp) { handleUpload(req, resp); });
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
        resp.setJson(userService_.registerUser(
            j.value("username", ""), j.value("password", ""), j.value("nickname", "")
        ).dump());
    }

    void handleLogin(const HttpRequest& req, HttpResponse& resp) {
        json j;
        if (!parseJsonBody(req, resp, j)) return;
        resp.setJson(userService_.login(
            j.value("username", ""), j.value("password", "")
        ).dump());
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
        resp.setJson(groupService_.kickMember(userId, groupId, targetUserId).dump());
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
        resp.setJson(result.dump());
    }

    void handleSearchMessages(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        resp.setJson(messageService_.searchMessages(userId, req.getParam("keyword", "")).dump());
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
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        json j;
        if (!parseJsonBody(req, resp, j)) return;
        resp.setJson(userService_.changePassword(
            userId, j.value("oldPassword", ""), j.value("newPassword", "")
        ).dump());
    }

    void handleDeleteAccount(const HttpRequest& req, HttpResponse& resp) {
        int64_t userId = requireAuth(req, resp);
        if (userId < 0) return;
        json j;
        if (!parseJsonBody(req, resp, j)) return;
        resp.setJson(userService_.deleteAccount(userId, j.value("password", "")).dump());
    }

    // ==================== Route Handlers: Upload ====================

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

                resp.setJson(json({
                    {"success", true},
                    {"url", "/uploads/" + savedName},
                    {"filename", part.filename},
                    {"size", part.data.size()}
                }).dump());
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
    int64_t extractUserIdFromPath(const std::string& path) {
        auto pos = path.find("token=");
        if (pos == std::string::npos) return -1;
        std::string token = path.substr(pos + 6);
        // Strip any trailing query params
        auto ampPos = token.find('&');
        if (ampPos != std::string::npos) {
            token = token.substr(0, ampPos);
        }
        return userService_.verifyToken(token);
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

            session->setContext("userId", std::to_string(userId));
            onlineManager_.addUser(userId, session);

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
            } else if (type == Protocol::READ_ACK) {
                handleReadAck(session, j, userId);
            } else {
                session->sendText(Protocol::makeError("unknown message type"));
            }
        });

        // Close handler: remove from online, notify friends offline
        wsServer_.setCloseHandler([this](const WsSessionPtr& session) {
            std::string userIdStr = session->getContext("userId", "0");
            int64_t userId = 0;
            try { userId = std::stoll(userIdStr); } catch (...) {}
            if (userId <= 0) return;

            onlineManager_.removeUser(userId);

            // Notify friends that user is offline
            auto friends = friendService_.getFriends(userId);
            std::string offlineMsg = Protocol::makeOffline(userId);
            for (auto& f : friends) {
                int64_t friendId = f.value("userId", (int64_t)0);
                auto friendSession = onlineManager_.getSession(friendId);
                if (friendSession) {
                    friendSession->sendText(offlineMsg);
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

        std::string msgId = Protocol::generateMsgId();

        // Idempotent delivery: dedup by msgId
        if (isDuplicate(msgId)) {
            session->sendText(Protocol::makeAck(msgId));
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

        // ACK sender
        session->sendText(Protocol::makeAck(msgId));

        // Forward to recipient if online
        auto recipientSession = onlineManager_.getSession(toUserId);
        if (recipientSession) {
            json fwd;
            fwd["type"] = Protocol::MSG;
            fwd["from"] = std::to_string(fromUserId);
            fwd["to"] = std::to_string(toUserId);
            fwd["content"] = content;
            fwd["msgId"] = msgId;
            fwd["timestamp"] = timestamp;
            if (!replyTo.empty()) fwd["replyTo"] = replyTo;
            recipientSession->sendText(fwd.dump());
        } else {
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

        std::string msgId = Protocol::generateMsgId();

        // Idempotent delivery: dedup by msgId
        if (isDuplicate(msgId)) {
            session->sendText(Protocol::makeAck(msgId));
            return;
        }

        int64_t timestamp = Protocol::nowMs();

        // Queue message (Redis) or direct save (fallback)
        if (redisPool_) {
            json queueItem = {
                {"_type", "group"}, {"msgId", msgId}, {"groupId", groupId},
                {"from", fromUserId}, {"content", content}, {"timestamp", timestamp}
            };
            queueMessage(queueItem.dump());
        } else {
            messageService_.saveGroupMessage(msgId, groupId, fromUserId, content, timestamp);
        }

        // ACK sender
        session->sendText(Protocol::makeAck(msgId));

        // Forward to all online group members (skip sender)
        auto memberIds = groupService_.getMemberIds(groupId);
        json fwd;
        fwd["type"] = Protocol::GROUP_MSG;
        fwd["from"] = std::to_string(fromUserId);
        fwd["to"] = std::to_string(groupId);
        fwd["content"] = content;
        fwd["msgId"] = msgId;
        fwd["timestamp"] = timestamp;
        if (!replyTo.empty()) fwd["replyTo"] = replyTo;
        std::string groupMsg = fwd.dump();
        for (int64_t memberId : memberIds) {
            if (memberId == fromUserId) continue;
            auto memberSession = onlineManager_.getSession(memberId);
            if (memberSession) {
                memberSession->sendText(groupMsg);
            }
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

        std::string msgId = Protocol::generateMsgId();
        int64_t timestamp = Protocol::nowMs();

        // Save as private message with file URL as content
        std::string content = json({{"url", url}, {"filename", filename}, {"fileSize", fileSize}}).dump();
        messageService_.savePrivateMessage(msgId, fromUserId, toUserId, content, timestamp);

        session->sendText(Protocol::makeAck(msgId));

        auto recipientSession = onlineManager_.getSession(toUserId);
        if (recipientSession) {
            recipientSession->sendText(Protocol::makeFileMsg(fromUserId, toUserId, url, filename, fileSize, msgId));
        }
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
            auto s = onlineManager_.getSession(fid);
            if (s) s->sendText(recallMsg);
        }
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
        auto senderSession = onlineManager_.getSession(toUserId);
        if (senderSession) {
            senderSession->sendText(Protocol::makeReadAck(fromUserId, toUserId, lastMsgId));
        }
    }

    void handleTyping(const WsSessionPtr& /*session*/, const json& j, int64_t fromUserId) {
        std::string toStr = j.value("to", "");
        if (toStr.empty()) return;
        int64_t toUserId = 0;
        try { toUserId = std::stoll(toStr); } catch (...) {}
        if (toUserId <= 0) return;

        auto recipientSession = onlineManager_.getSession(toUserId);
        if (recipientSession) {
            recipientSession->sendText(Protocol::makeTyping(fromUserId, toUserId));
        }
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
                j.value("timestamp", (int64_t)0));
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
                        j.value("timestamp", (int64_t)0));
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

    std::string jwtSecret_;

    // 熔断器：保护 MySQL / Redis 调用，避免下游故障时把服务拖垮
    // 打开时直接短路跳过，超时后进入半开态探测恢复
    CircuitBreaker mysqlBreaker_;
    CircuitBreaker redisBreaker_;
};
