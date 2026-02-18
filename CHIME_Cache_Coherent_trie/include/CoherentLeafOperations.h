#if !defined(_COHERENT_LEAF_OPERATIONS_H_)
#define _COHERENT_LEAF_OPERATIONS_H_

#include "Common.h"
#include "LeafNode.h"
#include "GlobalAddress.h"
#include "DSM.h"
#include "ComputeSideBitmap.h"
#include "CacheCoherence.h"
#include "Hash.h"

#include <cassert>

/**
 * CoherentLeafOperations - Integrated leaf operations with compute-side caching.
 * 
 * This module provides the core leaf operations (search, insert, delete, update)
 * that utilize:
 * - Compute-side vacancy bitmap cache (ComputeSideBitmap)
 * - Trie-based internal node cache (TrieCache)
 * - Cache coherence protocol (CacheCoherence)
 * 
 * The key insight is that by caching vacancy bitmaps at compute nodes, we can:
 * 1. Find free slots for inserts without reading the full leaf
 * 2. Skip hopscotch neighborhoods that are definitely empty
 * 3. Reduce RDMA round-trips for common operations
 */

namespace coherent_leaf {

// ============================================================================
// SLOT SEARCH RESULT
// ============================================================================

struct SlotSearchResult {
  int slot_idx;           // Found slot index, -1 if not found
  bool from_cache;        // Whether result came from cache
  bool needs_validation;  // Whether slot needs to be validated via RDMA
  uint64_t cached_version; // Cache version for validation
  
  SlotSearchResult() : slot_idx(-1), from_cache(false), 
                       needs_validation(false), cached_version(0) {}
  
  static SlotSearchResult not_found() { return SlotSearchResult(); }
  static SlotSearchResult found(int idx, bool cached, uint64_t ver = 0) {
    SlotSearchResult r;
    r.slot_idx = idx;
    r.from_cache = cached;
    r.cached_version = ver;
    return r;
  }
};


// ============================================================================
// COHERENT LEAF MANAGER
// ============================================================================

/**
 * CoherentLeafManager - Manager for coherent leaf operations.
 * 
 * Integrates DSM, bitmap cache, and coherence controller.
 */
class CoherentLeafManager {
public:
  CoherentLeafManager(DSM* dsm)
    : dsm_(dsm),
      bitmap_mgr_(dsm),
      coherence_(dsm, dsm->getMyNodeID()) {
    
    // Wire up invalidation callback
    coherence_.set_invalidate_callback([this](const GlobalAddress& addr) {
      bitmap_mgr_.invalidate(addr);
    });
  }
  
  // ========== SLOT SEARCH OPERATIONS ==========
  
  /**
   * Find a free slot for insertion using cached bitmap.
   * 
   * @param leaf_addr Address of the leaf node
   * @param key Key being inserted (for hash calculation)
   * @param sink Coroutine context
   * @return SlotSearchResult with free slot info
   */
  SlotSearchResult find_free_slot(
    const GlobalAddress& leaf_addr,
    const Key& key,
    CoroPull* sink = nullptr
  ) {
    int hash_idx = get_hashed_leaf_entry_index(key);
    
    // Try cache first
    auto* cached = bitmap_mgr_.get_bitmap(leaf_addr, sink);
    if (cached && cached->is_valid()) {
      int free_slot = cached->find_free_slot(hash_idx);
      if (free_slot >= 0) {
        return SlotSearchResult::found(free_slot, true, cached->version.load());
      }
      // Cache says no free slot - might need split
      SlotSearchResult r;
      r.slot_idx = -1;
      r.from_cache = true;
      r.needs_validation = true;  // Should verify before splitting
      r.cached_version = cached->version.load();
      return r;
    }
    
    // Cache miss - need to read from memory
    return SlotSearchResult::not_found();
  }
  
