#if !defined(_COMPUTE_SIDE_BITMAP_H_)
#define _COMPUTE_SIDE_BITMAP_H_

#include "Common.h"
#include "GlobalAddress.h"
#include "LeafNode.h"

#include <tbb/concurrent_hash_map.h>
#include <tbb/concurrent_queue.h>
#include <atomic>
#include <cstdint>

/**
 * ComputeSideBitmap - Compute-node caching of leaf vacancy bitmaps.
 * 
 * This moves the vacancy bitmap from memory-side to compute-side, enabling:
 * - Fast slot lookup without RDMA round-trip
 * - Reduced memory-node CPU involvement
 * - Cache coherence through version-based invalidation
 * 
 * The bitmap cache uses 8-way set-associative organization with LFU eviction.
 * Each entry is 64-byte aligned for cache-line efficiency.
 */

// ============================================================================
// CACHED BITMAP ENTRY
// ============================================================================

/**
 * CachedBitmapEntry - Single cached leaf bitmap with metadata.
 * 
 * Layout (64 bytes, cacheline-aligned):
 * - leaf_addr: 8 bytes (GlobalAddress of leaf node)
 * - vacancy_bitmap: 8 bytes (64-bit vacancy bitmap, 1=occupied, 0=free)
 * - version: 8 bytes (coherence version for invalidation)
 * - access_count: 8 bytes (LFU frequency counter)
 * - fence_low/high: 16 bytes (key range for validation)
 * - hop_bitmap: 8 bytes (hopscotch neighborhood bitmap)
 * - max_key_idx: 2 bytes (index of max key for split detection)
 * - flags: 2 bytes (state flags)
 * - padding: 4 bytes
 */
struct alignas(64) CachedBitmapEntry {
  GlobalAddress leaf_addr;        // Address of cached leaf
  uint64_t vacancy_bitmap;        // Bitmap: bit i=1 means slot i is occupied
  std::atomic<uint64_t> version;  // Version for coherence
  std::atomic<uint64_t> access_count;  // LFU counter
  Key fence_low;                  // Lower fence key
  Key fence_high;                 // Upper fence key (exclusive)
  uint64_t hop_bitmap;            // Hopscotch neighborhood summary
  uint16_t max_key_idx;           // Index of maximum key in leaf
  uint16_t flags;                 // State flags
  uint32_t reserved;              // Padding to 64 bytes
  
  // Flag values
  static constexpr uint16_t FLAG_VALID = 0x0001;
  static constexpr uint16_t FLAG_DIRTY = 0x0002;  // Local modifications not synced
  static constexpr uint16_t FLAG_SHARED = 0x0004; // Other compute nodes may have copy
  static constexpr uint16_t FLAG_EXCLUSIVE = 0x0008;  // We have exclusive access
  
  CachedBitmapEntry() 
    : leaf_addr(), vacancy_bitmap(0), version(0), access_count(0),
      fence_low(), fence_high(), hop_bitmap(0), max_key_idx(0), 
      flags(0), reserved(0) {}
  
  CachedBitmapEntry(const GlobalAddress& addr, uint64_t bitmap, uint64_t ver)
    : leaf_addr(addr), vacancy_bitmap(bitmap), version(ver), access_count(1),
      fence_low(), fence_high(), hop_bitmap(0), max_key_idx(0),
      flags(FLAG_VALID), reserved(0) {}
  
  // Check if entry is valid
  bool is_valid() const { return flags & FLAG_VALID; }
  bool is_dirty() const { return flags & FLAG_DIRTY; }
  bool is_shared() const { return flags & FLAG_SHARED; }
  bool is_exclusive() const { return flags & FLAG_EXCLUSIVE; }
  
  // Touch for LFU
  void touch() {
    access_count.fetch_add(1, std::memory_order_relaxed);
  }
  
  // Get number of free slots
  int count_free_slots() const {
    return define::leafSpanSize - __builtin_popcountll(vacancy_bitmap);
  }
  
