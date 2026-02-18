/**
 * TrieBitmapCache.h — Integrated Compute-Side Cache Manager
 *
 * Orchestrates the two new components:
 *   1. TrieCache   — fast key → leaf routing  (replaces CHIME's skip-list TreeCache)
 *   2. BitmapLeafDirectory — leaf page cache   (pulls CHIME's memory-side bitmap to compute)
 *
 * Lookup flow:
 *   key → TrieCache → leaf_addr?
 *     YES → BitmapLeafDirectory → cached_page?
 *              YES → local search  (ZERO RDMA)
 *              NO  → single RDMA read of leaf, cache it
 *     NO  → slow-path B-tree traversal, update trie + bitmap
 *
 * Compared to CHIME:
 *   CHIME: skip-list lookup → RDMA read VALOCK bitmap → RDMA read leaf entries
 *   TBC:   trie lookup → local bitmap check → local search   (0 RDMA on hot path)
 *
 * Compared to DEX:
 *   DEX: swizzle-check per inner level → walk inner nodes → cache-get → RDMA miss
 *   TBC: single trie walk → direct to leaf → local or 1 RDMA
 *
 * Reuses: DSM (DEX), Key/Value/GlobalAddress (DEX Common.h)
 */

#pragma once

#include "TrieNode.h"
#include "BitmapLeafDirectory.h"
#include "DSM.h"

namespace tbc {

// Page size from DEX's Common.h
static constexpr uint64_t kPageSizeForCache = kLeafPageSize;

class TrieBitmapCache {
public:
    // Result of a cache lookup
    struct CacheLookupResult {
        GlobalAddress leaf_addr;    // Remote leaf address (from trie)
        void*         cached_page;  // Local page copy (from bitmap dir)
        bool          trie_hit;     // Did the trie know where the leaf is?
        bool          bitmap_hit;   // Is the leaf page cached locally?
        uint64_t      vacancy;      // Vacancy bitmap of the cached leaf
        Key           fence_low;
        Key           fence_high;
    };

    TrieBitmapCache(uint64_t cache_mb, DSM* dsm)
        : trie_(cache_mb * 1024),   // max trie entries proportional to cache
          bitmap_dir_(cache_mb * define::MB, kPageSizeForCache),
          dsm_(dsm) {}

    ~TrieBitmapCache() = default;

    // ---------------------------------------------------------------
    // Fast-path lookup:  trie → bitmap → local page pointer
    // ---------------------------------------------------------------
    CacheLookupResult lookup(Key k) {
        CacheLookupResult r{};
        r.leaf_addr   = GlobalAddress::Null();
        r.cached_page = nullptr;
        r.trie_hit    = false;
        r.bitmap_hit  = false;
        r.vacancy     = 0;

        total_lookups_.fetch_add(1, std::memory_order_relaxed);

        // Step 1: Trie routing
        Key fl, fh;
        if (trie_.lookup_route(k, r.leaf_addr, fl, fh)) {
            r.trie_hit   = true;
            r.fence_low  = fl;
            r.fence_high = fh;
            trie_hits_.fetch_add(1, std::memory_order_relaxed);

            // Step 2: Bitmap directory check for cached leaf page
            uint64_t vac = 0;
            Key bfl = 0, bfh = 0;
            void* page = bitmap_dir_.lookup(r.leaf_addr, &vac, &bfl, &bfh);
            if (page) {
                r.cached_page = page;
                r.bitmap_hit  = true;
                r.vacancy     = vac;
                // Use more precise fence keys from bitmap if available
                if (bfl != 0 || bfh != 0) {
                    r.fence_low  = bfl;
                    r.fence_high = bfh;
                }
            }
        } else {
            trie_misses_.fetch_add(1, std::memory_order_relaxed);
        }

        return r;
    }

    // ---------------------------------------------------------------
    // Cache a leaf after RDMA read.  Updates both trie and bitmap.
    // vacancy_bitmap is extracted from the leaf page (pulled from
    // memory node to compute side — the core of our optimization).
    // ---------------------------------------------------------------
    void cache_leaf(GlobalAddress leaf_addr, const void* page_data,
                    Key fence_low, Key fence_high,
                    uint64_t vacancy_bitmap = 0) {
        trie_.insert_route(fence_low, fence_high, leaf_addr);
        bitmap_dir_.insert(leaf_addr, page_data,
                           vacancy_bitmap, fence_low, fence_high);
    }

    // Update only the trie routing (no leaf page to cache yet)
    void update_routing(Key fence_low, Key fence_high,
                        GlobalAddress leaf_addr) {
        trie_.insert_route(fence_low, fence_high, leaf_addr);
    }

    // Invalidate: remove from both trie and bitmap
    void invalidate(Key fence_low, Key fence_high,
                    GlobalAddress leaf_addr) {
        trie_.invalidate_range(fence_low, fence_high);
        bitmap_dir_.invalidate(leaf_addr);
    }

    // Invalidate just the cached leaf page (trie route may still be valid)
    void invalidate_leaf(GlobalAddress leaf_addr) {
        bitmap_dir_.invalidate(leaf_addr);
    }

    // ---------------------------------------------------------------
    // Statistics
    // ---------------------------------------------------------------
    void print_statistics() {
        auto total = total_lookups_.load();
        auto th = trie_hits_.load();
        auto tm = trie_misses_.load();
        auto bh = bitmap_dir_.hit_count();
        auto bm = bitmap_dir_.miss_count();
        printf("======= TBC Cache Statistics =======\n");
        printf("Total lookups      : %lu\n", total);
        printf("Trie hits          : %lu (%.1f%%)\n", th,
               total > 0 ? 100.0 * th / total : 0.0);
        printf("Trie misses        : %lu\n", tm);
        printf("Bitmap cache hits  : %lu (%.1f%% of trie hits)\n", bh,
               th > 0 ? 100.0 * bh / th : 0.0);
        printf("Bitmap misses      : %lu\n", bm);
        printf("Cached leaf pages  : %lu\n", bitmap_dir_.cached_count());
        printf("Trie route entries : %lu\n", trie_.size());
        printf("====================================\n");
    }

    void clear_statistics() {
        total_lookups_.store(0);
        trie_hits_.store(0);
        trie_misses_.store(0);
        bitmap_dir_.clear_statistics();
    }

    void reset() {
        trie_.clear();
        bitmap_dir_.reset();
        clear_statistics();
    }

    TrieCache&           get_trie()   { return trie_; }
    BitmapLeafDirectory& get_bitmap() { return bitmap_dir_; }

private:
    TrieCache           trie_;
    BitmapLeafDirectory bitmap_dir_;
    DSM*                dsm_;

    std::atomic<uint64_t> total_lookups_{0};
    std::atomic<uint64_t> trie_hits_{0};
    std::atomic<uint64_t> trie_misses_{0};
};

} // namespace tbc