  /**
   * Find slot containing a key using cached bitmap (for search).
   * 
   * @param leaf_addr Address of the leaf node
   * @param key Key to search for
   * @param neighborhood_size Hopscotch neighborhood size
   * @param sink Coroutine context
   * @return Slot index if definitely found, -1 if not in neighborhood, -2 if need full read
   */
  int find_key_slot_hint(
    const GlobalAddress& leaf_addr,
    const Key& key,
    int neighborhood_size,
    CoroPull* sink = nullptr
  ) {
    int hash_idx = get_hashed_leaf_entry_index(key);
    return bitmap_mgr_.get_cache().check_key_slot_cached(
      leaf_addr, hash_idx, neighborhood_size
    );
  }
  
  /**
   * Find slots in hopscotch neighborhood using cached bitmap.
   * 
   * Returns bitmap of occupied slots in neighborhood starting at hash_idx.
   */
  uint64_t get_neighborhood_occupancy(
    const GlobalAddress& leaf_addr,
    int hash_idx,
    int neighborhood_size,
    CoroPull* sink = nullptr
  ) {
    auto* cached = bitmap_mgr_.get_bitmap(leaf_addr, sink);
    if (!cached || !cached->is_valid()) {
      return 0xFFFFFFFFFFFFFFFFULL;  // Unknown - assume all occupied
    }
    
    uint64_t result = 0;
    for (int i = 0; i < neighborhood_size; i++) {
      int idx = (hash_idx + i) % define::leafSpanSize;
      if (cached->vacancy_bitmap & (1ULL << idx)) {
        result |= (1ULL << i);
      }
    }
    return result;
  }
  
  // ========== CACHE UPDATE OPERATIONS ==========
  
  /**
   * Update cached bitmap after successful insert.
   */
  void update_after_insert(
    const GlobalAddress& leaf_addr,
    int slot_idx
  ) {
    bitmap_mgr_.mark_slot_occupied(leaf_addr, slot_idx);
    coherence_.mark_modified(leaf_addr);
  }
  
  /**
   * Update cached bitmap after successful delete.
   */
  void update_after_delete(
    const GlobalAddress& leaf_addr,
    int slot_idx
  ) {
    bitmap_mgr_.mark_slot_free(leaf_addr, slot_idx);
    coherence_.mark_modified(leaf_addr);
  }
  
  /**
   * Cache a new leaf's bitmap after reading it.
   */
  void cache_leaf_bitmap(
    const GlobalAddress& leaf_addr,
    const LeafNode* leaf,
    uint64_t vacancy_bitmap,
    uint64_t version
  ) {
    bitmap_mgr_.get_cache().cache_bitmap(
      leaf_addr,
      vacancy_bitmap,
      version,
      leaf->metadata.fence_keys.lowest,
      leaf->metadata.fence_keys.highest
    );
  }
  
  /**
   * Invalidate cached bitmap (e.g., after split).
   */
  void invalidate(const GlobalAddress& leaf_addr) {
    bitmap_mgr_.invalidate(leaf_addr);
    coherence_.evict(leaf_addr);
  }
  
  // ========== COHERENCE OPERATIONS ==========
  
  /**
   * Request read access for leaf (coherence).
   */
  bool request_read(
    const GlobalAddress& leaf_addr,
    CoroPull* sink = nullptr
  ) {
    uint64_t version;
    auto state = coherence_.request_read(leaf_addr, version, sink);
    return state != coherence::CoherenceState::INVALID;
  }
  
  /**
   * Request write access for leaf (coherence).
   */
  bool request_write(
    const GlobalAddress& leaf_addr,
    CoroPull* sink = nullptr
  ) {
    uint64_t version;
    auto state = coherence_.request_write(leaf_addr, version, sink);
    return state == coherence::CoherenceState::EXCLUSIVE || 
           state == coherence::CoherenceState::MODIFIED;
  }
  
