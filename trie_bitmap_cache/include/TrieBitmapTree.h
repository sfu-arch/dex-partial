/**
 * TrieBitmapTree.h — B+-Tree with Compute-Side Trie+Bitmap Cache
 *
 * Implements tree_api<Key, Value> (DEX's benchmark interface) so it can be
 * benchmarked head-to-head against DEX and CHIME in the same harness.
 *
 * Architecture (vs DEX and CHIME):
 *
 *   DEX:   per-level OLC → swizzle check → CacheManager (LeanStore buffer pool)
 *          Cache miss: sample → HOT→COOLING→COLD state machine
 *          Inner nodes cached via pointer swizzling in parent
 *
 *   CHIME: TreeCache (skip-list) → IdxCache (hash table)
 *          Memory-side VALOCK vacancy bitmap must be RDMA-read each time
 *          Inner nodes cached in skip-list keyed by fence-key ranges
 *
 *   TBC (this):
 *          TrieCache → key→leaf in O(key_bytes), no per-level RDMA
 *          BitmapLeafDirectory → leaf cached locally with vacancy state
 *          Hot path: trie hit + bitmap hit → ZERO RDMA reads
 *          Cold path: full B-tree traversal (builds up trie for next time)
 *
 * Remote B-tree node format: DEX's BTreeInner<Key> / BTreeLeaf<Key,Value>
 * RDMA operations: DEX's DSM class (read_sync, write_sync, alloc, etc.)
 */

#pragma once

#include "TrieBitmapCache.h"
#include "tree_api.h"
#include "cache/btree_node.h"   // DEX: NodeBase, BTreeInner<Key>, BTreeLeaf<Key,Value>
#include "DSM.h"
#include "Timer.h"

#include <algorithm>
#include <cstring>
#include <iostream>
#include <vector>

namespace tbc {

// Pull DEX's B-tree node types into our namespace for convenience
using cachepush::PageType;
using cachepush::NodeBase;
using cachepush::BTreeInner;
using cachepush::BTreeLeaf;
using cachepush::pageSize;
using cachepush::swizzle_tag;

class TrieBitmapTree : public tree_api<Key, Value> {
public:
    // -----------------------------------------------------------------
    // Construction: allocate an empty root leaf on the memory node
    // -----------------------------------------------------------------
    TrieBitmapTree(DSM* dsm, uint64_t tree_id, uint64_t cache_mb)
        : cache_(cache_mb, dsm), dsm_(dsm), tree_id_(tree_id) {
        // Root pointer location in remote memory (same convention as DEX)
        root_ptr_ptr_ = GlobalAddress(
            0, define::kRootPointerStoreOffest +
                   tree_id * sizeof(GlobalAddress));

        // Allocate root leaf on memory node 0
        GlobalAddress root_leaf_addr = dsm_->alloc(pageSize);

        // Initialize root as empty leaf
        char* buf = get_page_buffer();
        auto* leaf = new (buf) BTreeLeaf<Key, Value>(root_leaf_addr);
        leaf->level   = 0;
        leaf->count   = 0;
        leaf->min_limit_ = std::numeric_limits<Key>::min();
        leaf->max_limit_ = std::numeric_limits<Key>::max();

        dsm_->write_sync(buf, root_leaf_addr, pageSize);

        // Store root pointer
        root_ = root_leaf_addr;
        auto* cas_buf = reinterpret_cast<char*>(dsm_->get_rbuf(0).get_cas_buffer());
        *reinterpret_cast<GlobalAddress*>(cas_buf) = root_;
        dsm_->write_sync(cas_buf, root_ptr_ptr_, sizeof(GlobalAddress));

        height_ = 1;
        printf("[TBC] Root created at [%u, %lu].  cache=%luMB\n",
               (unsigned)root_.nodeID, root_.offset, cache_mb);
    }

    // With partition information (matches DEX's constructor signature)
    TrieBitmapTree(DSM* dsm, uint64_t tree_id, uint64_t cache_mb,
                   std::vector<Key> partition_info, int num_partitions)
        : TrieBitmapTree(dsm, tree_id, cache_mb) {
        // Partitioning is handled by the benchmark driver; tree just
        // needs to know its bounds (set_bound is called after bulk_load).
        (void)partition_info;
        (void)num_partitions;
    }

    // =================================================================
    //  tree_api<Key, Value> interface
    // =================================================================

