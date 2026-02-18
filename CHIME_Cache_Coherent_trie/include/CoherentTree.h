#if !defined(_COHERENT_TREE_H_)
#define _COHERENT_TREE_H_

/**
 * CoherentTree.h - Modified Tree with trie cache and coherent leaf operations.
 * 
 * This is the main tree interface that integrates:
 * - TrieCache for internal node caching (replaces SkipList-based TreeCache)
 * - ComputeSideBitmap for vacancy bitmap caching
 * - CacheCoherence for multi-node consistency
 * - CoherentLeafOperations for optimized leaf operations
 * 
 * To use this instead of the original Tree.h, define USE_COHERENT_TREE.
 */

#include "TrieCache.h"
#include "IdxCache.h"
#include "DSM.h"
#include "Common.h"
#include "LocalLockTable.h"
#include "MetadataManager.h"
#include "LeafVersionManager.h"
#include "VersionManager.h"
#include "ComputeSideBitmap.h"
#include "CacheCoherence.h"
#include "CoherentLeafOperations.h"

#include <atomic>
#include <city.h>
#include <functional>
#include <map>
#include <algorithm>
#include <queue>
#include <set>
#include <iostream>


/* Workloads */
enum RequestType : int {
  INSERT = 0,
  UPDATE,
  SEARCH,
  SCAN
};

struct Request {
  RequestType req_type;
  Key k;
  Value v;
  int range_size;
};

class RequstGen {
public:
  RequstGen() = default;
  virtual Request next() { return Request{}; }
};


/* Coherent Tree Configuration */
struct CoherentTreeConfig {
  bool use_trie_cache = true;           // Use trie cache instead of skip list
  bool use_bitmap_cache = true;         // Use compute-side bitmap cache
  bool use_coherence = true;            // Enable cache coherence protocol
  bool lazy_coherence = true;           // Use lazy (version-based) coherence
  int cache_size_mb = define::kIndexCacheSize;
  int bitmap_cache_mb = 8;              // 8MB for bitmap cache
};


/* Tree */
using GenFunc = std::function<RequstGen *(DSM*, Request*, int, int, int)>;
#define MAX_FLAG_NUM 4
enum {
  FIRST_TRY,
  INVALID_LEAF,
  INVALID_NODE,
  FIND_NEXT
};


class RootEntry {
public:
  uint16_t level;
  PackedGAddr ptr;

  RootEntry(const uint16_t level, const GlobalAddress& ptr) : level(level), ptr(ptr) {}

  operator uint64_t() const { return ((uint64_t)ptr << 16) | level; }
  operator std::pair<uint16_t, GlobalAddress>() const { return std::make_pair(level, (GlobalAddress)ptr); }
} __attribute__((packed));

static_assert(sizeof(RootEntry) == 8);


/**
 * CoherentTree - B+ Tree with trie cache and coherent leaf operations.
 */
class CoherentTree {
public:
  CoherentTree(DSM *dsm, uint16_t tree_id = 0, bool init_root = true);
  CoherentTree(DSM *dsm, const CoherentTreeConfig& config, uint16_t tree_id = 0, bool init_root = true);

  using WorkFunc = std::function<void (CoherentTree *, const Request&, CoroPull *)>;
  void run_coroutine(GenFunc gen_func, WorkFunc work_func, int coro_cnt, Request* req = nullptr, int req_num = 0);

  // ========== MAIN OPERATIONS ==========
  
  /**
   * Insert key-value pair. Updates if key exists.
   */
  void insert(const Key &k, Value v, CoroPull* sink = nullptr);
  
  /**
   * Update value for existing key. Asserts if key not found.
   */
  void update(const Key &k, Value v, CoroPull* sink = nullptr);
  
  /**
   * Search for key. Returns false if not found.
   */
  bool search(const Key &k, Value &v, CoroPull* sink = nullptr);
  
  /**
   * Range query. Returns all key-values in [from, to].
   */
  bool range_query(const Key &from, const Key &to, std::map<Key, Value> &ret);
  
  /**
   * Delete key. Returns false if not found.
   */
  bool remove(const Key &k, CoroPull* sink = nullptr);

  // ========== STATISTICS ==========
  
  void statistics();
  void clear_debug_info();
  
  // ========== ACCESSORS ==========
  
  TrieCache* get_trie_cache() { return trie_cache_; }
  coherent_leaf::CoherentLeafManager* get_leaf_manager() { return leaf_manager_; }

private:
  // ========== COMMON OPERATIONS ==========
  
  void before_operation(CoroPull* sink);
  GlobalAddress get_root_ptr_ptr();
  RootEntry get_root_ptr(CoroPull* sink);