  /**
   * Validate cached data against memory version.
   */
  bool validate_cache(
    const GlobalAddress& leaf_addr,
    uint64_t cached_version,
    CoroPull* sink = nullptr
  ) {
    return coherence_.validate_version(leaf_addr, cached_version, sink);
  }
  
  // ========== STATISTICS ==========
  
  void print_stats() {
    bitmap_mgr_.print_stats();
  }
  
  // ========== ACCESSORS ==========
  
  ComputeSideBitmapManager& bitmap_manager() { return bitmap_mgr_; }
  coherence::CoherenceController& coherence_controller() { return coherence_; }
  DSM* dsm() { return dsm_; }

private:
  DSM* dsm_;
  ComputeSideBitmapManager bitmap_mgr_;
  coherence::CoherenceController coherence_;
};


// ============================================================================
// HOPSCOTCH OPERATIONS WITH CACHED BITMAP
// ============================================================================

/**
 * Cached hopscotch insert helper.
 * 
 * Uses cached bitmap to:
 * 1. Quickly find free slot in neighborhood
 * 2. Determine if hopping is needed
 * 3. Minimize RDMA reads
 */
class CachedHopscotchHelper {
public:
  CachedHopscotchHelper(CoherentLeafManager& mgr) : mgr_(mgr) {}
  
  /**
   * Find best slot for insert using cached bitmap.
   * 
   * Returns:
   * - slot_idx >= 0: Found free slot at this index
   * - slot_idx == -1: Need hopping (all neighborhood slots occupied)
   * - slot_idx == -2: Cache miss, need full leaf read
   */
  int find_insert_slot(
    const GlobalAddress& leaf_addr,
    const Key& key,
    CoroPull* sink = nullptr
  ) {
    int hash_idx = get_hashed_leaf_entry_index(key);
    
    // Get cached bitmap
    auto& cache = mgr_.bitmap_manager().get_cache();
    auto* cached = cache.lookup(leaf_addr);
    
    if (!cached || !cached->is_valid()) {
      return -2;  // Cache miss
    }
    
    // Look for free slot in neighborhood
    int free_in_neighborhood = cached->find_free_in_neighborhood(
      hash_idx, define::neighborSize
    );
    
    if (free_in_neighborhood >= 0) {
      return free_in_neighborhood;
    }
    
    // No free slot in neighborhood - look for free slot anywhere
    // (will need hopping)
    int free_anywhere = cached->find_free_slot(hash_idx);
    if (free_anywhere >= 0) {
      return -1;  // Signal that hopping is needed
    }
    
    // No free slots at all - leaf is full, need split
    return -1;
  }
  
  /**
   * Calculate entries to read for hopscotch search.
   * 
   * Uses cached bitmap to determine minimal read range.
   */
  std::pair<int, int> calculate_read_range(
    const GlobalAddress& leaf_addr,
    const Key& key,
    bool for_write,
    CoroPull* sink = nullptr
  ) {
    int hash_idx = get_hashed_leaf_entry_index(key);
    
    auto& cache = mgr_.bitmap_manager().get_cache();
    auto* cached = cache.lookup(leaf_addr);
    
    if (!cached || !cached->is_valid()) {
      // Cache miss - read full neighborhood
      return {hash_idx, define::neighborSize};
    }
    
    // Get occupancy in neighborhood
    uint64_t occupancy = mgr_.get_neighborhood_occupancy(
      leaf_addr, hash_idx, define::neighborSize, sink
    );
    
    if (occupancy == 0) {
      // Empty neighborhood - for search, return not found
      // For write, read minimal (just metadata)
      return {hash_idx, for_write ? 1 : 0};
    }
    
    // Find actual range of occupied slots
    int first_occupied = __builtin_ctzll(occupancy);
    int last_occupied = 63 - __builtin_clzll(occupancy);
    
    return {(hash_idx + first_occupied) % define::leafSpanSize, 
            last_occupied - first_occupied + 1};
  }
  
