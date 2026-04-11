#pragma once

#include "websocket/WsSession.h"
#include <unordered_map>
#include <mutex>
#include <vector>

class OnlineManager {
public:
    void addUser(int64_t userId, const WsSessionPtr& session) {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_[userId] = session;
    }

    void removeUser(int64_t userId) {
        std::lock_guard<std::mutex> lock(mutex_);
        sessions_.erase(userId);
    }

    WsSessionPtr getSession(int64_t userId) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(userId);
        if (it != sessions_.end() && it->second->isOpen()) {
            return it->second;
        }
        return nullptr;
    }

    bool isOnline(int64_t userId) {
        return getSession(userId) != nullptr;
    }

    std::vector<int64_t> getOnlineUsers() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<int64_t> users;
        for (auto& [id, session] : sessions_) {
            if (session->isOpen()) users.push_back(id);
        }
        return users;
    }

    int64_t getUserId(const WsSessionPtr& session) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [id, s] : sessions_) {
            if (s == session) return id;
        }
        return -1;
    }

private:
    std::unordered_map<int64_t, WsSessionPtr> sessions_;
    std::mutex mutex_;
};