  // ========== CACHE OPERATIONS ==========
  
  void record_cache_hit_ratio(bool from_cache, int level=1);
  void cache_node(InternalNode* node);
  bool try_cache_hit(const Key& k, GlobalAddress& p, GlobalAddress& sibling_p, uint16_t& level);

  // ========== LOCK OPERATIONS ==========
  
  static uint64_t get_lock_info(bool is_leaf);
  void lock_node(const GlobalAddress &node_addr, uint64_t* lock_buffer, bool is_leaf, CoroPull* sink);
  void unlock_node(const GlobalAddress &node_addr, uint64_t* lock_buffer, bool is_leaf, CoroPull* sink, bool async = false);

  // ========== SEARCH OPERATIONS ==========
  
  bool leaf_node_search(const GlobalAddress& node_addr, const GlobalAddress& sibling_addr, const Key &k, Value &v, bool from_cache, CoroPull* sink);
  bool internal_node_search(GlobalAddress& node_addr, GlobalAddress& sibling_addr, const Key &k, uint16_t& level, bool from_cache, CoroPull* sink);
  
  // Optimized search using cached bitmap
  bool leaf_node_search_cached(const GlobalAddress& node_addr, const Key &k, Value &v, CoroPull* sink);

  // ========== INSERT OPERATIONS ==========
  
  bool leaf_node_insert(const GlobalAddress& node_addr, const GlobalAddress& sibling_addr, const Key &k, Value v, bool from_cache, CoroPull* sink);
  bool internal_node_insert(const GlobalAddress& node_addr, const Key &k, const GlobalAddress &v, bool from_cache, uint8_t level, CoroPull* sink);
  
  // Optimized insert using cached bitmap
  bool leaf_node_insert_cached(const GlobalAddress& node_addr, const Key &k, Value v, CoroPull* sink);

  // ========== UPDATE OPERATIONS ==========
  
  bool leaf_node_update(const GlobalAddress& node_addr, const GlobalAddress& sibling_addr, const Key &k, Value v, bool from_cache, CoroPull* sink);

  // ========== DELETE OPERATIONS ==========
  
  bool leaf_node_delete(const GlobalAddress& node_addr, const Key &k, CoroPull* sink);

  // ========== HOPSCOTCH OPERATIONS ==========
  
#ifdef HOPSCOTCH_LEAF_NODE
  bool hopscotch_insert_and_unlock(LeafNode* leaf, const Key& k, Value v, const GlobalAddress& node_addr, uint64_t* lock_buffer, CoroPull* sink, int entry_num=define::leafSpanSize);
  void hopscotch_split_and_unlock(LeafNode* leaf, const Key& k, Value v, const GlobalAddress& node_addr, uint64_t* lock_buffer, CoroPull* sink);
  void hopscotch_search(const GlobalAddress& node_addr, int hash_idx, char *raw_leaf_buffer, char *leaf_buffer, CoroPull* sink, int entry_num=define::neighborSize, bool for_write=false);

  Key hopscotch_get_split_key(LeafEntry* records, const Key& k);
  int hopscotch_insert_locally(LeafEntry* records, const Key& k, Value v);
  
  // Optimized hopscotch with cached bitmap
  bool hopscotch_insert_cached(const GlobalAddress& node_addr, const Key& k, Value v, CoroPull* sink);
#endif

  // ========== SPECULATIVE READ ==========
  
#ifdef SPECULATIVE_READ
  bool speculative_read(const GlobalAddress& leaf_addr, std::pair<int, int> range, char *raw_leaf_buffer, char *leaf_buffer, const Key &k, Value &v, int& speculative_idx, CoroPull* sink, bool for_write=false);
#endif

  // ========== LOW-LEVEL OPERATIONS ==========
  
  void leaf_entry_read(const GlobalAddress& leaf_addr, const int idx, char *raw_leaf_buffer, char *leaf_buffer, CoroPull* sink, bool for_write=false);
  
  template <class NODE, class ENTRY, class VAL>
  void entry_write_and_unlock(NODE* node, const int idx, const Key& k, VAL v, const GlobalAddress& node_addr, uint64_t* lock_buffer, CoroPull* sink, bool async=false);
  
  template <class NODE, class ENTRY, int TRANS_SIZE>
  void node_write_and_unlock(NODE* node, const GlobalAddress& node_addr, uint64_t* lock_buffer, CoroPull* sink, bool async=false);
  
  void segment_write_and_unlock(LeafNode* leaf, int l_idx, int r_idx, const std::vector<int>& hopped_idxes, const GlobalAddress& node_addr, uint64_t* lock_buffer, CoroPull* sink);

