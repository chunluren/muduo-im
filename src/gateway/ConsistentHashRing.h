/**
 * @file ConsistentHashRing.h
 * @brief 一致性 hash 环（Phase 1.2 W3.D1-D2）
 *
 * 用 std::hash<string> + 200 虚拟节点。get(key) 返回选中的 node 字符串
 * （一般是 endpoint 的 addr）。线程不安全；外层用 mutex 保护读写。
 */
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

class ConsistentHashRing {
public:
    explicit ConsistentHashRing(int virtualNodes = 200) : virtualNodes_(virtualNodes) {}

    void addNode(const std::string& node) {
        for (int i = 0; i < virtualNodes_; ++i) {
            uint64_t h = std::hash<std::string>{}(node + "#" + std::to_string(i));
            ring_[h] = node;
        }
    }

    void removeNode(const std::string& node) {
        for (auto it = ring_.begin(); it != ring_.end();) {
            if (it->second == node) it = ring_.erase(it);
            else ++it;
        }
    }

    bool empty() const { return ring_.empty(); }

    std::string get(const std::string& key) const {
        if (ring_.empty()) return {};
        uint64_t h = std::hash<std::string>{}(key);
        auto it = ring_.lower_bound(h);
        if (it == ring_.end()) it = ring_.begin();
        return it->second;
    }

    std::vector<std::string> distinctNodes() const {
        std::vector<std::string> out;
        std::string last;
        for (auto& [_, n] : ring_) {
            if (n != last) { out.push_back(n); last = n; }
        }
        // 唯一化（虚节点会重复）
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

private:
    int virtualNodes_;
    std::map<uint64_t, std::string> ring_;
};
