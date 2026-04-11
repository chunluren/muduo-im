#pragma once

#include "common/JWT.h"
#include "common/Protocol.h"
#include "server/UserService.h"
#include "server/OnlineManager.h"
#include "server/FriendService.h"
#include "server/GroupService.h"
#include "server/MessageService.h"
#include "http/HttpServer.h"
#include "websocket/WebSocketServer.h"
#include "pool/MySQLPool.h"
#include "pool/RedisPool.h"
#include "net/EventLoop.h"
#include "net/InetAddress.h"
#include <nlohmann/json.hpp>
#include <memory>
#include <string>

using json = nlohmann::json;

class ChatServer {
public:
    ChatServer(EventLoop* loop, uint16_t httpPort, uint16_t wsPort,
               const MySQLPoolConfig& mysqlConfig, const RedisPoolConfig& redisConfig,
               const std::string& jwtSecret)
        : httpServer_(loop, InetAddress(httpPort), "HttpServer")
        , wsServer_(loop, InetAddress(wsPort), "WebSocketServer")
        , mysqlPool_(std::make_shared<MySQLPool>(mysqlConfig))
        , redisPool_(std::make_shared<RedisPool>(redisConfig))
        , userService_(mysqlPool_, jwtSecret)
        , friendService_(mysqlPool_)
        , groupService_(mysqlPool_)
        , messageService_(mysqlPool_)
        , jwtSecret_(jwtSecret)
    {
        setupHttpRoutes();
        setupWebSocket();
    }

    void start() {
        httpServer_.enableCors();
        httpServer_.start();
        wsServer_.start();
    }

private:
    // ==================== Auth Helper ====================

    /// Extract userId from "Authorization: Bearer xxx" header, returns -1 on failure
    int64_t authFromRequest(const HttpRequest& req) {
        std::string auth = req.getHeader("authorization");
        if (auth.size() <= 7 || auth.substr(0, 7) != "Bearer ") {
            return -1;
        }
        std::string token = auth.substr(7);
        return userService_.verifyToken(token);
    }

    // ==================== HTTP REST API ====================

    void setupHttpRoutes() {
        // 静态文件服务（前端）
        httpServer_.serveStatic("/", "../web");

        // POST /api/register
        httpServer_.POST("/api/register", [this](const HttpRequest& req, HttpResponse& resp) {
            auto j = json::parse(req.body, nullptr, false);
            if (j.is_discarded()) {
                resp = HttpResponse::badRequest("invalid JSON");
                return;
            }
            std::string username = j.value("username", "");
            std::string password = j.value("password", "");
            std::string nickname = j.value("nickname", "");
            json result = userService_.registerUser(username, password, nickname);
            resp.setJson(result.dump());
        });

        // POST /api/login
        httpServer_.POST("/api/login", [this](const HttpRequest& req, HttpResponse& resp) {
            auto j = json::parse(req.body, nullptr, false);
            if (j.is_discarded()) {
                resp = HttpResponse::badRequest("invalid JSON");
                return;
            }
            std::string username = j.value("username", "");
            std::string password = j.value("password", "");
            json result = userService_.login(username, password);
            resp.setJson(result.dump());
        });

        // GET /api/friends
        httpServer_.GET("/api/friends", [this](const HttpRequest& req, HttpResponse& resp) {
            int64_t userId = authFromRequest(req);
            if (userId < 0) {
                resp.setStatusCode(HttpStatusCode::UNAUTHORIZED);
                resp.setText("unauthorized");
                return;
            }
            json result = friendService_.getFriends(userId);
            resp.setJson(result.dump());
        });

        // POST /api/friends/add
        httpServer_.POST("/api/friends/add", [this](const HttpRequest& req, HttpResponse& resp) {
            int64_t userId = authFromRequest(req);
            if (userId < 0) {
                resp.setStatusCode(HttpStatusCode::UNAUTHORIZED);
                resp.setText("unauthorized");
                return;
            }
            auto j = json::parse(req.body, nullptr, false);
            if (j.is_discarded()) {
                resp = HttpResponse::badRequest("invalid JSON");
                return;
            }
            int64_t friendId = j.value("friendId", (int64_t)0);
            json result = friendService_.addFriend(userId, friendId);
            resp.setJson(result.dump());
        });

        // POST /api/friends/delete
        httpServer_.POST("/api/friends/delete", [this](const HttpRequest& req, HttpResponse& resp) {
            int64_t userId = authFromRequest(req);
            if (userId < 0) {
                resp.setStatusCode(HttpStatusCode::UNAUTHORIZED);
                resp.setText("unauthorized");
                return;
            }
            auto j = json::parse(req.body, nullptr, false);
            if (j.is_discarded()) {
                resp = HttpResponse::badRequest("invalid JSON");
                return;
            }
            int64_t friendId = j.value("friendId", (int64_t)0);
            json result = friendService_.deleteFriend(userId, friendId);
            resp.setJson(result.dump());
        });

        // GET /api/groups
        httpServer_.GET("/api/groups", [this](const HttpRequest& req, HttpResponse& resp) {
            int64_t userId = authFromRequest(req);
            if (userId < 0) {
                resp.setStatusCode(HttpStatusCode::UNAUTHORIZED);
                resp.setText("unauthorized");
                return;
            }
            json result = groupService_.getUserGroups(userId);
            resp.setJson(result.dump());
        });

        // POST /api/groups/create
        httpServer_.POST("/api/groups/create", [this](const HttpRequest& req, HttpResponse& resp) {
            int64_t userId = authFromRequest(req);
            if (userId < 0) {
                resp.setStatusCode(HttpStatusCode::UNAUTHORIZED);
                resp.setText("unauthorized");
                return;
            }
            auto j = json::parse(req.body, nullptr, false);
            if (j.is_discarded()) {
                resp = HttpResponse::badRequest("invalid JSON");
                return;
            }
            std::string name = j.value("name", "");
            json result = groupService_.createGroup(userId, name);
            resp.setJson(result.dump());
        });

        // POST /api/groups/join
        httpServer_.POST("/api/groups/join", [this](const HttpRequest& req, HttpResponse& resp) {
            int64_t userId = authFromRequest(req);
            if (userId < 0) {
                resp.setStatusCode(HttpStatusCode::UNAUTHORIZED);
                resp.setText("unauthorized");
                return;
            }
            auto j = json::parse(req.body, nullptr, false);
            if (j.is_discarded()) {
                resp = HttpResponse::badRequest("invalid JSON");
                return;
            }
            int64_t groupId = j.value("groupId", (int64_t)0);
            json result = groupService_.joinGroup(userId, groupId);
            resp.setJson(result.dump());
        });

        // GET /api/groups/members
        httpServer_.GET("/api/groups/members", [this](const HttpRequest& req, HttpResponse& resp) {
            int64_t userId = authFromRequest(req);
            if (userId < 0) {
                resp.setStatusCode(HttpStatusCode::UNAUTHORIZED);
                resp.setText("unauthorized");
                return;
            }
            std::string groupIdStr = req.getParam("groupId", "0");
            int64_t groupId = 0;
            try { groupId = std::stoll(groupIdStr); } catch (...) {}
            json result = groupService_.getMembers(groupId);
            resp.setJson(result.dump());
        });

        // GET /api/messages/history
        httpServer_.GET("/api/messages/history", [this](const HttpRequest& req, HttpResponse& resp) {
            int64_t userId = authFromRequest(req);
            if (userId < 0) {
                resp.setStatusCode(HttpStatusCode::UNAUTHORIZED);
                resp.setText("unauthorized");
                return;
            }

            std::string peerIdStr = req.getParam("peerId", "0");
            std::string groupIdStr = req.getParam("groupId", "0");
            std::string beforeStr = req.getParam("before", "0");
            int64_t peerId = 0, groupId = 0, before = 0;
            try { peerId = std::stoll(peerIdStr); } catch (...) {}
            try { groupId = std::stoll(groupIdStr); } catch (...) {}
            try { before = std::stoll(beforeStr); } catch (...) {}

            json result;
            if (groupId > 0) {
                result = messageService_.getGroupHistory(groupId, 50, before);
            } else {
                result = messageService_.getPrivateHistory(userId, peerId, 50, before);
            }
            resp.setJson(result.dump());
        });
    }