  /**
   * Predict if key exists based on cached bitmap.
   * 
   * Returns:
   * - true: Key might exist (slot in neighborhood is occupied)
   * - false: Key definitely doesn't exist (all neighborhood slots empty)
   */
  bool may_exist(
    const GlobalAddress& leaf_addr,
    const Key& key,
    CoroPull* sink = nullptr
  ) {
    int hint = mgr_.find_key_slot_hint(
      leaf_addr, key, define::neighborSize, sink
    );
    return hint != -1;  // -1 means definitely not exist
  }
  
private:
  CoherentLeafManager& mgr_;
};


// ============================================================================
// LEAF SPLIT HELPER
// ============================================================================

/**
 * Helper for leaf split operations with cache management.
 */
class LeafSplitHelper {
public:
  LeafSplitHelper(CoherentLeafManager& mgr) : mgr_(mgr) {}
  
  /**
   * Called before split to prepare caches.
   */
  void prepare_split(const GlobalAddress& old_leaf_addr) {
    // Invalidate old leaf's cached bitmap (will change completely)
    mgr_.invalidate(old_leaf_addr);
  }
  
  /**
   * Called after split to update caches.
   */
  void complete_split(
    const GlobalAddress& old_leaf_addr,
    const GlobalAddress& new_leaf_addr,
    const LeafNode* old_leaf,
    const LeafNode* new_leaf,
    uint64_t old_bitmap,
    uint64_t new_bitmap
  ) {
    // Cache both leaves' bitmaps
    mgr_.cache_leaf_bitmap(old_leaf_addr, old_leaf, old_bitmap, 1);
    mgr_.cache_leaf_bitmap(new_leaf_addr, new_leaf, new_bitmap, 1);
  }
  
private:
  CoherentLeafManager& mgr_;
};


// ============================================================================
// EVICTION POLICY
// ============================================================================

/**
 * EvictionPolicy - Policy for bitmap cache eviction.
 */
enum class EvictionPolicy {
  LFU,           // Least Frequently Used (default)
  LRU,           // Least Recently Used
  RANDOM,        // Random eviction
  TWO_RANDOM     // Two-random-choice (CHIME style)
};

/**
 * EvictionManager - Manages cache eviction decisions.
 */
class EvictionManager {
public:
  EvictionManager(EvictionPolicy policy = EvictionPolicy::TWO_RANDOM)
    : policy_(policy) {}
  
  /**
   * Select entry to evict from a cache set.
   * Returns index of entry to evict.
   */
  int select_victim(
    const CachedBitmapEntry* entries,
    int num_entries
  ) {
    switch (policy_) {
      case EvictionPolicy::LFU:
        return select_lfu(entries, num_entries);
      case EvictionPolicy::LRU:
        return select_lru(entries, num_entries);
      case EvictionPolicy::RANDOM:
        return select_random(entries, num_entries);
      case EvictionPolicy::TWO_RANDOM:
      default:
        return select_two_random(entries, num_entries);
    }
  }
  
private:
  int select_lfu(const CachedBitmapEntry* entries, int num_entries) {
    int min_idx = 0;
    uint64_t min_freq = entries[0].access_count.load();
    for (int i = 1; i < num_entries; i++) {
      if (entries[i].is_valid()) {
        uint64_t freq = entries[i].access_count.load();
        if (freq < min_freq) {
          min_freq = freq;
          min_idx = i;
        }
      }
    }
    return min_idx;
  }
  
  int select_lru(const CachedBitmapEntry* entries, int num_entries) {
    // Would need access timestamp - not currently tracked
    // Fall back to LFU
    return select_lfu(entries, num_entries);
  }
  
  int select_random(const CachedBitmapEntry* entries, int num_entries) {
    // Simple random selection
    return rand() % num_entries;
  }
  
