/**
 * BitmapLeafDirectory.h — Compute-Side Bitmap for Leaf Page Caching
 *
 * In CHIME, the vacancy bitmap lives on the memory node (inside VALOCK),
 * forcing an RDMA round-trip just to check which leaf slots are occupied.
 *
 * This component pulls that bitmap to the compute node. For each B-tree leaf
 * whose page is cached locally, we maintain:
 *   - A bitmap tracking which cache slots are occupied  (CHIME's Bitmap.h pattern)
 *   - The leaf's vacancy state (which KV slots have data)
 *   - A pointer to the local page copy
 *
 * Structure: N-way set-associative directory.
 *   hash(leaf_addr) → set → scan WAYS slots → bitmap-based free-slot finding
 *
 * Reuses from DEX:  Key, Value, GlobalAddress, pageSize, HugePageAlloc
 * Adapts from CHIME: Bitmap free-slot finding (__builtin_ctzll pattern),
 *                     LFU two-random-choice eviction (TreeCache/IdxCache)
 */

#pragma once

#include "Common.h"
#include "GlobalAddress.h"
#include "HugePageAlloc.h"

#include <atomic>
#include <cstring>
#include <cstdint>
#include <random>

namespace tbc {

// Page size for B-tree nodes (from DEX's Common.h: kLeafPageSize = 1024)
static constexpr uint64_t kPageSize = kLeafPageSize;

// -------------------------------------------------------------------
// CachedLeafMeta: per-slot metadata for a cached leaf page.
// Mirrors what CHIME's VALOCK tracks, but on the compute side.
// -------------------------------------------------------------------
struct CachedLeafMeta {
    GlobalAddress leaf_addr;       // Remote leaf GlobalAddress
    void*         local_page;      // Pointer to local page copy
    uint64_t      vacancy_bitmap;  // Which KV slots are occupied (from leaf)
    Key           fence_low;       // Lower fence key (inclusive)
    Key           fence_high;      // Upper fence key (inclusive)
    std::atomic<int32_t> freq{0};  // LFU access frequency
    uint32_t      version;         // Remote version for staleness detection

    CachedLeafMeta()
        : leaf_addr(GlobalAddress::Null()), local_page(nullptr),
          vacancy_bitmap(0), fence_low(0), fence_high(0),
          version(0) {}

    bool is_valid() const { return leaf_addr != GlobalAddress::Null(); }

    void reset() {
        leaf_addr = GlobalAddress::Null();
        local_page = nullptr;
        vacancy_bitmap = 0;
        fence_low = 0;
        fence_high = 0;
        freq.store(0, std::memory_order_relaxed);
        version = 0;
    }
};

// -------------------------------------------------------------------
// BitmapLeafDirectory: set-associative leaf page cache with bitmap
// occupancy tracking.  Uses CHIME's __builtin_ctzll pattern for
// fast free-slot discovery.
// -------------------------------------------------------------------
class BitmapLeafDirectory {
public:
    static constexpr int WAYS = 8;  // 8-way set associativity

    struct alignas(64) CacheSet {
        CachedLeafMeta slots[WAYS];
        uint8_t occupancy;  // Bitmap: bit i set → slot i occupied

        CacheSet() : occupancy(0) {}

        // CHIME-inspired: find first zero bit using __builtin_ctz
        int find_free_slot() const {
            uint8_t free_bits = static_cast<uint8_t>(~occupancy);
            if (free_bits == 0) return -1;
            return __builtin_ctz(static_cast<unsigned>(free_bits));
        }

        void set_occupied(int way)   { occupancy |=  (1u << way); }
        void clear_occupied(int way) { occupancy &= ~(1u << way); }
        bool is_occupied(int way) const { return occupancy & (1u << way); }

        void reset() {
            for (int i = 0; i < WAYS; ++i)
                slots[i].reset();
            occupancy = 0;
        }
    };

    BitmapLeafDirectory(uint64_t cache_size_bytes, uint64_t pg_size)
        : page_size_(pg_size) {
        uint64_t total_pages = cache_size_bytes / pg_size;
        num_sets_ = (total_pages + WAYS - 1) / WAYS;
        if (num_sets_ < 64) num_sets_ = 64;

        sets_ = new CacheSet[num_sets_];

        // Pre-allocate page pool via huge pages (DEX's HugePageAlloc)
        pool_capacity_ = num_sets_ * WAYS;
        page_pool_ = static_cast<char*>(hugePageAlloc(pool_capacity_ * pg_size));
        if (page_pool_)
            memset(page_pool_, 0, pool_capacity_ * pg_size);
    }

    ~BitmapLeafDirectory() {
        delete[] sets_;
        // page_pool_ was mmap'd by hugePageAlloc; munmap it
        if (page_pool_)
            munmap(page_pool_, pool_capacity_ * page_size_);
    }

    // ---------------------------------------------------------------
    // Lookup: check if a leaf is cached locally.
    // Returns pointer to the local page copy, or nullptr.
    // On hit, also returns vacancy_bitmap and fence keys.
    // ---------------------------------------------------------------
    void* lookup(GlobalAddress leaf_addr,
                 uint64_t* out_vacancy = nullptr,
                 Key* out_fence_low = nullptr,
                 Key* out_fence_high = nullptr) {
        uint64_t idx = hash_to_set(leaf_addr);
        CacheSet& set = sets_[idx];

        for (int w = 0; w < WAYS; ++w) {
            if (set.is_occupied(w) &&
                set.slots[w].leaf_addr == leaf_addr) {
                set.slots[w].freq.fetch_add(1, std::memory_order_relaxed);
                hits_.fetch_add(1, std::memory_order_relaxed);
                if (out_vacancy)   *out_vacancy   = set.slots[w].vacancy_bitmap;
                if (out_fence_low) *out_fence_low  = set.slots[w].fence_low;
                if (out_fence_high)*out_fence_high = set.slots[w].fence_high;
                return set.slots[w].local_page;
            }
        }
        misses_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }

    // ---------------------------------------------------------------
    // Insert: cache a leaf page.  Copies page_data into local pool.
    // vacancy_bitmap is the leaf's slot-occupancy state (pulled from
    // memory node so we never need an RDMA read for it again).
    // ---------------------------------------------------------------
    bool insert(GlobalAddress leaf_addr, const void* page_data,
                uint64_t vacancy_bitmap, Key fence_low, Key fence_high) {
        uint64_t idx = hash_to_set(leaf_addr);
        CacheSet& set = sets_[idx];

        // Check if already cached → update in place
        for (int w = 0; w < WAYS; ++w) {
            if (set.is_occupied(w) &&
                set.slots[w].leaf_addr == leaf_addr) {
                memcpy(set.slots[w].local_page, page_data, page_size_);
                set.slots[w].vacancy_bitmap = vacancy_bitmap;
                set.slots[w].fence_low  = fence_low;
                set.slots[w].fence_high = fence_high;
                set.slots[w].freq.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }

        // Find free slot using bitmap  (CHIME's ctzll pattern)
        int free_way = set.find_free_slot();
        if (free_way < 0) {
            // All WAYS occupied — evict lowest-frequency entry
            free_way = evict_from_set(set);
        }

        void* local_page = allocate_page();
        if (!local_page) return false;

        memcpy(local_page, page_data, page_size_);

        auto& slot = set.slots[free_way];
        slot.leaf_addr      = leaf_addr;
        slot.local_page     = local_page;
        slot.vacancy_bitmap = vacancy_bitmap;
        slot.fence_low      = fence_low;
        slot.fence_high     = fence_high;
        slot.freq.store(1, std::memory_order_relaxed);
        slot.version        = 0;
        set.set_occupied(free_way);

        cached_count_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    // Simplified insert (derive fence keys from the leaf page itself)
    bool insert(GlobalAddress leaf_addr, const void* page_data) {
        return insert(leaf_addr, page_data, 0, 0, 0);
    }

    // ---------------------------------------------------------------
    // Invalidate: remove a cached leaf.
    // ---------------------------------------------------------------
    void invalidate(GlobalAddress leaf_addr) {
        uint64_t idx = hash_to_set(leaf_addr);
        CacheSet& set = sets_[idx];

        for (int w = 0; w < WAYS; ++w) {
            if (set.is_occupied(w) &&
                set.slots[w].leaf_addr == leaf_addr) {
                set.slots[w].leaf_addr = GlobalAddress::Null();
                set.slots[w].local_page = nullptr;
                set.slots[w].freq.store(0, std::memory_order_relaxed);
                set.clear_occupied(w);
                cached_count_.fetch_sub(1, std::memory_order_relaxed);
                break;
            }
        }
    }

    // ---------------------------------------------------------------
    // Statistics
    // ---------------------------------------------------------------
    uint64_t cached_count() const { return cached_count_.load(); }
    uint64_t hit_count()    const { return hits_.load(); }
    uint64_t miss_count()   const { return misses_.load(); }

    void clear_statistics() {
        hits_.store(0);
        misses_.store(0);
    }

    void reset() {
        for (uint64_t i = 0; i < num_sets_; ++i)
            sets_[i].reset();
        pool_head_.store(0);
        cached_count_.store(0);
        clear_statistics();
    }

private:
    uint64_t  num_sets_;
    uint64_t  page_size_;
    CacheSet* sets_;

    // Page buffer pool (pre-allocated huge pages via DEX's hugePageAlloc)
    char*                  page_pool_;
    std::atomic<uint64_t>  pool_head_{0};
    uint64_t               pool_capacity_;

    // Statistics
    std::atomic<uint64_t> cached_count_{0};
    std::atomic<uint64_t> hits_{0};
    std::atomic<uint64_t> misses_{0};

    // Multiplicative hash for good distribution across sets
    uint64_t hash_to_set(GlobalAddress addr) const {
        uint64_t h = addr.val * 0x9E3779B97F4A7C15ULL;  // golden-ratio hash
        return (h >> 32) % num_sets_;
    }

    // Bump-allocate from the pre-allocated pool
    void* allocate_page() {
        uint64_t idx = pool_head_.fetch_add(1, std::memory_order_relaxed);
        if (idx >= pool_capacity_)
            idx = idx % pool_capacity_;  // wrap around
        return page_pool_ + idx * page_size_;
    }

    // Two-random-choice LFU eviction within a set (CHIME-inspired)
    int evict_from_set(CacheSet& set) {
        thread_local std::mt19937 rng(std::random_device{}());

        int way1 = rng() % WAYS;
        int way2 = (way1 + 1 + rng() % (WAYS - 1)) % WAYS;

        int victim = (set.slots[way1].freq.load(std::memory_order_relaxed) <=
                      set.slots[way2].freq.load(std::memory_order_relaxed))
                         ? way1 : way2;

        set.slots[victim].leaf_addr = GlobalAddress::Null();
        set.slots[victim].local_page = nullptr;
        set.slots[victim].freq.store(0, std::memory_order_relaxed);
        set.clear_occupied(victim);
        cached_count_.fetch_sub(1, std::memory_order_relaxed);

        return victim;
    }
};

} // namespace tbc