    // ==================== WebSocket ====================

    /// Extract userId from path like "/ws?token=xxx"
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
        if (toStr.empty() || content.empty()) {
            session->sendText(Protocol::makeError("missing 'to' or 'content'"));
            return;
        }

        int64_t toUserId = 0;
        try { toUserId = std::stoll(toStr); } catch (...) {}
        if (toUserId <= 0) {
            session->sendText(Protocol::makeError("invalid 'to'"));
            return;
        }

        std::string msgId = Protocol::generateMsgId();
        int64_t timestamp = Protocol::nowMs();

        // Save to DB
        messageService_.savePrivateMessage(msgId, fromUserId, toUserId, content, timestamp);

        // ACK sender
        session->sendText(Protocol::makeAck(msgId));

        // Forward to recipient if online
        auto recipientSession = onlineManager_.getSession(toUserId);
        if (recipientSession) {
            recipientSession->sendText(Protocol::makePrivateMsg(fromUserId, toUserId, content, msgId));
        }
    }

    void handleGroupMessage(const WsSessionPtr& session, const json& j, int64_t fromUserId) {
        std::string toStr = j.value("to", "");
        std::string content = j.value("content", "");
        if (toStr.empty() || content.empty()) {
            session->sendText(Protocol::makeError("missing 'to' or 'content'"));
            return;
        }

        int64_t groupId = 0;
        try { groupId = std::stoll(toStr); } catch (...) {}
        if (groupId <= 0) {
            session->sendText(Protocol::makeError("invalid group id"));
            return;
        }

        std::string msgId = Protocol::generateMsgId();
        int64_t timestamp = Protocol::nowMs();

        // Save to DB
        messageService_.saveGroupMessage(msgId, groupId, fromUserId, content, timestamp);

        // ACK sender
        session->sendText(Protocol::makeAck(msgId));

        // Forward to all online group members (skip sender)
        auto memberIds = groupService_.getMemberIds(groupId);
        std::string groupMsg = Protocol::makeGroupMsg(fromUserId, groupId, content, msgId);
        for (int64_t memberId : memberIds) {
            if (memberId == fromUserId) continue;
            auto memberSession = onlineManager_.getSession(memberId);
            if (memberSession) {
                memberSession->sendText(groupMsg);
            }
        }
    }

    // ==================== Members ====================

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
};
