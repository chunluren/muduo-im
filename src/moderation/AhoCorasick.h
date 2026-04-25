/**
 * @file AhoCorasick.h
 * @brief AC 自动机（Aho-Corasick）— 多模式串匹配（Phase 5.2）
 *
 * 用途：内容审核的敏感词扫描。
 * 复杂度：构建 O(Σ|word|)；查找 O(|text| + match_count)
 * 容量：10 万词级别 < 50ms 构建；10KB 文本 < 1ms 扫描。
 *
 * 用 std::unordered_map 而非数组节点（适配 UTF-8 中文，每节点子节点稀疏）。
 *
 * 注意：本实现按 byte 处理（UTF-8 兼容）。中文 GB 一字三 byte，但 AC 算法不区分；
 * 命中时返回的 byte 范围可能切到 UTF-8 字符中间，调用方按 byte 串使用即可（如比较
 * "是否在文本中存在"），不要用于 substring 显示给用户。
 */
#pragma once

#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

class AhoCorasick {
public:
    struct Match {
        size_t start;
        size_t length;
        std::string word;
        std::string category;  // "ban" / "review" / "warn"
    };

    AhoCorasick() {
        nodes_.emplace_back();  // root
    }

    /// 添加敏感词
    void addWord(const std::string& word, const std::string& category) {
        if (word.empty()) return;
        int cur = 0;
        for (unsigned char c : word) {
            // 不持引用：emplace_back 会让 nodes_ 扩容失效之前持有的引用
            auto it = nodes_[cur].children.find(c);
            if (it == nodes_[cur].children.end()) {
                int next = (int)nodes_.size();
                nodes_.emplace_back();
                // 此处 nodes_ 已扩容，重新通过 cur 索引访问
                nodes_[cur].children[c] = next;
                cur = next;
            } else {
                cur = it->second;
            }
        }
        nodes_[cur].words.emplace_back(word, category);
    }

    /// 构建失败指针（addWord 完成后必须调用一次）
    void build() {
        std::queue<int> q;
        nodes_[0].fail = 0;
        for (auto& [_, child] : nodes_[0].children) {
            nodes_[child].fail = 0;
            q.push(child);
        }
        while (!q.empty()) {
            int cur = q.front(); q.pop();
            for (auto& [c, child] : nodes_[cur].children) {
                int f = nodes_[cur].fail;
                while (f != 0 && nodes_[f].children.find(c) == nodes_[f].children.end()) {
                    f = nodes_[f].fail;
                }
                auto it = nodes_[f].children.find(c);
                if (it != nodes_[f].children.end() && it->second != child) {
                    nodes_[child].fail = it->second;
                } else {
                    nodes_[child].fail = 0;
                }
                // 沿失败指针累计能匹配到的词
                int fail = nodes_[child].fail;
                if (fail != 0) {
                    for (auto& w : nodes_[fail].words) {
                        nodes_[child].words.push_back(w);
                    }
                }
                q.push(child);
            }
        }
    }

    /// 在文本中查找所有匹配
    std::vector<Match> search(const std::string& text) const {
        std::vector<Match> result;
        int cur = 0;
        for (size_t i = 0; i < text.size(); ++i) {
            unsigned char c = text[i];
            while (cur != 0 && nodes_[cur].children.find(c) == nodes_[cur].children.end()) {
                cur = nodes_[cur].fail;
            }
            auto it = nodes_[cur].children.find(c);
            if (it != nodes_[cur].children.end()) {
                cur = it->second;
            }
            for (auto& [w, cat] : nodes_[cur].words) {
                Match m;
                m.start = i + 1 - w.size();
                m.length = w.size();
                m.word = w;
                m.category = cat;
                result.push_back(m);
            }
        }
        return result;
    }

    /// 节点数（调试用）
    size_t nodeCount() const { return nodes_.size(); }

private:
    struct Node {
        std::unordered_map<unsigned char, int> children;
        int fail = 0;
        std::vector<std::pair<std::string, std::string>> words;
    };
    std::vector<Node> nodes_;
};