    // -----------------------------------------------------------------
    // LOOKUP — the core operation where trie+bitmap shines
    // -----------------------------------------------------------------
    bool lookup(Key k, Value& result) override {
        int restarts = 0;
    restart:
        if (restarts++ > 128) return false;

        // --- Fast path: trie + bitmap ---
        auto cr = cache_.lookup(k);

        if (cr.trie_hit && cr.bitmap_hit) {
            // Both hit → search cached leaf locally (ZERO RDMA)
            auto* leaf = reinterpret_cast<BTreeLeaf<Key, Value>*>(cr.cached_page);
            if (leaf->rangeValid(k) && leaf->check_consistent()) {
                return leaf->find(k, result);
            }
            // Stale cache → invalidate, fall through
            cache_.invalidate_leaf(cr.leaf_addr);
        }

        if (cr.trie_hit && !cr.bitmap_hit) {
            // Trie hit, leaf not cached → single RDMA read
            char* buf = get_page_buffer();
            dsm_->read_sync(buf, cr.leaf_addr, pageSize);
            auto* leaf = reinterpret_cast<BTreeLeaf<Key, Value>*>(buf);

            if (!leaf->check_consistent() || !leaf->rangeValid(k)) {
                cache_.invalidate(leaf->min_limit_, leaf->max_limit_,
                                  cr.leaf_addr);
                goto restart;
            }

            // Pull vacancy bitmap to compute side & cache the leaf
            uint64_t vacancy = extract_vacancy(leaf);
            cache_.cache_leaf(cr.leaf_addr, buf,
                              leaf->min_limit_, leaf->max_limit_, vacancy);
            return leaf->find(k, result);
        }

        // --- Slow path: full B-tree traversal ---
        return slow_path_lookup(k, result);
    }

    // -----------------------------------------------------------------
    // INSERT
    // -----------------------------------------------------------------
    bool insert(Key k, Value v) override {
        int restarts = 0;
    restart:
        if (restarts++ > 128) return false;

        // Find the target leaf
        GlobalAddress leaf_addr;
        if (!find_leaf_for_key(k, leaf_addr))
            goto restart;

        {
            // Read the leaf
            char* buf = get_page_buffer();
            dsm_->read_sync(buf, leaf_addr, pageSize);
            auto* leaf = reinterpret_cast<BTreeLeaf<Key, Value>*>(buf);

            if (!leaf->check_consistent() || !leaf->rangeValid(k)) {
                cache_.invalidate(leaf->min_limit_, leaf->max_limit_,
                                  leaf_addr);
                goto restart;
            }

            if (leaf->isFull()) {
                // Need to split
                return handle_leaf_split(k, v, leaf, leaf_addr);
            }

            bool inserted = leaf->insert(k, v);
            dsm_->write_sync(buf, leaf_addr, pageSize);

            // Update compute-side cache
            uint64_t vacancy = extract_vacancy(leaf);
            cache_.cache_leaf(leaf_addr, buf,
                              leaf->min_limit_, leaf->max_limit_, vacancy);
            return inserted;
        }
    }

    // -----------------------------------------------------------------
    // UPDATE
    // -----------------------------------------------------------------
    bool update(Key k, Value v) override {
        int restarts = 0;
    restart:
        if (restarts++ > 128) return false;

        GlobalAddress leaf_addr;
        if (!find_leaf_for_key(k, leaf_addr))
            goto restart;

        {
            char* buf = get_page_buffer();
            dsm_->read_sync(buf, leaf_addr, pageSize);
            auto* leaf = reinterpret_cast<BTreeLeaf<Key, Value>*>(buf);

            if (!leaf->check_consistent() || !leaf->rangeValid(k)) {
                cache_.invalidate(leaf->min_limit_, leaf->max_limit_,
                                  leaf_addr);
                goto restart;
            }

            bool ok = leaf->update(k, v);
            if (ok) {
                dsm_->write_sync(buf, leaf_addr, pageSize);
                uint64_t vacancy = extract_vacancy(leaf);
                cache_.cache_leaf(leaf_addr, buf,
                                  leaf->min_limit_, leaf->max_limit_, vacancy);
            }
            return ok;
        }
    }

