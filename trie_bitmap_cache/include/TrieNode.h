/**
 * TrieNode.h — Radix Trie for B-tree Inner-Node Routing Acceleration
 *
 * This trie caches the routing function of the B+-tree: given a key, which
 * remote leaf does it reside in?  Instead of traversing cached inner nodes
 * one-by-one (DEX's swizzle-based approach) or searching a skip-list
 * (CHIME's TreeCache), the trie provides O(key_bytes) lookup by walking
 * the key's bytes through a 256-way radix trie.
 *
 * Design:
 *   - 8-bit radix (256-way branching at each level, one per key byte)
 *   - Children arrays are lazily allocated (only inner nodes that branch)
 *   - Terminal nodes store a TrieRouteEntry: {leaf_addr, fence_low, fence_high}
 *   - LFU frequency counters for two-random-choice eviction (CHIME-inspired)
 *
 * Reuses from DEX: Key (uint64_t), GlobalAddress (from Common.h / GlobalAddress.h)
 * Adapts from CHIME: LFU two-random-choice eviction pattern (TreeCache.h)
 */

#pragma once

#include "Common.h"
#include "GlobalAddress.h"

#include <atomic>
#include <algorithm>
#include <cstring>
#include <random>
#include <vector>

namespace tbc {

// -------------------------------------------------------------------
// TrieRouteEntry: stored at terminal trie nodes, maps a key range
// [fence_low, fence_high] to a B-tree leaf's remote GlobalAddress.
// -------------------------------------------------------------------
struct TrieRouteEntry {
    GlobalAddress leaf_addr;  // Remote address of the B-tree leaf page
    Key fence_low;            // Lower fence key (inclusive)
    Key fence_high;           // Upper fence key (inclusive)

    TrieRouteEntry()
        : leaf_addr(GlobalAddress::Null()), fence_low(0), fence_high(0) {}

    TrieRouteEntry(GlobalAddress addr, Key low, Key high)
        : leaf_addr(addr), fence_low(low), fence_high(high) {}

    bool valid() const { return leaf_addr != GlobalAddress::Null(); }
};

// -------------------------------------------------------------------
// TrieNode: a single node in the radix trie.
// Inner nodes have a children[256] array (lazily allocated).
// Terminal nodes hold a TrieRouteEntry.
// -------------------------------------------------------------------
struct TrieNode {
    static constexpr int FANOUT = 256;

    // Children: lazily allocated array of 256 pointers.
    // nullptr until this node needs to branch.
    std::atomic<TrieNode*>* children;

    // Routing info: valid when route.valid() is true.
    // Means the entire subtrie rooted here maps to a single B-tree leaf.
    TrieRouteEntry route;

    // LFU frequency counter for eviction decisions
    std::atomic<int32_t> freq{0};

    // Depth in key bytes (0 = root, splits on MSB; 7 = splits on LSB)
    uint8_t depth;

    TrieNode(uint8_t d = 0) : children(nullptr), depth(d) {}

    ~TrieNode() {
        if (children) {
            for (int i = 0; i < FANOUT; ++i) {
                TrieNode* child = children[i].load(std::memory_order_relaxed);
                if (child) delete child;
            }
            delete[] children;
        }
    }

    bool is_terminal() const { return route.valid(); }

    void ensure_children() {
        if (!children) {
            auto* arr = new std::atomic<TrieNode*>[FANOUT];
            for (int i = 0; i < FANOUT; ++i)
                arr[i].store(nullptr, std::memory_order_relaxed);
            children = arr;
        }
    }

    TrieNode* get_child(uint8_t byte) const {
        if (!children) return nullptr;
        return children[byte].load(std::memory_order_acquire);
    }

