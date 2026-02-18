/**
 * CHIMEAdapter.h — Drop-in CHIME Integration for Trie+Bitmap Cache
 *
 * This adapter allows TBC to integrate with CHIME's existing infrastructure:
 *   - Wraps CHIME's TreeCache/IdxCache interface
 *   - Provides same API for CHIME's Tree.cpp to use
 *   - Transparently pulls bitmap to compute side
 *
 * Usage:
 *   Replace CHIME's TreeCache with TBCTreeCache
 *   Replace CHIME's IdxCache with TBCIdxCache
 *
 * The key insight: CHIME's TreeCache + IdxCache + VALOCK bitmap
 * is replaced by: TrieCache + BitmapLeafDirectory (all compute-side)
 */

#pragma once

#include "TrieBitmapCache.h"
#include "GlobalAddress.h"
#include "Common.h"

#include <cstdint>
#include <vector>

namespace tbc {

// -----------------------------------------------------------------------
// TBCTreeCache: Replaces CHIME's TreeCache (skip-list based)
//
// CHIME's TreeCache API:
//   - add_to_cache(InternalNode*)
//   - search_from_cache(Key, addr, sibling_addr, level)
//   - invalidate(TreeCacheEntry*)
//
// TBC equivalent: TrieCache for routing + instant lookup
// -----------------------------------------------------------------------
class TBCTreeCache {
public:
    TBCTreeCache(uint64_t cache_mb, void* dsm)
        : trie_cache_(cache_mb * 1024) {
        (void)dsm;
    }

    // Add routing for [fence_low, fence_high] → leaf_addr
    // In CHIME this would be called with an InternalNode*;
    // we extract the fence keys and routing info
    bool add_to_cache(Key fence_low, Key fence_high, GlobalAddress leaf_addr) {
        trie_cache_.insert_route(fence_low, fence_high, leaf_addr);
        return true;
    }

    // Search: returns true if routing found
    // addr = leaf address, level = 0 (we route directly to leaf)
    bool search_from_cache(Key k, GlobalAddress& addr, 
                           GlobalAddress& sibling_addr, uint16_t& level) {
        Key fl, fh;
        sibling_addr = GlobalAddress::Null();
        level = 0;  // Always leaf level (trie routes directly)
        return trie_cache_.lookup_route(k, addr, fl, fh);
    }

    // Pointer-based search (for CHIME compatibility)
    bool search_ptr_from_cache(Key k, GlobalAddress& addr, uint16_t level) {
        (void)level;  // TBC doesn't use level-by-level lookup
        return trie_cache_.lookup_route(k, addr);
    }

    void invalidate(Key fence_low, Key fence_high) {
        trie_cache_.invalidate_range(fence_low, fence_high);
    }

    void clear() { trie_cache_.clear(); }

    uint64_t size() const { return trie_cache_.size(); }

    void statistics() {
        printf("[TBCTreeCache] Trie entries: %lu\n", trie_cache_.size());
    }

private:
    TrieCache trie_cache_;
};

// -----------------------------------------------------------------------
// TBCIdxCache: Replaces CHIME's IdxCache (hash-table based)
//
// CHIME's IdxCache caches {leaf_addr, kv_idx} → fingerprint for
// speculative reads. The entire purpose is to skip RDMA reads.
//
// In TBC, we cache the entire leaf page locally (BitmapLeafDirectory),
// so IdxCache becomes unnecessary — we just search the local page!
//
// This class provides the same API but always returns "miss",
// deferring to BitmapLeafDirectory's local page search.
// -----------------------------------------------------------------------
class TBCIdxCache {
public:
    TBCIdxCache(uint64_t cache_mb, void* dsm) {
        (void)cache_mb;
        (void)dsm;
    }

    // Always return false (defer to local page search)
    bool add_to_cache(GlobalAddress leaf_addr, int kv_idx, Key k) {
        (void)leaf_addr; (void)kv_idx; (void)k;
        return false;  // No-op; we cache whole pages instead
    }

    bool search_idx_from_cache(GlobalAddress leaf_addr, int l_idx, int r_idx,
                               Key k, int& kv_idx) {
        (void)leaf_addr; (void)l_idx; (void)r_idx; (void)k; (void)kv_idx;
        return false;  // Always miss → caller will search local page
    }