    // -----------------------------------------------------------------
    // REMOVE
    // -----------------------------------------------------------------
    bool remove(Key k) override {
        int restarts = 0;
    restart:
        if (restarts++ > 128) return false;

        GlobalAddress leaf_addr;
        if (!find_leaf_for_key(k, leaf_addr))
            goto restart;

        {
            char* buf = get_page_buffer();
            dsm_->read_sync(buf, leaf_addr, pageSize);
            auto* leaf = reinterpret_cast<BTreeLeaf<Key, Value>*>(buf);

            if (!leaf->check_consistent() || !leaf->rangeValid(k)) {
                cache_.invalidate(leaf->min_limit_, leaf->max_limit_,
                                  leaf_addr);
                goto restart;
            }

            bool ok = leaf->remove(k);
            if (ok) {
                dsm_->write_sync(buf, leaf_addr, pageSize);
                uint64_t vacancy = extract_vacancy(leaf);
                cache_.cache_leaf(leaf_addr, buf,
                                  leaf->min_limit_, leaf->max_limit_, vacancy);
            }
            return ok;
        }
    }

    // -----------------------------------------------------------------
    // RANGE SCAN
    // -----------------------------------------------------------------
    int range_scan(Key k, uint32_t num,
                   std::pair<Key, Value>*& kv_buffer) override {
        GlobalAddress leaf_addr;
        if (!find_leaf_for_key(k, leaf_addr))
            return 0;

        uint32_t total_scanned = 0;

        while (total_scanned < num &&
               leaf_addr != GlobalAddress::Null()) {
            char* buf = get_page_buffer();
            dsm_->read_sync(buf, leaf_addr, pageSize);
            auto* leaf = reinterpret_cast<BTreeLeaf<Key, Value>*>(buf);

            if (!leaf->check_consistent()) break;

            // Cache the leaf (pull bitmap to compute side)
            uint64_t vacancy = extract_vacancy(leaf);
            cache_.cache_leaf(leaf_addr, buf,
                              leaf->min_limit_, leaf->max_limit_, vacancy);

            auto cnt = leaf->range_scan(k, num - total_scanned, kv_buffer);
            total_scanned += cnt;
            kv_buffer += cnt;

            leaf_addr = leaf->next_leaf;
            // Strip swizzle tag if present
            leaf_addr.val &= ~swizzle_tag;
            k = leaf->max_limit_;
        }

        return static_cast<int>(total_scanned);
    }

    // -----------------------------------------------------------------
    // BULK LOAD — sequential insertions
    // -----------------------------------------------------------------
    void bulk_load(Key* keys, uint64_t num) override {
        std::sort(keys, keys + num);
        for (uint64_t i = 0; i < num; ++i) {
            insert(keys[i], keys[i] + 1);
        }
        printf("[TBC] Bulk load complete: %lu keys, height=%d\n", num, height_);
    }

    // -----------------------------------------------------------------
    // Miscellaneous tree_api methods
    // -----------------------------------------------------------------
    void clear_statistic() override { cache_.clear_statistics(); }
    void get_statistic()   override { cache_.print_statistics(); }

    void set_shared(std::vector<Key>& /*bound*/) override {}
    void set_bound(Key left, Key right) override {
        left_bound_  = left;
        right_bound_ = right;
    }

    void get_newest_root() override {
        char* buf = reinterpret_cast<char*>(dsm_->get_rbuf(0).get_cas_buffer());
        dsm_->read_sync(buf, root_ptr_ptr_, sizeof(GlobalAddress));
        root_ = *reinterpret_cast<GlobalAddress*>(buf);
    }

    void reset_buffer_pool(bool /*flush_dirty*/) override {
        cache_.reset();
    }

    void validate()  override {}
    void flush_all() override {}

    void get_basic() override {
        printf("[TBC] height=%d  root=[%u,%lu]  trie_entries=%lu  cached_leaves=%lu\n",
               height_, (unsigned)root_.nodeID, root_.offset,
               cache_.get_trie().size(), cache_.get_bitmap().cached_count());
    }

    // ================================================================
private:
    TrieBitmapCache cache_;
    DSM*            dsm_;
    uint64_t        tree_id_;

    GlobalAddress root_;
    GlobalAddress root_ptr_ptr_;
    int           height_ = 1;

    Key left_bound_  = 0;
    Key right_bound_ = std::numeric_limits<Key>::max();

    // Per-thread RDMA buffer accessors (from DEX's RdmaBuffer)
    char* get_page_buffer()    { return dsm_->get_rbuf(0).get_page_buffer(); }
    char* get_sibling_buffer() { return dsm_->get_rbuf(0).get_sibling_buffer(); }