    void set_child(uint8_t byte, TrieNode* child) {
        ensure_children();
        children[byte].store(child, std::memory_order_release);
    }
};

// -------------------------------------------------------------------
// Helpers: convert between uint64_t Key and big-endian byte array
// for trie traversal (MSB at index 0).
// -------------------------------------------------------------------
inline void key_to_bytes(Key k, uint8_t bytes[8]) {
    for (int i = 7; i >= 0; --i) {
        bytes[i] = static_cast<uint8_t>(k & 0xFF);
        k >>= 8;
    }
}

inline Key bytes_to_key(const uint8_t bytes[8]) {
    Key k = 0;
    for (int i = 0; i < 8; ++i)
        k = (k << 8) | bytes[i];
    return k;
}

// -------------------------------------------------------------------
// TrieCache: manages the routing trie.
// Insert routing entries, look up key → leaf GlobalAddress, evict LFU.
// -------------------------------------------------------------------
class TrieCache {
public:
    explicit TrieCache(uint64_t max_entries = 4ULL * 1024 * 1024)
        : max_entries_(max_entries) {
        root_ = new TrieNode(0);
    }

    ~TrieCache() { delete root_; }

    // Insert: keys in [fence_low, fence_high] route to leaf_addr.
    // If the trie is at capacity, evicts 10% before inserting.
    void insert_route(Key fence_low, Key fence_high, GlobalAddress leaf_addr) {
        if (entry_count_.load(std::memory_order_relaxed) >= max_entries_)
            evict(static_cast<int>(max_entries_ / 10));

        uint8_t low_bytes[8], high_bytes[8];
        key_to_bytes(fence_low, low_bytes);
        key_to_bytes(fence_high, high_bytes);

        // Find common prefix depth
        int common_depth = 0;
        while (common_depth < 8 &&
               low_bytes[common_depth] == high_bytes[common_depth])
            ++common_depth;

        // Navigate to the common-prefix node, creating nodes as needed
        TrieNode* node = root_;
        for (int d = 0; d < common_depth; ++d) {
            TrieNode* child = node->get_child(low_bytes[d]);
            if (!child) {
                child = new TrieNode(static_cast<uint8_t>(d + 1));
                node->set_child(low_bytes[d], child);
            }
            // If this intermediate node had a terminal route covering our
            // range, clear it (the new, narrower range supersedes it)
            if (child->is_terminal() &&
                child->route.fence_low <= fence_low &&
                child->route.fence_high >= fence_high) {
                child->route = TrieRouteEntry();
                entry_count_.fetch_sub(1, std::memory_order_relaxed);
            }
            node = child;
        }

        if (common_depth < 8) {
            // Install routing entry at the branching byte
            // For a narrow range that differs only at byte `common_depth`,
            // we install entries for each byte value in [low, high].
            uint8_t start_byte = low_bytes[common_depth];
            uint8_t end_byte   = high_bytes[common_depth];
            for (int b = start_byte; b <= static_cast<int>(end_byte); ++b) {
                TrieNode* child = node->get_child(static_cast<uint8_t>(b));
                if (!child) {
                    child = new TrieNode(static_cast<uint8_t>(common_depth + 1));
                    node->set_child(static_cast<uint8_t>(b), child);
                }
                if (!child->is_terminal()) {
                    entry_count_.fetch_add(1, std::memory_order_relaxed);
                }
                child->route = TrieRouteEntry(leaf_addr, fence_low, fence_high);
                child->freq.store(1, std::memory_order_relaxed);
            }
        } else {
            // All 8 bytes are the same (fence_low == fence_high)
            if (!node->is_terminal()) {
                entry_count_.fetch_add(1, std::memory_order_relaxed);
            }
            node->route = TrieRouteEntry(leaf_addr, fence_low, fence_high);
            node->freq.store(1, std::memory_order_relaxed);
        }
    }