  template <class NODE, class ENTRY, class VAL, int SPAN_SIZE, int ALLOC_SIZE, int TRANS_SIZE>
  void node_split_and_unlock(NODE* node, const Key& k, VAL v, const GlobalAddress& node_addr, uint64_t* lock_buffer, uint8_t level, CoroPull* sink);
  
  void insert_internal(const Key &k, const GlobalAddress& ptr, const RootEntry& root_entry, uint8_t target_level, CoroPull* sink);

  // ========== COROUTINE SUPPORT ==========
  
  void coro_worker(CoroPull &sink, RequstGen *gen, WorkFunc work_func);

private:
  // ========== STATE ==========
  
  DSM *dsm_;
  CoherentTreeConfig config_;
  
  // Trie cache for internal nodes (replaces TreeCache)
  TrieCache *trie_cache_;
  
  // Index cache for hotspot (from original CHIME)
#ifdef SPECULATIVE_READ
  IdxCache *idx_cache_;
#endif
  
  // Coherent leaf manager (bitmap cache + coherence)
  coherent_leaf::CoherentLeafManager *leaf_manager_;
  
  // Tree metadata
  uint64_t tree_id_;
  std::atomic<uint16_t> rough_height_;
  GlobalAddress root_ptr_ptr_;

public:
  LocalLockTable *local_lock_table;
  static thread_local std::vector<CoroPush> workers;
  static thread_local CoroQueue busy_waiting_queue;
};


// ============================================================================
// INLINE IMPLEMENTATIONS - CACHE OPERATIONS
// ============================================================================

inline bool CoherentTree::try_cache_hit(
  const Key& k, 
  GlobalAddress& p, 
  GlobalAddress& sibling_p, 
  uint16_t& level
) {
  if (!config_.use_trie_cache || !trie_cache_) {
    return false;
  }
  
  auto* entry = trie_cache_->search_from_cache(k, p, sibling_p, level);
  return entry != nullptr;
}


inline void CoherentTree::cache_node(InternalNode* node) {
  if (!config_.use_trie_cache || !trie_cache_) {
    return;
  }
  
#ifdef CACHE_MORE_INTERNAL_NODE
  trie_cache_->add_to_cache(node);
#else
  if (node->metadata.level == 1) {
    trie_cache_->add_to_cache(node);
  }
#endif
}


// ============================================================================
// INLINE IMPLEMENTATIONS - OPTIMIZED LEAF OPERATIONS
// ============================================================================

inline bool CoherentTree::leaf_node_search_cached(
  const GlobalAddress& node_addr, 
  const Key &k, 
  Value &v, 
  CoroPull* sink
) {
  if (!config_.use_bitmap_cache || !leaf_manager_) {
    // Fall back to regular search
    return false;
  }
  
  // Check if key might exist using cached bitmap
  int hash_idx = get_hashed_leaf_entry_index(k);
  coherent_leaf::CachedHopscotchHelper helper(*leaf_manager_);
  
  if (!helper.may_exist(node_addr, k, sink)) {
    // Definitely not in this leaf - check sibling or return not found
    return false;
  }
  
  // Key might exist - calculate minimal read range
  auto [start_idx, read_count] = helper.calculate_read_range(
    node_addr, k, false, sink
  );
  
  if (read_count == 0) {
    // No occupied slots - key doesn't exist
    return false;
  }
  
  // Need to actually read and search
  // Fall through to regular search path
  return false;  // Indicates caller should use regular path
}


inline bool CoherentTree::leaf_node_insert_cached(
  const GlobalAddress& node_addr, 
  const Key &k, 
  Value v, 
  CoroPull* sink
) {
  if (!config_.use_bitmap_cache || !leaf_manager_) {
    return false;
  }
  
  coherent_leaf::CachedHopscotchHelper helper(*leaf_manager_);
  
  // Try to find free slot using cached bitmap
  int free_slot = helper.find_insert_slot(node_addr, k, sink);
  
  if (free_slot == -2) {
    // Cache miss - need full leaf read
    return false;
  }
  
  if (free_slot >= 0) {
    // Found free slot - but still need to lock and verify
    // The cached info helps us know WHERE to write
    // Return false to fall through to regular path with this hint
    return false;
  }
  
  // free_slot == -1: Need hopping or split
  return false;
}


// ============================================================================
// STATISTICS
// ============================================================================

inline void CoherentTree::statistics() {
  printf("========== CoherentTree Statistics ==========\n");
  
  if (trie_cache_) {
    trie_cache_->statistics();
  }
  
  if (leaf_manager_) {
    leaf_manager_->print_stats();
  }
  
  printf("==============================================\n");
}


#endif // _COHERENT_TREE_H_