    // -----------------------------------------------------------------
    // Extract "vacancy" info from a leaf — what CHIME stores on the mem
    // node in VALOCK, we now compute on the fly and cache locally.
    // -----------------------------------------------------------------
    static uint64_t extract_vacancy(const BTreeLeaf<Key, Value>* leaf) {
        // Build a bitmap: bit i set if slot i is occupied.
        // BTreeLeaf stores sorted KV pairs in data[0..count-1].
        uint64_t bmp = 0;
        for (unsigned i = 0; i < leaf->count && i < 64; ++i)
            bmp |= (1ULL << i);
        return bmp;
    }

    // -----------------------------------------------------------------
    // Find the leaf GlobalAddress for a given key.
    // Fast path via trie; slow path via B-tree traversal.
    // -----------------------------------------------------------------
    bool find_leaf_for_key(Key k, GlobalAddress& leaf_addr) {
        auto cr = cache_.lookup(k);
        if (cr.trie_hit) {
            leaf_addr = cr.leaf_addr;
            return true;
        }
        // Slow path: traverse
        return traverse_to_leaf(k, leaf_addr);
    }

    // -----------------------------------------------------------------
    // Slow-path traversal: RDMA-read inner nodes one at a time from
    // root to leaf.  Populates the trie cache along the way so future
    // lookups are fast.
    // -----------------------------------------------------------------
    bool traverse_to_leaf(Key k, GlobalAddress& leaf_addr) {
        char* buf = get_sibling_buffer();
        GlobalAddress cur = root_;

        for (int level = 0; level < 20; ++level) {  // max depth guard
            dsm_->read_sync(buf, cur, pageSize);
            auto* base = reinterpret_cast<NodeBase*>(buf);

            if (base->type == PageType::BTreeLeaf) {
                auto* leaf = reinterpret_cast<BTreeLeaf<Key, Value>*>(buf);
                if (!leaf->check_consistent()) return false;

                leaf_addr = cur;
                // Populate trie + bitmap for this leaf
                uint64_t vacancy = extract_vacancy(leaf);
                cache_.cache_leaf(cur, buf,
                                  leaf->min_limit_, leaf->max_limit_, vacancy);
                return true;
            }

            // Inner node
            auto* inner = reinterpret_cast<BTreeInner<Key>*>(buf);
            if (!inner->check_consistent()) return false;

            unsigned idx = inner->lowerBound(k);
            cur = inner->children[idx];
            cur.val &= ~swizzle_tag;   // strip DEX swizzle tag if present
        }
        return false;  // Should not reach here
    }

    // -----------------------------------------------------------------
    // Slow-path lookup: traverse + search leaf
    // -----------------------------------------------------------------
    bool slow_path_lookup(Key k, Value& result) {
        GlobalAddress leaf_addr;
        if (!traverse_to_leaf(k, leaf_addr)) return false;

        // The leaf was already cached by traverse_to_leaf, but the
        // buffer may have been reused.  Read via bitmap cache first.
        auto cr = cache_.lookup(k);
        if (cr.bitmap_hit) {
            auto* leaf = reinterpret_cast<BTreeLeaf<Key, Value>*>(cr.cached_page);
            return leaf->find(k, result);
        }

        // Fallback: re-read from remote
        char* buf = get_page_buffer();
        dsm_->read_sync(buf, leaf_addr, pageSize);
        auto* leaf = reinterpret_cast<BTreeLeaf<Key, Value>*>(buf);
        return leaf->find(k, result);
    }