  // Find first free slot starting from hash_idx
  int find_free_slot(int hash_idx) const {
    uint64_t mask = ~vacancy_bitmap;
    if (mask == 0) return -1;  // No free slots
    
    // Rotate bitmap so hash_idx is at bit 0
    uint64_t rotated = (mask >> hash_idx) | (mask << (define::leafSpanSize - hash_idx));
    rotated &= ((1ULL << define::leafSpanSize) - 1);
    
    if (rotated == 0) return -1;
    
    int offset = __builtin_ctzll(rotated);
    return (hash_idx + offset) % define::leafSpanSize;
  }
  
  // Find free slot within hopscotch neighborhood
  int find_free_in_neighborhood(int hash_idx, int neighborhood_size) const {
    uint64_t mask = ~vacancy_bitmap;
    uint64_t neighborhood_mask = 0;
    
    for (int i = 0; i < neighborhood_size; i++) {
      int idx = (hash_idx + i) % define::leafSpanSize;
      neighborhood_mask |= (1ULL << idx);
    }
    
    mask &= neighborhood_mask;
    if (mask == 0) return -1;
    
    return __builtin_ctzll(mask);
  }
  
  // Mark slot as occupied
  void set_occupied(int slot_idx) {
    vacancy_bitmap |= (1ULL << slot_idx);
    flags |= FLAG_DIRTY;
  }
  
  // Mark slot as free
  void set_free(int slot_idx) {
    vacancy_bitmap &= ~(1ULL << slot_idx);
    flags |= FLAG_DIRTY;
  }
};

static_assert(sizeof(CachedBitmapEntry) == 64, "CachedBitmapEntry must be cacheline-aligned");


// ============================================================================
// BITMAP CACHE SET (8-WAY SET ASSOCIATIVE)
// ============================================================================

/**
 * BitmapCacheSet - One set in the set-associative bitmap cache.
 * 
 * 8-way associative with LFU eviction.
 */
class BitmapCacheSet {
public:
  static constexpr int ASSOCIATIVITY = 8;
  
  CachedBitmapEntry entries[ASSOCIATIVITY];
  tbb::spin_mutex set_lock;
  
  BitmapCacheSet() = default;
  
  // Find entry for leaf_addr, returns nullptr if not found
  CachedBitmapEntry* find(const GlobalAddress& leaf_addr) {
    for (int i = 0; i < ASSOCIATIVITY; i++) {
      if (entries[i].is_valid() && entries[i].leaf_addr == leaf_addr) {
        return &entries[i];
      }
    }
    return nullptr;
  }
  
  // Insert or update entry, returns evicted entry if any
  CachedBitmapEntry* insert(const CachedBitmapEntry& new_entry, CachedBitmapEntry* evicted_out) {
    tbb::spin_mutex::scoped_lock lock(set_lock);
    
    // First check if already exists
    for (int i = 0; i < ASSOCIATIVITY; i++) {
      if (entries[i].is_valid() && entries[i].leaf_addr == new_entry.leaf_addr) {
        // Update existing
        entries[i].vacancy_bitmap = new_entry.vacancy_bitmap;
        entries[i].version.store(new_entry.version.load());
        entries[i].touch();
        return &entries[i];
      }
    }
    
    // Find empty slot
    for (int i = 0; i < ASSOCIATIVITY; i++) {
      if (!entries[i].is_valid()) {
        entries[i] = new_entry;
        return &entries[i];
      }
    }
    
    // No empty slot - evict LFU entry
    int min_idx = 0;
    uint64_t min_freq = entries[0].access_count.load();
    for (int i = 1; i < ASSOCIATIVITY; i++) {
      uint64_t freq = entries[i].access_count.load();
      if (freq < min_freq) {
        min_freq = freq;
        min_idx = i;
      }
    }
    
    if (evicted_out) {
      *evicted_out = entries[min_idx];
    }
    entries[min_idx] = new_entry;
    return &entries[min_idx];
  }
  