    // Lookup: given key k, find the routing entry.
    // Returns true if a valid route was found.
    bool lookup_route(Key k, GlobalAddress& leaf_addr,
                      Key& fence_low, Key& fence_high) const {
        uint8_t bytes[8];
        key_to_bytes(k, bytes);

        TrieNode* node = root_;
        TrieNode* last_match = nullptr;

        for (int d = 0; d < 8; ++d) {
            // Check if current node is a terminal covering our key
            if (node->is_terminal() &&
                k >= node->route.fence_low &&
                k <= node->route.fence_high) {
                last_match = node;
            }
            TrieNode* child = node->get_child(bytes[d]);
            if (!child) break;
            node = child;
        }

        // Also check the final node we stopped at
        if (node && node->is_terminal() &&
            k >= node->route.fence_low && k <= node->route.fence_high) {
            last_match = node;
        }

        if (last_match) {
            last_match->freq.fetch_add(1, std::memory_order_relaxed);
            leaf_addr  = last_match->route.leaf_addr;
            fence_low  = last_match->route.fence_low;
            fence_high = last_match->route.fence_high;
            return true;
        }
        return false;
    }

    // Simplified lookup returning only the leaf address
    bool lookup_route(Key k, GlobalAddress& leaf_addr) const {
        Key fl, fh;
        return lookup_route(k, leaf_addr, fl, fh);
    }

    // Invalidate all routing entries whose range overlaps [low, high]
    void invalidate_range(Key low, Key high) {
        invalidate_recursive(root_, low, high);
    }

    // Evict `count` least-frequently-used entries
    // Two-random-choice LFU, inspired by CHIME's TreeCache eviction
    void evict(int count) {
        std::vector<TrieNode*> terminals;
        collect_terminals(root_, terminals);
        if (terminals.empty()) return;

        thread_local std::mt19937 rng(std::random_device{}());
        int evicted = 0;
        int attempts = 0;
        while (evicted < count && attempts < count * 4 && !terminals.empty()) {
            size_t idx1 = rng() % terminals.size();
            size_t idx2 = rng() % terminals.size();
            if (idx1 == idx2) { ++attempts; continue; }

            auto* n1 = terminals[idx1];
            auto* n2 = terminals[idx2];
            size_t victim_idx;
            if (n1->freq.load(std::memory_order_relaxed) <=
                n2->freq.load(std::memory_order_relaxed)) {
                victim_idx = idx1;
            } else {
                victim_idx = idx2;
            }
            terminals[victim_idx]->route = TrieRouteEntry(); // invalidate
            // Swap-and-pop to remove from vector
            std::swap(terminals[victim_idx], terminals.back());
            terminals.pop_back();
            ++evicted;
            entry_count_.fetch_sub(1, std::memory_order_relaxed);
            ++attempts;
        }
    }

    uint64_t size() const { return entry_count_.load(std::memory_order_relaxed); }

    void clear() {
        delete root_;
        root_ = new TrieNode(0);
        entry_count_.store(0, std::memory_order_relaxed);
    }

private:
    TrieNode* root_;
    std::atomic<uint64_t> entry_count_{0};
    uint64_t max_entries_;

    void collect_terminals(TrieNode* node, std::vector<TrieNode*>& out) {
        if (!node) return;
        if (node->is_terminal())
            out.push_back(node);
        if (node->children) {
            for (int i = 0; i < TrieNode::FANOUT; ++i) {
                TrieNode* c = node->children[i].load(std::memory_order_relaxed);
                if (c) collect_terminals(c, out);
            }
        }
    }

    void invalidate_recursive(TrieNode* node, Key low, Key high) {
        if (!node) return;
        if (node->is_terminal()) {
            // Overlap check
            if (node->route.fence_high >= low && node->route.fence_low <= high) {
                node->route = TrieRouteEntry();
                entry_count_.fetch_sub(1, std::memory_order_relaxed);
            }
        }
        if (node->children) {
            for (int i = 0; i < TrieNode::FANOUT; ++i) {
                TrieNode* c = node->children[i].load(std::memory_order_relaxed);
                if (c) invalidate_recursive(c, low, high);
            }
        }
    }
};

} // namespace tbc
