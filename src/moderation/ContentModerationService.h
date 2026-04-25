/**
 * @file ContentModerationService.h
 * @brief 内容审核服务（Phase 5.2 自建方案：AC 自动机 + 词库热更新）
 *
 * 工作流程：
 * 1. 启动时从 moderation_wordlist 表加载敏感词，构建 AC 自动机
 * 2. 后台线程每 60 秒检查 max(updated_at) 是否变化 → 触发热更新（双缓冲无锁切换）
 * 3. checkText(text) 返回 Pass/Review/Block + 匹配词
 * 4. 调用方落 moderation_logs 审计（合规要求 180 天）
 *
 * 决策规则（多匹配时取最严级别）：
 *   含 ban → Block
 *   含 review → Review
 *   含 warn → Pass + warning（实现按 Pass 处理）
 *   全无 → Pass
 *
 * 图片审核：在外部 NSFW Python worker 实现，本类只管文字。
 */
#pragma once

#include "moderation/AhoCorasick.h"
#include "pool/MySQLPool.h"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

class ContentModerationService {
public:
    enum Result { Pass, Review, Block };

    struct Response {
        Result result = Pass;
        std::string reason;
        std::vector<std::string> matchedWords;
    };

    explicit ContentModerationService(std::shared_ptr<MySQLPool> db) : db_(db) {
        reload();  // 启动加载
    }

    ~ContentModerationService() {
        stopReloadThread();
    }

    /**
     * @brief 文字审核
     */
    Response checkText(const std::string& text) {
        Response r;
        if (text.empty()) return r;
        // 拷贝当前 AC 指针（atomic load）—— 双缓冲安全
        auto ac = std::atomic_load(&current_);
        if (!ac) return r;
        auto matches = ac->search(text);
        if (matches.empty()) return r;

        // 取最严级别
        bool hasBan = false, hasReview = false;
        for (auto& m : matches) {
            r.matchedWords.push_back(m.word);
            if (m.category == "ban") hasBan = true;
            else if (m.category == "review") hasReview = true;
        }
        if (hasBan) {
            r.result = Block;
            r.reason = "contains banned word";
        } else if (hasReview) {
            r.result = Review;
            r.reason = "needs human review";
        }
        return r;
    }

    /// 启动后台热更新线程（每 60s 检查词库版本）
    void startReloadThread(int intervalSec = 60) {
        if (reloadThread_.joinable()) return;
        running_ = true;
        reloadThread_ = std::thread([this, intervalSec]() {
            while (running_) {
                std::this_thread::sleep_for(std::chrono::seconds(intervalSec));
                if (!running_) break;
                int64_t latest = queryMaxUpdatedAt();
                if (latest > 0 && latest != lastVersion_) {
                    reload();
                }
            }
        });
    }

    void stopReloadThread() {
        running_ = false;
        if (reloadThread_.joinable()) reloadThread_.join();
    }

    /// 强制重新加载词库（双缓冲原子切换）
    void reload() {
        if (!db_) return;
        auto conn = db_->acquire(2000);
        if (!conn || !conn->valid()) return;
        auto res = conn->query(
            "SELECT word, category, IFNULL(UNIX_TIMESTAMP(updated_at)*1000, 0) "
            "FROM moderation_wordlist WHERE enabled=1");
        if (!res) {
            db_->release(std::move(conn));
            return;
        }
        auto next = std::make_shared<AhoCorasick>();
        int64_t maxVer = 0;
        int count = 0;
        MYSQL_ROW row;
        while ((row = mysql_fetch_row(res.get()))) {
            if (row[0] && row[1]) {
                next->addWord(row[0], row[1]);
                ++count;
                if (row[2]) {
                    int64_t v = std::stoll(row[2]);
                    if (v > maxVer) maxVer = v;
                }
            }
        }
        next->build();
        db_->release(std::move(conn));

        // 原子切换
        std::atomic_store(&current_, next);
        lastVersion_ = maxVer;
        wordCount_ = count;
    }

    /// 写审计日志（不抛异常 — 失败静默）
    void logModeration(int64_t uid, const std::string& contentType,
                        const std::string& contentSample,
                        Result result, const std::string& reason,
                        const std::vector<std::string>& matched,
                        double nsfwScore = 0.0) {
        auto conn = db_->acquire(1000);
        if (!conn || !conn->valid()) return;
        const char* resultStr = result == Block ? "block"
                              : result == Review ? "review" : "pass";
        std::string matchedStr;
        for (size_t i = 0; i < matched.size(); ++i) {
            if (i > 0) matchedStr += ",";
            matchedStr += matched[i];
        }
        std::string sample = contentSample.size() > 200
            ? contentSample.substr(0, 200) : contentSample;
        PreparedStatement stmt(conn,
            "INSERT INTO moderation_logs (uid, content_type, content_sample, "
            "result, reason, matched_words, nsfw_score) VALUES (?, ?, ?, ?, ?, ?, ?)");
        if (!stmt.valid()) { db_->release(std::move(conn)); return; }
        stmt.bindInt64(1, uid);
        stmt.bindString(2, contentType);
        stmt.bindString(3, sample);
        stmt.bindString(4, resultStr);
        stmt.bindString(5, reason);
        stmt.bindString(6, matchedStr);
        // nsfw_score: 简化用 stringstream 转字符串再 bindString
        char nsfwBuf[16];
        std::snprintf(nsfwBuf, sizeof(nsfwBuf), "%.3f", nsfwScore);
        stmt.bindString(7, nsfwBuf);
        stmt.execute();
        db_->release(std::move(conn));
    }

    int wordCount() const { return wordCount_; }

private:
    int64_t queryMaxUpdatedAt() {
        if (!db_) return 0;
        auto conn = db_->acquire(1000);
        if (!conn || !conn->valid()) return 0;
        auto res = conn->query(
            "SELECT IFNULL(UNIX_TIMESTAMP(MAX(updated_at))*1000, 0) "
            "FROM moderation_wordlist WHERE enabled=1");
        int64_t v = 0;
        if (res) {
            MYSQL_ROW row = mysql_fetch_row(res.get());
            if (row && row[0]) v = std::stoll(row[0]);
        }
        db_->release(std::move(conn));
        return v;
    }

    std::shared_ptr<MySQLPool> db_;
    std::shared_ptr<AhoCorasick> current_;
    std::atomic<int64_t> lastVersion_{0};
    std::atomic<int> wordCount_{0};
    std::atomic<bool> running_{false};
    std::thread reloadThread_;
};