  // Invalidate entry for leaf_addr
  bool invalidate(const GlobalAddress& leaf_addr) {
    tbb::spin_mutex::scoped_lock lock(set_lock);
    
    for (int i = 0; i < ASSOCIATIVITY; i++) {
      if (entries[i].is_valid() && entries[i].leaf_addr == leaf_addr) {
        entries[i].flags = 0;  // Clear valid flag
        return true;
      }
    }
    return false;
  }
  
  // Invalidate if version mismatch
  bool invalidate_if_stale(const GlobalAddress& leaf_addr, uint64_t expected_version) {
    tbb::spin_mutex::scoped_lock lock(set_lock);
    
    for (int i = 0; i < ASSOCIATIVITY; i++) {
      if (entries[i].is_valid() && entries[i].leaf_addr == leaf_addr) {
        if (entries[i].version.load() != expected_version) {
          entries[i].flags = 0;
          return true;
        }
        return false;  // Version matches, not stale
      }
    }
    return false;  // Not found
  }
};


// ============================================================================
// COMPUTE-SIDE BITMAP CACHE
// ============================================================================

/**
 * ComputeSideBitmapCache - Main bitmap cache for compute node.
 * 
 * Set-associative cache indexed by leaf address hash.
 * Integrates with cache coherence protocol for multi-node consistency.
 */
class ComputeSideBitmapCache {
public:
  // Number of sets (power of 2 for fast indexing)
  static constexpr size_t NUM_SETS = 16384;  // 16K sets × 8 ways × 64B = 8MB
  static constexpr size_t SET_MASK = NUM_SETS - 1;
  
  ComputeSideBitmapCache() {
    sets = new BitmapCacheSet[NUM_SETS];
    stats_hits.store(0);
    stats_misses.store(0);
  }
  
  ~ComputeSideBitmapCache() {
    delete[] sets;
  }
  
  // ========== MAIN API ==========
  
  /**
   * Lookup cached bitmap for leaf.
   * Returns nullptr on miss.
   */
  CachedBitmapEntry* lookup(const GlobalAddress& leaf_addr) {
    size_t set_idx = hash_leaf_addr(leaf_addr);
    auto* entry = sets[set_idx].find(leaf_addr);
    if (entry) {
      stats_hits.fetch_add(1);
      entry->touch();
    } else {
      stats_misses.fetch_add(1);
    }
    return entry;
  }
  
  /**
   * Cache a new bitmap entry.
   * Returns pointer to cached entry (may evict existing entry).
   */
  CachedBitmapEntry* cache_bitmap(
    const GlobalAddress& leaf_addr,
    uint64_t vacancy_bitmap,
    uint64_t version,
    const Key& fence_low,
    const Key& fence_high
  ) {
    CachedBitmapEntry new_entry(leaf_addr, vacancy_bitmap, version);
    new_entry.fence_low = fence_low;
    new_entry.fence_high = fence_high;
    
    size_t set_idx = hash_leaf_addr(leaf_addr);
    CachedBitmapEntry evicted;
    auto* cached = sets[set_idx].insert(new_entry, &evicted);
    
    // If evicted entry was dirty, would need to write back
    // (handled by coherence layer)
    
    return cached;
  }
  
  /**
   * Update cached bitmap after local modification.
   */
  bool update_bitmap(
    const GlobalAddress& leaf_addr,
    int slot_idx,
    bool occupied
  ) {
    size_t set_idx = hash_leaf_addr(leaf_addr);
    auto* entry = sets[set_idx].find(leaf_addr);
    
    if (entry) {
      if (occupied) {
        entry->set_occupied(slot_idx);
      } else {
        entry->set_free(slot_idx);
      }
      entry->version.fetch_add(1);
      return true;
    }
    return false;
  }
  
  /**
   * Invalidate cached bitmap.
   */
  bool invalidate(const GlobalAddress& leaf_addr) {
    size_t set_idx = hash_leaf_addr(leaf_addr);
    return sets[set_idx].invalidate(leaf_addr);
  }
  