    // -----------------------------------------------------------------
    // Leaf split handling
    // -----------------------------------------------------------------
    bool handle_leaf_split(Key k, Value v,
                           BTreeLeaf<Key, Value>* full_leaf,
                           GlobalAddress leaf_addr) {
        // Allocate new sibling leaf
        GlobalAddress new_addr = dsm_->alloc(pageSize);

        char* left_buf  = get_page_buffer();
        char* right_buf = get_sibling_buffer();

        // Copy the full leaf into left_buf
        memcpy(left_buf, full_leaf, pageSize);
        auto* left  = reinterpret_cast<BTreeLeaf<Key, Value>*>(left_buf);
        auto* right = new (right_buf) BTreeLeaf<Key, Value>(new_addr);
        right->level = 0;

        Key separator;
        left->split(separator, right, new_addr);

        // Insert key into correct side
        if (k <= separator)
            left->insert(k, v);
        else
            right->insert(k, v);

        // Write both leaves to remote
        dsm_->write_sync(left_buf,  leaf_addr, pageSize);
        dsm_->write_sync(right_buf, new_addr,  pageSize);

        // Invalidate old routing, cache both new leaves
        cache_.invalidate(full_leaf->min_limit_, full_leaf->max_limit_,
                          leaf_addr);
        cache_.cache_leaf(leaf_addr, left_buf,
                          left->min_limit_, left->max_limit_,
                          extract_vacancy(left));
        cache_.cache_leaf(new_addr, right_buf,
                          right->min_limit_, right->max_limit_,
                          extract_vacancy(right));

        // Insert separator into parent
        insert_into_parent(leaf_addr, separator, new_addr);
        return true;
    }

    // -----------------------------------------------------------------
    // Insert separator key into parent inner node
    // -----------------------------------------------------------------
    void insert_into_parent(GlobalAddress left_child,
                            Key separator,
                            GlobalAddress right_child) {
        if (height_ == 1) {
            create_new_root(left_child, separator, right_child);
            return;
        }

        // Traverse from root to find the parent containing left_child
        char* buf = get_page_buffer();
        GlobalAddress cur = root_;

        for (int depth = 0; depth < 20; ++depth) {
            dsm_->read_sync(buf, cur, pageSize);
            auto* base = reinterpret_cast<NodeBase*>(buf);
            if (base->type == PageType::BTreeLeaf) break;

            auto* inner = reinterpret_cast<BTreeInner<Key>*>(buf);

            // Check if left_child is a child of this inner node
            for (int i = 0; i <= static_cast<int>(inner->count); ++i) {
                GlobalAddress child = inner->children[i];
                child.val &= ~swizzle_tag;
                if (child == left_child) {
                    if (inner->isFull()) {
                        split_inner_and_insert(cur, inner, separator,
                                               right_child);
                    } else {
                        inner->insert(separator, right_child);
                        dsm_->write_sync(buf, cur, pageSize);
                    }
                    return;
                }
            }

            // Route deeper
            unsigned idx = inner->lowerBound(separator);
            cur = inner->children[idx];
            cur.val &= ~swizzle_tag;
        }
    }

    void create_new_root(GlobalAddress left, Key sep, GlobalAddress right) {
        GlobalAddress new_root = dsm_->alloc(pageSize);
        char* buf = get_page_buffer();

        auto* root = new (buf) BTreeInner<Key>(
            static_cast<uint8_t>(height_), new_root);
        root->keys[0]     = sep;
        root->children[0] = left;
        root->children[1] = right;
        root->count        = 1;
        root->min_limit_   = std::numeric_limits<Key>::min();
        root->max_limit_   = std::numeric_limits<Key>::max();

        dsm_->write_sync(buf, new_root, pageSize);

        // Update root pointer in remote memory
        root_ = new_root;
        auto* cas_buf = reinterpret_cast<char*>(
            dsm_->get_rbuf(0).get_cas_buffer());
        *reinterpret_cast<GlobalAddress*>(cas_buf) = root_;
        dsm_->write_sync(cas_buf, root_ptr_ptr_, sizeof(GlobalAddress));

        height_++;
        printf("[TBC] New root → height=%d\n", height_);
    }

    void split_inner_and_insert(GlobalAddress inner_addr,
                                BTreeInner<Key>* full_inner,
                                Key sep, GlobalAddress right_child) {
        GlobalAddress new_addr = dsm_->alloc(pageSize);

        char* left_buf  = get_page_buffer();
        char* right_buf = get_sibling_buffer();

        memcpy(left_buf, full_inner, pageSize);
        auto* left  = reinterpret_cast<BTreeInner<Key>*>(left_buf);
        auto* right = new (right_buf) BTreeInner<Key>(
            left->level, new_addr);

        Key inner_sep;
        left->split(inner_sep, right);

        if (sep < inner_sep)
            left->insert(sep, right_child);
        else
            right->insert(sep, right_child);

        dsm_->write_sync(left_buf,  inner_addr, pageSize);
        dsm_->write_sync(right_buf, new_addr,   pageSize);

        insert_into_parent(inner_addr, inner_sep, new_addr);
    }
};

} // namespace tbc