  int select_two_random(const CachedBitmapEntry* entries, int num_entries) {
    int idx1 = rand() % num_entries;
    int idx2 = rand() % num_entries;
    while (idx2 == idx1 && num_entries > 1) {
      idx2 = rand() % num_entries;
    }
    
    uint64_t freq1 = entries[idx1].access_count.load();
    uint64_t freq2 = entries[idx2].access_count.load();
    
    return (freq1 < freq2) ? idx1 : idx2;
  }
  
  EvictionPolicy policy_;
};


// ============================================================================
// PAGE MANAGEMENT
// ============================================================================

/**
 * PageManager - Manages leaf page allocation and deallocation.
 */
class PageManager {
public:
  PageManager(DSM* dsm) : dsm_(dsm) {}
  
  /**
   * Allocate a new leaf page.
   */
  GlobalAddress allocate_leaf() {
    return dsm_->alloc(define::allocationLeafSize, PACKED_ADDR_ALIGN_BIT);
  }
  
  /**
   * Free a leaf page.
   * Note: In CHIME, pages are typically not freed due to no garbage collection.
   */
  void free_leaf(const GlobalAddress& addr) {
    // No-op in current CHIME design
    // Would need epoch-based reclamation for safe deallocation
  }
  
private:
  DSM* dsm_;
};


// ============================================================================
// LOCK MANAGEMENT
// ============================================================================

/**
 * LockManager - Manages leaf locks with cache awareness.
 */
class LockManager {
public:
  LockManager(DSM* dsm) : dsm_(dsm) {}
  
  /**
   * Lock a leaf node.
   * Returns lock word value containing vacancy info.
   */
  uint64_t lock_leaf(
    const GlobalAddress& leaf_addr,
    CoroPull* sink = nullptr
  ) {
    uint64_t lock_offset = ROUND_UP(define::transLeafSize, 3);
    auto lock_buffer = (dsm_->get_rbuf(sink)).get_lock_buffer();
    
    uint64_t retry_cnt = 0;
    while (true) {
      // CAS to acquire lock (set high bit)
      bool success = dsm_->cas_mask_sync(
        leaf_addr + lock_offset,
        0UL, ~0UL,
        lock_buffer,
        1ULL << 63, ~0ULL,
        sink
      );
      
      if (success) {
        return *(uint64_t*)lock_buffer;
      }
      
      retry_cnt++;
      if (retry_cnt > 10000000) {
        assert(false && "Deadlock detected");
      }
    }
  }
  
  /**
   * Unlock a leaf node.
   */
  void unlock_leaf(
    const GlobalAddress& leaf_addr,
    uint64_t lock_value,
    bool async = false,
    CoroPull* sink = nullptr
  ) {
    uint64_t lock_offset = ROUND_UP(define::transLeafSize, 3);
    uint64_t* lock_buffer;
    
    if (async) {
      lock_buffer = (dsm_->get_rbuf(sink)).get_lock_buffer();
      *lock_buffer = lock_value;
      dsm_->write((char*)lock_buffer, leaf_addr + lock_offset, sizeof(uint64_t), false, sink);
    } else {
      lock_buffer = (dsm_->get_rbuf(sink)).get_lock_buffer();
      *lock_buffer = lock_value;
      dsm_->write_sync((char*)lock_buffer, leaf_addr + lock_offset, sizeof(uint64_t), sink);
    }
  }
  
  /**
   * Extract vacancy bitmap from lock word.
   */
  static uint64_t get_vacancy_from_lock(uint64_t lock_word) {
    return lock_word & ~(1ULL << 63);  // Clear lock bit
  }
  
  /**
   * Create lock word with vacancy bitmap.
   */
  static uint64_t make_lock_word(uint64_t vacancy_bitmap, bool locked) {
    uint64_t result = vacancy_bitmap & ~(1ULL << 63);
    if (locked) {
      result |= (1ULL << 63);
    }
    return result;
  }
  
private:
  DSM* dsm_;
};

} // namespace coherent_leaf

#endif // _COHERENT_LEAF_OPERATIONS_H_