  /**
   * Invalidate if version doesn't match.
   */
  bool invalidate_if_stale(const GlobalAddress& leaf_addr, uint64_t expected_version) {
    size_t set_idx = hash_leaf_addr(leaf_addr);
    return sets[set_idx].invalidate_if_stale(leaf_addr, expected_version);
  }
  
  /**
   * Find first free slot in cached leaf.
   * Returns -1 if not cached or no free slot.
   */
  int find_free_slot_cached(const GlobalAddress& leaf_addr, int hash_idx) {
    auto* entry = lookup(leaf_addr);
    if (!entry) return -2;  // Not cached, need to fetch
    
    return entry->find_free_slot(hash_idx);
  }
  
  /**
   * Check if key might exist in cached leaf.
   * Returns: -2 = not cached, -1 = definitely not exist, >=0 = might exist at idx
   */
  int check_key_slot_cached(
    const GlobalAddress& leaf_addr,
    int hash_idx,
    int neighborhood_size = define::neighborSize
  ) {
    auto* entry = lookup(leaf_addr);
    if (!entry) return -2;
    
    // Check if any slot in neighborhood is occupied
    for (int i = 0; i < neighborhood_size; i++) {
      int idx = (hash_idx + i) % define::leafSpanSize;
      if (entry->vacancy_bitmap & (1ULL << idx)) {
        return idx;  // Might be here
      }
    }
    return -1;  // Not in neighborhood
  }
  
  // ========== STATISTICS ==========
  
  void print_stats() {
    uint64_t hits = stats_hits.load();
    uint64_t misses = stats_misses.load();
    double hit_rate = (hits + misses > 0) 
      ? (double)hits / (hits + misses) * 100.0 
      : 0.0;
    
    printf("[ComputeSideBitmapCache] hits=%lu misses=%lu hit_rate=%.2f%%\n",
           hits, misses, hit_rate);
  }
  
  void reset_stats() {
    stats_hits.store(0);
    stats_misses.store(0);
  }

private:
  // Hash leaf address to set index
  size_t hash_leaf_addr(const GlobalAddress& addr) const {
    // Simple hash using address bits
    uint64_t h = ((uint64_t)addr.nodeID << 48) | addr.offset;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h & SET_MASK;
  }

private:
  BitmapCacheSet* sets;
  std::atomic<uint64_t> stats_hits;
  std::atomic<uint64_t> stats_misses;
};


// ============================================================================
// BITMAP SERIALIZATION FOR RDMA
// ============================================================================

/**
 * BitmapFetchRequest - Request to fetch bitmap from memory node.
 */
struct BitmapFetchRequest {
  GlobalAddress leaf_addr;
  uint64_t requestor_id;
  uint64_t request_version;
} __attribute__((packed));

/**
 * BitmapFetchResponse - Response from memory node with bitmap.
 */
struct BitmapFetchResponse {
  GlobalAddress leaf_addr;
  uint64_t vacancy_bitmap;
  uint64_t version;
  Key fence_low;
  Key fence_high;
  uint16_t max_key_idx;
  uint8_t status;  // 0 = success, 1 = not found, 2 = busy
  uint8_t reserved[5];
} __attribute__((packed));

/**
 * BitmapUpdateRequest - Request to update bitmap on memory node.
 */
struct BitmapUpdateRequest {
  GlobalAddress leaf_addr;
  uint64_t new_vacancy_bitmap;
  uint64_t expected_version;
  uint64_t requestor_id;
} __attribute__((packed));

/**
 * BitmapInvalidation - Invalidation message from memory node.
 */
struct BitmapInvalidation {
  GlobalAddress leaf_addr;
  uint64_t new_version;
  uint64_t modifier_id;  // ID of node that caused invalidation
} __attribute__((packed));


// ============================================================================
// COMPUTE-SIDE BITMAP MANAGER (INTEGRATES WITH DSM)
// ============================================================================

/**
 * ComputeSideBitmapManager - High-level manager for bitmap caching.
 * 
 * Handles:
 * - Bitmap fetch on cache miss
 * - Coherence invalidation processing
 * - Write-back of dirty bitmaps
 */