    void statistics() {
        printf("[TBCIdxCache] (disabled — using BitmapLeafDirectory instead)\n");
    }
};

// -----------------------------------------------------------------------
// LocalLeafSearch: Search a locally cached leaf page
//
// Since TBC caches entire pages in BitmapLeafDirectory, this helper
// performs binary search on the cached data — ZERO RDMA!
// -----------------------------------------------------------------------
template<typename LeafType>
class LocalLeafSearch {
public:
    // Search cached page for key k
    // Returns (found, kv_idx, value)
    static bool search(const void* cached_page, Key k, uint64_t& value) {
        auto* leaf = reinterpret_cast<const LeafType*>(cached_page);
        
        // Binary search (same as CHIME/DEX leaf->lowerBound + check)
        unsigned lower = 0;
        unsigned upper = leaf->count;
        
        while (lower < upper) {
            unsigned mid = lower + (upper - lower) / 2;
            Key mid_key = leaf->data[mid].first;
            
            if (k < mid_key) {
                upper = mid;
            } else if (k > mid_key) {
                lower = mid + 1;
            } else {
                value = leaf->data[mid].second;
                return true;
            }
        }
        return false;
    }

    // Get vacancy bitmap from leaf
    static uint64_t get_vacancy_bitmap(const void* cached_page) {
        auto* leaf = reinterpret_cast<const LeafType*>(cached_page);
        uint64_t bitmap = 0;
        for (unsigned i = 0; i < leaf->count && i < 64; ++i)
            bitmap |= (1ULL << i);
        return bitmap;
    }
};

// -----------------------------------------------------------------------
// ChimeCompatibleCache: Full CHIME-compatible interface
//
// Provides the combined functionality of:
//   - TreeCache (routing) → TrieCache
//   - IdxCache (idx hints) → disabled; use local search
//   - VALOCK bitmap       → BitmapLeafDirectory (on compute side!)
// -----------------------------------------------------------------------
class ChimeCompatibleCache {
public:
    ChimeCompatibleCache(uint64_t cache_mb, void* dsm)
        : underlying_(cache_mb, reinterpret_cast<DSM*>(dsm)),
          tree_cache_(cache_mb, dsm),
          idx_cache_(cache_mb, dsm) {}

    // ---- TreeCache-compatible methods ----
    bool add_routing(Key fence_low, Key fence_high, GlobalAddress addr) {
        return tree_cache_.add_to_cache(fence_low, fence_high, addr);
    }

    bool lookup_routing(Key k, GlobalAddress& addr) {
        GlobalAddress sibling;
        uint16_t level;
        return tree_cache_.search_from_cache(k, addr, sibling, level);
    }

    // ---- Leaf page caching (pulls bitmap to compute side) ----
    void cache_leaf_page(GlobalAddress addr, const void* page_data,
                         Key fence_low, Key fence_high, uint64_t vacancy = 0) {
        underlying_.cache_leaf(addr, page_data, fence_low, fence_high, vacancy);
    }

    // Returns pointer to locally cached page (or nullptr)
    void* get_cached_leaf(GlobalAddress addr, uint64_t* vacancy = nullptr,
                          Key* fence_low = nullptr, Key* fence_high = nullptr) {
        return underlying_.get_bitmap().lookup(addr, vacancy, fence_low, fence_high);
    }

    // ---- Full lookup (trie + bitmap) ----
    TrieBitmapCache::CacheLookupResult full_lookup(Key k) {
        return underlying_.lookup(k);
    }

    // ---- Invalidation ----
    void invalidate_routing(Key fence_low, Key fence_high) {
        tree_cache_.invalidate(fence_low, fence_high);
    }

    void invalidate_leaf(GlobalAddress addr) {
        underlying_.invalidate_leaf(addr);
    }

    void invalidate_all(Key fence_low, Key fence_high, GlobalAddress addr) {
        underlying_.invalidate(fence_low, fence_high, addr);
    }

    // ---- Statistics ----
    void print_statistics() {
        underlying_.print_statistics();
    }

    void clear_statistics() {
        underlying_.clear_statistics();
    }

    void reset() {
        underlying_.reset();
    }

    // ---- Access underlying components ----
    TrieBitmapCache& get_tbc() { return underlying_; }
    TBCTreeCache& get_tree_cache() { return tree_cache_; }
    TBCIdxCache& get_idx_cache() { return idx_cache_; }

private:
    TrieBitmapCache underlying_;
    TBCTreeCache    tree_cache_;
    TBCIdxCache     idx_cache_;
};

}  // namespace tbc

// -----------------------------------------------------------------------
// Macro for drop-in CHIME replacement
// -----------------------------------------------------------------------
#ifdef USE_TBC_CACHE
    #define TreeCache    tbc::TBCTreeCache
    #define IdxCache     tbc::TBCIdxCache
#endif