class ComputeSideBitmapManager {
public:
  ComputeSideBitmapManager(DSM* dsm) : dsm_(dsm), cache_() {}
  
  /**
   * Get bitmap for leaf, fetching from memory if not cached.
   * Returns cached entry (always valid on success).
   */
  CachedBitmapEntry* get_bitmap(
    const GlobalAddress& leaf_addr,
    CoroPull* sink = nullptr
  ) {
    // Try cache first
    auto* entry = cache_.lookup(leaf_addr);
    if (entry && entry->is_valid()) {
      return entry;
    }
    
    // Cache miss - fetch from memory node
    return fetch_bitmap(leaf_addr, sink);
  }
  
  /**
   * Find free slot, fetching bitmap if needed.
   */
  int find_free_slot(
    const GlobalAddress& leaf_addr,
    int hash_idx,
    CoroPull* sink = nullptr
  ) {
    auto* entry = get_bitmap(leaf_addr, sink);
    if (!entry) return -1;  // Fetch failed
    
    return entry->find_free_slot(hash_idx);
  }
  
  /**
   * Update bitmap after insert.
   */
  void mark_slot_occupied(
    const GlobalAddress& leaf_addr,
    int slot_idx
  ) {
    cache_.update_bitmap(leaf_addr, slot_idx, true);
  }
  
  /**
   * Update bitmap after delete.
   */
  void mark_slot_free(
    const GlobalAddress& leaf_addr,
    int slot_idx
  ) {
    cache_.update_bitmap(leaf_addr, slot_idx, false);
  }
  
  /**
   * Process invalidation from memory node.
   */
  void process_invalidation(const BitmapInvalidation& inv) {
    cache_.invalidate_if_stale(inv.leaf_addr, inv.new_version - 1);
  }
  
  /**
   * Invalidate cached bitmap.
   */
  void invalidate(const GlobalAddress& leaf_addr) {
    cache_.invalidate(leaf_addr);
  }
  
  /**
   * Get the underlying cache for direct access.
   */
  ComputeSideBitmapCache& get_cache() { return cache_; }
  
  void print_stats() { cache_.print_stats(); }

private:
  /**
   * Fetch bitmap from memory node via RDMA.
   */
  CachedBitmapEntry* fetch_bitmap(
    const GlobalAddress& leaf_addr,
    CoroPull* sink
  ) {
    // Read leaf metadata to extract vacancy info
    // In CHIME, vacancy is embedded in the lock word (VALOCK)
    
    auto lock_buffer = (dsm_->get_rbuf(sink)).get_lock_buffer();
    
    // Read the lock word which contains vacancy bitmap
    // Lock offset is after the leaf node data
    uint64_t lock_offset = ROUND_UP(define::transLeafSize, 3);
    dsm_->read_sync((char*)lock_buffer, leaf_addr + lock_offset, sizeof(uint64_t), sink);
    
    // Parse VALOCK structure to get vacancy bitmap
    // VALOCK format: [lock_bit(1) | vacancy_bitmap(63)]
    uint64_t lock_word = *(uint64_t*)lock_buffer;
    uint64_t vacancy_bits = lock_word & ~(1ULL << 63);  // Clear lock bit
    
    // Also need to read fence keys from leaf metadata
    auto raw_leaf_buffer = (dsm_->get_rbuf(sink)).get_leaf_buffer();
    dsm_->read_sync(raw_leaf_buffer, leaf_addr, sizeof(LeafMetadata), sink);
    auto* metadata = (LeafMetadata*)raw_leaf_buffer;
    
    // Cache the bitmap
    return cache_.cache_bitmap(
      leaf_addr,
      vacancy_bits,
      1,  // Initial version
      metadata->fence_keys.lowest,
      metadata->fence_keys.highest
    );
  }

private:
  DSM* dsm_;
  ComputeSideBitmapCache cache_;
};

#endif // _COMPUTE_SIDE_BITMAP_H_
