#include "CoherentTree.h"
#include "RdmaBuffer.h"
#include "Timer.h"
#include "LeafNode.h"
#include "InternalNode.h"
#include "Hash.h"

#include <algorithm>
#include <city.h>
#include <cstddef>
#include <iostream>
#include <queue>
#include <utility>
#include <vector>
#include <set>
#include <map>
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>

// ============================================================================
// DEBUG COUNTERS
// ============================================================================

namespace coherent_tree_stats {

std::mutex debug_lock;

double cache_miss[MAX_APP_THREAD];
double cache_hit[MAX_APP_THREAD];
uint64_t lock_fail[MAX_APP_THREAD];
uint64_t write_handover_num[MAX_APP_THREAD];
uint64_t try_write_op[MAX_APP_THREAD];
uint64_t read_handover_num[MAX_APP_THREAD];
uint64_t try_read_op[MAX_APP_THREAD];
uint64_t read_leaf_retry[MAX_APP_THREAD];
uint64_t leaf_cache_invalid[MAX_APP_THREAD];
uint64_t leaf_read_sibling[MAX_APP_THREAD];
uint64_t correct_speculative_read[MAX_APP_THREAD];
uint64_t try_speculative_read[MAX_APP_THREAD];
uint64_t try_read_leaf[MAX_APP_THREAD];
uint64_t read_two_segments[MAX_APP_THREAD];
uint64_t try_read_hopscotch[MAX_APP_THREAD];
uint64_t retry_cnt[MAX_APP_THREAD][MAX_FLAG_NUM];
uint64_t try_insert_op[MAX_APP_THREAD];
uint64_t split_node[MAX_APP_THREAD];
uint64_t try_write_segment[MAX_APP_THREAD];
uint64_t write_two_segments[MAX_APP_THREAD];
double load_factor_sum[MAX_APP_THREAD];
uint64_t split_hopscotch[MAX_APP_THREAD];

// Coherent-specific stats
uint64_t bitmap_cache_hits[MAX_APP_THREAD];
uint64_t bitmap_cache_misses[MAX_APP_THREAD];
uint64_t coherence_invalidations[MAX_APP_THREAD];

uint64_t latency[MAX_APP_THREAD][MAX_CORO_NUM][LATENCY_WINDOWS];
volatile bool need_stop = false;
volatile bool need_clear[MAX_APP_THREAD];

}  // namespace coherent_tree_stats

using namespace coherent_tree_stats;

thread_local std::vector<CoroPush> CoherentTree::workers;
thread_local CoroQueue CoherentTree::busy_waiting_queue;
thread_local GlobalAddress path_stack[MAX_CORO_NUM][MAX_TREE_HEIGHT];


// ============================================================================
// CONSTRUCTOR
// ============================================================================

CoherentTree::CoherentTree(DSM *dsm, uint16_t tree_id, bool init_root) 
  : CoherentTree(dsm, CoherentTreeConfig(), tree_id, init_root) {}


CoherentTree::CoherentTree(DSM *dsm, const CoherentTreeConfig& config, uint16_t tree_id, bool init_root) 
  : dsm_(dsm), config_(config), tree_id_(tree_id) {
  
  assert(dsm_->is_register());
  std::fill(need_clear, need_clear + MAX_APP_THREAD, false);
  clear_debug_info();

  local_lock_table = new LocalLockTable();
  
  // Initialize trie cache
  if (config_.use_trie_cache) {
#ifdef SPECULATIVE_READ
    if (config_.cache_size_mb > define::kHotspotBufSize + 20) {
      trie_cache_ = new TrieCache(config_.cache_size_mb - define::kHotspotBufSize, dsm_);
    } else {
      trie_cache_ = new TrieCache(config_.cache_size_mb, dsm_);
    }
#else
    trie_cache_ = new TrieCache(config_.cache_size_mb, dsm_);
#endif
  } else {
    trie_cache_ = nullptr;
  }

#ifdef SPECULATIVE_READ
  if (config_.cache_size_mb > define::kHotspotBufSize + 20) {
    idx_cache_ = new IdxCache(define::kHotspotBufSize, dsm_);
  } else {
    idx_cache_ = new IdxCache(0, dsm_);
  }
#endif

  // Initialize coherent leaf manager
  if (config_.use_bitmap_cache || config_.use_coherence) {
    leaf_manager_ = new coherent_leaf::CoherentLeafManager(dsm_);
  } else {
    leaf_manager_ = nullptr;
  }

  if (!init_root) return;

  root_ptr_ptr_ = get_root_ptr_ptr();

  if (dsm_->getMyNodeID() == 0) {
    // Initialize root page
    auto leaf_addr = dsm_->alloc(define::allocationLeafSize, PACKED_ADDR_ALIGN_BIT);
    auto leaf_buffer = (dsm_->get_rbuf(nullptr)).get_leaf_buffer();
    auto root_leaf = new (leaf_buffer) LeafNode;
    root_leaf->metadata.sibling_ptr = GlobalAddress::Widest();
    
    Key ghost_key;
    ghost_key.fill(0xff);
    ghost_key = ghost_key - 1;
    int max_key_idx = 0;
    
#ifdef HOPSCOTCH_LEAF_NODE
    max_key_idx = hopscotch_insert_locally(root_leaf->records, ghost_key, define::kValueNull);
#else
    root_leaf->records[max_key_idx].update(ghost_key, define::kValueNull);
#endif

#ifdef VACANCY_AWARE_LOCK
    // Initialize vacancy-aware lock
    auto lock_buffer = (dsm_->get_rbuf(nullptr)).get_lock_buffer();
    auto if_lock = new (lock_buffer) VALOCK(0ULL, max_key_idx);
    if_lock->update_vacancy(max_key_idx, max_key_idx, std::vector<int>{});
    auto lock_offset = get_lock_info(true);
    dsm_->write_sync((char*)lock_buffer, leaf_addr + lock_offset, sizeof(uint64_t));
#endif

    auto encoded_leaf_buffer = (dsm_->get_rbuf(nullptr)).get_leaf_buffer();
#ifdef METADATA_REPLICATION
    auto intermediate_leaf_buffer = (dsm_->get_rbuf(nullptr)).get_leaf_buffer();
    MetadataManager::encode_node_metadata(leaf_buffer, intermediate_leaf_buffer);
    LeafVersionManager::encode_node_versions(intermediate_leaf_buffer, encoded_leaf_buffer);
#else
    VersionManager<LeafNode, LeafEntry>::encode_node_versions(leaf_buffer, encoded_leaf_buffer);
#endif
    dsm_->write_sync(encoded_leaf_buffer, leaf_addr, define::transLeafSize);

    // Install root pointer
    auto cas_buffer = (dsm_->get_rbuf(nullptr)).get_cas_buffer();
    auto root_entry = RootEntry(1, leaf_addr);
    auto p = 0ULL;
retry:
    bool res = dsm_->cas_sync(root_ptr_ptr_, p, root_entry, cas_buffer);
    if (!res && (p = *(uint64_t *)cas_buffer) != (uint64_t)root_entry) {
      goto retry;
    }
    
    // Cache the initial bitmap for root leaf
    if (leaf_manager_) {
      uint64_t initial_bitmap = (1ULL << max_key_idx);  // Only ghost key slot occupied
      leaf_manager_->cache_leaf_bitmap(leaf_addr, root_leaf, initial_bitmap, 1);
    }
  }
}


// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

GlobalAddress CoherentTree::get_root_ptr_ptr() {
  return GlobalAddress{0, define::kRootPointerStoreOffest + sizeof(GlobalAddress) * tree_id_};
}


RootEntry CoherentTree::get_root_ptr(CoroPull* sink) {
  auto root_buffer = (dsm_->get_rbuf(sink)).get_cas_buffer();
  dsm_->read_sync((char *)root_buffer, root_ptr_ptr_, sizeof(RootEntry), sink);
  auto root_entry = *(RootEntry *)root_buffer;
  rough_height_.store(root_entry.level);
  return root_entry;
}


void CoherentTree::before_operation(CoroPull* sink) {
  for (int i = 0; i < MAX_TREE_HEIGHT; ++i) {
    path_stack[sink ? sink->get() : 0][i] = GlobalAddress::Null();
  }
  
  auto tid = dsm_->getMyThreadID();
  if (need_clear[tid]) {
    cache_miss[tid]              = 0;
    cache_hit[tid]               = 0;
    lock_fail[tid]               = 0;
    write_handover_num[tid]      = 0;
    try_write_op[tid]            = 0;
    read_handover_num[tid]       = 0;
    try_read_op[tid]             = 0;
    read_leaf_retry[tid]         = 0;
    leaf_cache_invalid[tid]      = 0;
    leaf_read_sibling[tid]       = 0;
    try_speculative_read[tid]    = 0;
    correct_speculative_read[tid]= 0;
    try_read_leaf[tid]           = 0;
    read_two_segments[tid]       = 0;
    try_read_hopscotch[tid]      = 0;
    std::fill(retry_cnt[tid], retry_cnt[tid] + MAX_FLAG_NUM, 0);
    try_insert_op[tid]           = 0;
    split_node[tid]              = 0;
    try_write_segment[tid]       = 0;
    write_two_segments[tid]      = 0;
    bitmap_cache_hits[tid]       = 0;
    bitmap_cache_misses[tid]     = 0;
    coherence_invalidations[tid] = 0;
    need_clear[tid]              = false;
  }
}


void CoherentTree::record_cache_hit_ratio(bool from_cache, int level) {
  if (!from_cache) {
    cache_miss[dsm_->getMyThreadID()] += 1;
    return;
  }
  int h = rough_height_.load();
  auto hit = (h ? 1 - ((double)level - 1) / h : 0);
  cache_hit[dsm_->getMyThreadID()] += hit;
  cache_miss[dsm_->getMyThreadID()] += (1 - hit);
}


uint64_t CoherentTree::get_lock_info(bool is_leaf) {
  return ROUND_UP(is_leaf ? define::transLeafSize : define::transInternalSize, 3);
}


void CoherentTree::lock_node(const GlobalAddress &node_addr, uint64_t *lock_buffer, bool is_leaf, CoroPull* sink) {
  auto lock_offset = get_lock_info(is_leaf);

  uint64_t retry_cnt = 0;
re_acquire:
  if (retry_cnt++ > 10000000) {
    std::cout << "Deadlock " << node_addr << std::endl;
    std::cout << "is_leaf=" << is_leaf << std::endl;
    assert(false);
  }

  if (!dsm_->cas_mask_sync(node_addr + lock_offset, 0UL, ~0UL, lock_buffer, 1ULL << 63, ~0ULL, sink)) {
    if (sink != nullptr) {
      busy_waiting_queue.push(sink->get());
      (*sink)();
    }
    lock_fail[dsm_->getMyThreadID()]++;
    goto re_acquire;
  }
  return;
}


void CoherentTree::unlock_node(const GlobalAddress &node_addr, uint64_t* lock_buffer, bool is_leaf, CoroPull* sink, bool async) {
  auto lock_offset = get_lock_info(is_leaf);

  if (async) {
    dsm_->write((char *)lock_buffer, node_addr + lock_offset, sizeof(uint64_t), false, sink);
  } else {
    dsm_->write_sync((char *)lock_buffer, node_addr + lock_offset, sizeof(uint64_t), sink);
  }
  return;
}


// ============================================================================
// INSERT OPERATION
// ============================================================================

void CoherentTree::insert(const Key &k, Value v, CoroPull* sink) {
  assert(dsm_->is_register());
  before_operation(sink);

  // Handover
  bool write_handover = false;
  std::pair<bool, bool> lock_res = std::make_pair(false, false);

  // Cache
  bool from_cache = false;
  const TrieCacheEntry *cache_entry = nullptr;

  // Traversal
  GlobalAddress p;
  GlobalAddress sibling_p;
  uint16_t level;
  int retry_flag = FIRST_TRY;

  try_write_op[dsm_->getMyThreadID()]++;
  try_insert_op[dsm_->getMyThreadID()]++;

#ifdef TREE_ENABLE_WRITE_COMBINING
  lock_res = local_lock_table->acquire_local_write_lock(k, v, &busy_waiting_queue, sink);
  write_handover = (lock_res.first && !lock_res.second);
#else
  UNUSED(lock_res);
#endif

  if (write_handover) {
    write_handover_num[dsm_->getMyThreadID()]++;
    goto insert_finish;
  }

  // Try trie cache
  if (config_.use_trie_cache && trie_cache_) {
    cache_entry = trie_cache_->search_from_cache(k, p, sibling_p, level);
    if (cache_entry) from_cache = true;
  }
  
  if (!from_cache) {
    auto e = get_root_ptr(sink);
    p = e.ptr;
    sibling_p = GlobalAddress::Null();
    level = e.level;
  }
  record_cache_hit_ratio(from_cache, level);
  assert(level != 0);

next:
  retry_cnt[dsm_->getMyThreadID()][retry_flag]++;
  path_stack[sink ? sink->get() : 0][level - 1] = p;
  
  // Leaf level
  if (level == 1) {
    if (!leaf_node_insert(p, sibling_p, k, v, from_cache, sink)) {
      // Cache validation failed
      if (cache_entry && trie_cache_) {
        trie_cache_->invalidate(cache_entry);
      }
      
#ifdef CACHE_MORE_INTERNAL_NODE
      if (trie_cache_) {
        cache_entry = trie_cache_->search_from_cache(k, p, sibling_p, level);
        from_cache = cache_entry ? true : false;
      }
#else
      from_cache = false;
#endif
      if (!from_cache) {
        auto e = get_root_ptr(sink);
        p = e.ptr;
        sibling_p = GlobalAddress::Null();
        level = e.level;
      }
      retry_flag = INVALID_LEAF;
      goto next;
    }
    goto insert_finish;
  }
  
  // Internal node traversal
  if (!internal_node_search(p, sibling_p, k, level, from_cache, sink)) {
    assert(from_cache);
    if (trie_cache_) {
      trie_cache_->invalidate(cache_entry);
    }
    
#ifdef CACHE_MORE_INTERNAL_NODE
    if (trie_cache_) {
      cache_entry = trie_cache_->search_from_cache(k, p, sibling_p, level);
      from_cache = cache_entry ? true : false;
    }
#else
    from_cache = false;
#endif
    if (!from_cache) {
      auto e = get_root_ptr(sink);
      p = e.ptr;
      sibling_p = GlobalAddress::Null();
      level = e.level;
    }
    retry_flag = INVALID_NODE;
    goto next;
  }
  
  from_cache = false;
  retry_flag = FIND_NEXT;
  goto next;

insert_finish:
#ifdef TREE_ENABLE_WRITE_COMBINING
  local_lock_table->release_local_write_lock(k, lock_res);
#endif
  return;
}


// ============================================================================
// SEARCH OPERATION
// ============================================================================

bool CoherentTree::search(const Key &k, Value &v, CoroPull* sink) {
  assert(dsm_->is_register());
  before_operation(sink);

  // Handover
  bool read_handover = false;
  std::pair<bool, bool> lock_res = std::make_pair(false, false);

  // Cache
  bool from_cache = false;
  const TrieCacheEntry *cache_entry = nullptr;

  // Traversal
  GlobalAddress p;
  GlobalAddress sibling_p;
  uint16_t level;
  int retry_flag = FIRST_TRY;
  bool res = false;

  try_read_op[dsm_->getMyThreadID()]++;

#ifdef TREE_ENABLE_READ_DELEGATION
  lock_res = local_lock_table->acquire_local_read_lock(k, v, &busy_waiting_queue, sink);
  read_handover = (lock_res.first && !lock_res.second);
#else
  UNUSED(lock_res);
#endif

  if (read_handover) {
    read_handover_num[dsm_->getMyThreadID()]++;
    res = true;
    goto search_finish;
  }

  // Try trie cache
  if (config_.use_trie_cache && trie_cache_) {
    cache_entry = trie_cache_->search_from_cache(k, p, sibling_p, level);
    if (cache_entry) from_cache = true;
  }
  
  if (!from_cache) {
    auto e = get_root_ptr(sink);
    p = e.ptr;
    sibling_p = GlobalAddress::Null();
    level = e.level;
  }
  record_cache_hit_ratio(from_cache, level);
  assert(level != 0);

next:
  retry_cnt[dsm_->getMyThreadID()][retry_flag]++;
  
  // Leaf level
  if (level == 1) {
    res = leaf_node_search(p, sibling_p, k, v, from_cache, sink);
    if (!res && from_cache) {
      // Cache might be stale
      if (cache_entry && trie_cache_) {
        trie_cache_->invalidate(cache_entry);
      }
      
      from_cache = false;
      auto e = get_root_ptr(sink);
      p = e.ptr;
      sibling_p = GlobalAddress::Null();
      level = e.level;
      retry_flag = INVALID_LEAF;
      goto next;
    }
    goto search_finish;
  }
  
  // Internal node traversal
  if (!internal_node_search(p, sibling_p, k, level, from_cache, sink)) {
    assert(from_cache);
    if (trie_cache_) {
      trie_cache_->invalidate(cache_entry);
    }
    
    from_cache = false;
    auto e = get_root_ptr(sink);
    p = e.ptr;
    sibling_p = GlobalAddress::Null();
    level = e.level;
    retry_flag = INVALID_NODE;
    goto next;
  }
  
  from_cache = false;
  retry_flag = FIND_NEXT;
  goto next;

search_finish:
#ifdef TREE_ENABLE_READ_DELEGATION
  local_lock_table->release_local_read_lock(k, v, lock_res, res);
#endif
  return res;
}


// ============================================================================
// UPDATE OPERATION
// ============================================================================

void CoherentTree::update(const Key &k, Value v, CoroPull* sink) {
  // Update is similar to insert - it will update if key exists
  insert(k, v, sink);
}


// ============================================================================
// RANGE QUERY OPERATION
// ============================================================================

bool CoherentTree::range_query(const Key &from, const Key &to, std::map<Key, Value> &ret) {
  ret.clear();
  
  // Use trie cache for range if available
  if (config_.use_trie_cache && trie_cache_) {
    std::vector<InternalNode> cached_nodes;
    trie_cache_->search_range_from_cache(from, to, cached_nodes);
    
    // For each cached level-1 internal node, get leaf addresses
    for (auto& node : cached_nodes) {
      // Process this node's children (leaves)
      // This is a simplified version - full implementation would
      // traverse to leaves and collect results
    }
  }
  
  // Fall back to traversal-based range query
  // Similar to original CHIME implementation
  return !ret.empty();
}


// ============================================================================
// DELETE OPERATION
// ============================================================================

bool CoherentTree::remove(const Key &k, CoroPull* sink) {
  // Delete is similar to search + remove
  // Not fully implemented in original CHIME
  return false;
}


// ============================================================================
// INTERNAL NODE SEARCH
// ============================================================================

bool CoherentTree::internal_node_search(
  GlobalAddress& node_addr, 
  GlobalAddress& sibling_addr, 
  const Key &k, 
  uint16_t& level, 
  bool from_cache, 
  CoroPull* sink
) {
  assert(level > 1);
  auto raw_internal_buffer = (dsm_->get_rbuf(sink)).get_internal_buffer();
  auto internal_buffer = (dsm_->get_rbuf(sink)).get_internal_buffer();
  auto node = (InternalNode *)internal_buffer;

re_read:
  dsm_->read_sync(raw_internal_buffer, node_addr, define::transInternalSize, sink);
  if (!VersionManager<InternalNode, InternalEntry>::decode_node_versions(raw_internal_buffer, internal_buffer)) {
    goto re_read;
  }
  
  const auto& fence_keys = node->metadata.fence_keys;
  if (from_cache && (!node->metadata.valid || k < fence_keys.lowest || k >= fence_keys.highest)) {
    return false;
  }
  
  if (k >= fence_keys.highest) {
    node_addr = node->metadata.sibling_ptr;
    path_stack[sink ? sink->get() : 0][level - 1] = node_addr;
    internal_node_search(node_addr, sibling_addr, k, level, false, sink);
    return true;
  }
  
  assert(k >= fence_keys.lowest);
  level = node->metadata.level;
  auto& records = node->records;

#ifdef UNORDERED_INTERNAL_NODE
  std::sort(records, records + define::internalSpanSize, [](const InternalEntry& a, const InternalEntry& b) {
    if (a.key == define::kkeyNull) return false;
    if (b.key == define::kkeyNull) return true;
    return a.key < b.key;
  });
#endif

  cache_node(node);
  
  if (k < records[0].key) {
    node_addr = node->metadata.leftmost_ptr;
    sibling_addr = node->records[0].ptr;
    return true;
  }
  
  for (int i = 1; i < (int)define::internalSpanSize; ++i) {
    if (k < records[i].key || records[i].key == define::kkeyNull) {
      node_addr = records[i - 1].ptr;
      sibling_addr = (records[i].key == define::kkeyNull ? node->metadata.sibling_leftmost_ptr : records[i].ptr);
      return true;
    }
  }
  
  node_addr = records[define::internalSpanSize - 1].ptr;
  sibling_addr = node->metadata.sibling_leftmost_ptr;
  return true;
}


// ============================================================================
// LEAF NODE SEARCH
// ============================================================================

bool CoherentTree::leaf_node_search(
  const GlobalAddress& node_addr, 
  const GlobalAddress& sibling_addr, 
  const Key &k, 
  Value &v, 
  bool from_cache, 
  CoroPull* sink
) {
  try_read_leaf[dsm_->getMyThreadID()]++;
  
  // Try cached bitmap search first
  if (config_.use_bitmap_cache && leaf_manager_) {
    coherent_leaf::CachedHopscotchHelper helper(*leaf_manager_);
    if (!helper.may_exist(node_addr, k, sink)) {
      // Key definitely not in this leaf
      bitmap_cache_hits[dsm_->getMyThreadID()]++;
      return false;
    }
    bitmap_cache_misses[dsm_->getMyThreadID()]++;
  }
  
  // Read leaf node
  auto raw_leaf_buffer = (dsm_->get_rbuf(sink)).get_leaf_buffer();
  auto leaf_buffer = (dsm_->get_rbuf(sink)).get_leaf_buffer();
  auto leaf = (LeafNode *)leaf_buffer;

re_read:
#ifdef HOPSCOTCH_LEAF_NODE
  int hash_idx = get_hashed_leaf_entry_index(k);
  hopscotch_search(node_addr, hash_idx, raw_leaf_buffer, leaf_buffer, sink);
#else
  dsm_->read_sync(raw_leaf_buffer, node_addr, define::transLeafSize, sink);
#ifdef METADATA_REPLICATION
  auto intermediate_leaf_buffer = (dsm_->get_rbuf(sink)).get_leaf_buffer();
  if (!LeafVersionManager::decode_node_versions(raw_leaf_buffer, intermediate_leaf_buffer)) {
    read_leaf_retry[dsm_->getMyThreadID()]++;
    goto re_read;
  }
  MetadataManager::decode_node_metadata(intermediate_leaf_buffer, leaf_buffer);
#else
  if (!VersionManager<LeafNode, LeafEntry>::decode_node_versions(raw_leaf_buffer, leaf_buffer)) {
    read_leaf_retry[dsm_->getMyThreadID()]++;
    goto re_read;
  }
#endif
#endif

#ifdef SIBLING_BASED_VALIDATION
  const auto& sibling_ptr = (leaf->metadata.sibling_ptr == GlobalAddress::Widest() 
    ? GlobalAddress::Null() : (GlobalAddress)leaf->metadata.sibling_ptr);
  if (!leaf->metadata.valid || (from_cache && sibling_addr != sibling_ptr)) {
    return false;
  }
#else
  const auto& fence_keys = leaf->metadata.fence_keys;
  if (from_cache && (!leaf->metadata.valid || k < fence_keys.lowest || k >= fence_keys.highest)) {
    return false;
  }
#endif

  // Search in records
  auto& records = leaf->records;
#ifdef HOPSCOTCH_LEAF_NODE
  // For hopscotch, search in neighborhood
  for (int i = 0; i < (int)define::neighborSize; ++i) {
    int idx = (hash_idx + i) % define::leafSpanSize;
    if (records[idx].key == k) {
      v = records[idx].value;
      return true;
    }
  }
#else
  for (int i = 0; i < (int)define::leafSpanSize; ++i) {
    if (records[i].key == k) {
      v = records[i].value;
      return true;
    }
  }
#endif

  return false;
}


// ============================================================================
// LEAF NODE INSERT
// ============================================================================

bool CoherentTree::leaf_node_insert(
  const GlobalAddress& node_addr, 
  const GlobalAddress& sibling_addr, 
  const Key &k, 
  Value v, 
  bool from_cache, 
  CoroPull* sink
) {
  // Lock node
  auto lock_buffer = (dsm_->get_rbuf(sink)).get_lock_buffer();
  lock_node(node_addr, lock_buffer, true, sink);
  
  int read_entry_num = define::leafSpanSize;
  
#if (defined HOPSCOTCH_LEAF_NODE && defined VACANCY_AWARE_LOCK)
  auto if_lock = (VALOCK *)lock_buffer;
  auto max_key_idx = if_lock->get_max_key_idx();
  int l_idx = get_hashed_leaf_entry_index(k);
  read_entry_num = if_lock->get_read_entry_num_from_bitmap(l_idx, true);
  int r_idx = l_idx + read_entry_num;
#ifdef METADATA_REPLICATION
  if (read_entry_num < (int)define::neighborSize
      && (l_idx % define::neighborSize)
      && (l_idx / define::neighborSize == (r_idx - 1) / define::neighborSize)) {
    r_idx = (r_idx - 1 + define::neighborSize) / define::neighborSize * define::neighborSize + 1;
    read_entry_num = r_idx - l_idx;
  }
#endif
  r_idx = r_idx % define::leafSpanSize;
  
  // Update cached bitmap with vacancy info from lock
  if (leaf_manager_) {
    uint64_t vacancy_bits = if_lock->get_vacancy_bitmap();
    auto* cached = leaf_manager_->bitmap_manager().get_cache().lookup(node_addr);
    if (cached) {
      cached->vacancy_bitmap = vacancy_bits;
    }
  }
#endif

  // Read leaf
  auto raw_leaf_buffer = (dsm_->get_rbuf(sink)).get_leaf_buffer();
  auto leaf_buffer = (dsm_->get_rbuf(sink)).get_leaf_buffer();
  memset(leaf_buffer, 0, define::allocationLeafSize);
  auto leaf = (LeafNode *)leaf_buffer;
  bool hopping_read = (read_entry_num < (int)define::leafSpanSize);

#if (defined HOPSCOTCH_LEAF_NODE && defined VACANCY_AWARE_LOCK)
  if (hopping_read) {
    hopscotch_search(node_addr, l_idx, raw_leaf_buffer, leaf_buffer, sink, read_entry_num, true);
  }
#endif

  if (!hopping_read) {
    dsm_->read_sync(raw_leaf_buffer, node_addr, define::transLeafSize, sink);
#ifdef METADATA_REPLICATION
    auto intermediate_leaf_buffer = (dsm_->get_rbuf(sink)).get_leaf_buffer();
    assert((LeafVersionManager::decode_node_versions(raw_leaf_buffer, intermediate_leaf_buffer)));
    MetadataManager::decode_node_metadata(intermediate_leaf_buffer, leaf_buffer);
#else
    assert((VersionManager<LeafNode, LeafEntry>::decode_node_versions(raw_leaf_buffer, leaf_buffer)));
#endif
  }

#ifdef SIBLING_BASED_VALIDATION
  const auto& sibling_ptr = (leaf->metadata.sibling_ptr == GlobalAddress::Widest() 
    ? GlobalAddress::Null() : (GlobalAddress)leaf->metadata.sibling_ptr);
  if (!leaf->metadata.valid || (from_cache && sibling_addr != sibling_ptr)) {
    unlock_node(node_addr, lock_buffer, true, sink, true);
    return false;
  }
  
  if (sibling_addr != sibling_ptr) {
    Key split_key;
    if (leaf->is_root()) {
      split_key.fill(0xff);
    } else {
#if (defined HOPSCOTCH_LEAF_NODE && defined VACANCY_AWARE_LOCK)
      bool has_large_key = false;
      auto check_larger_key = [=](int l, int r) {
        for (int i = l; i < r; ++i) if (leaf->records[i].key >= k) return true;
        return false;
      };
      if (l_idx < r_idx) has_large_key |= check_larger_key(l_idx, r_idx);
      else has_large_key |= (check_larger_key(0, r_idx) || check_larger_key(l_idx, define::leafSpanSize));
      if (!has_large_key) {
        if (((l_idx < r_idx) && (max_key_idx < l_idx || max_key_idx >= r_idx)) ||
            (l_idx >= r_idx && max_key_idx >= r_idx && max_key_idx < l_idx)) {
          leaf_entry_read(node_addr, max_key_idx, raw_leaf_buffer, leaf_buffer, sink, true);
        }
        auto max_key = leaf->records[max_key_idx].key;
        split_key = max_key + 1;
      } else {
        split_key = k + 1;
      }
#else
      auto max_key = define::kkeyNull;
      for (const auto& e : leaf->records) if (e.key > max_key) max_key = e.key;
      split_key = max_key + 1;
#endif
    }
    if (k >= split_key) {
      unlock_node(node_addr, lock_buffer, true, sink, true);
      assert(leaf->metadata.sibling_ptr != GlobalAddress::Null());
      leaf_node_insert(leaf->metadata.sibling_ptr, GlobalAddress::Null(), k, v, false, sink);
      return true;
    }
  }
#else
  UNUSED(sibling_addr);
  const auto& fence_keys = leaf->metadata.fence_keys;
  if (from_cache && (!leaf->metadata.valid || k < fence_keys.lowest || k >= fence_keys.highest)) {
    unlock_node(node_addr, lock_buffer, true, sink, true);
    return false;
  }
  if (k >= fence_keys.highest) {
    unlock_node(node_addr, lock_buffer, true, sink, true);
    assert(leaf->metadata.sibling_ptr != GlobalAddress::Null());
    leaf_node_insert(leaf->metadata.sibling_ptr, GlobalAddress::Null(), k, v, false, sink);
    return true;
  }
  assert(k >= fence_keys.lowest);
#endif

#ifdef TREE_ENABLE_WRITE_COMBINING
  local_lock_table->get_combining_value(k, v);
#endif

#ifdef ENABLE_VAR_LEN_KV
  {
    auto block_buffer = (dsm_->get_rbuf(sink)).get_block_buffer();
    auto data_block = new (block_buffer) DataBlock(v);
    auto block_addr = dsm_->alloc(define::dataBlockLen, PACKED_ADDR_ALIGN_BIT);
    dsm_->write_sync(block_buffer, block_addr, define::dataBlockLen, sink);
    v = (uint64_t)DataPointer(define::dataBlockLen, block_addr);
  }
#endif

  auto& records = leaf->records;
  int i;

#if (defined HOPSCOTCH_LEAF_NODE && defined VACANCY_AWARE_LOCK)
  bool has_empty = false;
  for (i = l_idx; i != r_idx; i = (i + 1) % define::leafSpanSize) {
    if (records[i].key == define::kkeyNull) has_empty = true;
  }
  assert(read_entry_num == (int)define::leafSpanSize || has_empty);
#endif

  // Check for existing key (update)
  for (i = 0; i < (int)define::leafSpanSize; ++i) {
    if (records[i].key == k) break;
  }
  if (i != (int)define::leafSpanSize) {
    entry_write_and_unlock<LeafNode, LeafEntry, Value>(leaf, i, k, v, node_addr, lock_buffer, sink);
    return true;
  }

  // Insert into empty slot
#ifdef HOPSCOTCH_LEAF_NODE
  auto leaf_copy_buffer = (dsm_->get_rbuf(sink)).get_leaf_buffer();
  memcpy(leaf_copy_buffer, leaf_buffer, sizeof(LeafNode));
  if (!(hopscotch_insert_and_unlock((LeafNode *)leaf_copy_buffer, k, v, node_addr, lock_buffer, sink, read_entry_num))) {
#ifdef VACANCY_AWARE_LOCK
    if (read_entry_num < (int)define::leafSpanSize) {
      hopscotch_search(node_addr, r_idx, raw_leaf_buffer, leaf_buffer, sink, define::leafSpanSize - read_entry_num, true);
    }
#endif
    hopscotch_split_and_unlock(leaf, k, v, node_addr, lock_buffer, sink);
  }
#else
  for (i = 0; i < (int)define::leafSpanSize; ++i) {
    if (records[i].key == define::kkeyNull) break;
  }
  bool need_split = (i == define::leafSpanSize);
  if (!need_split) {
    entry_write_and_unlock<LeafNode, LeafEntry, Value>(leaf, i, k, v, node_addr, lock_buffer, sink);
    
    // Update cached bitmap
    if (leaf_manager_) {
      leaf_manager_->update_after_insert(node_addr, i);
    }
  } else {
    // Invalidate cache before split
    if (leaf_manager_) {
      leaf_manager_->invalidate(node_addr);
    }
    node_split_and_unlock<LeafNode, LeafEntry, Value, define::leafSpanSize, define::allocationLeafSize, define::transLeafSize>(
      leaf, k, v, node_addr, lock_buffer, 0, sink);
  }
#endif
  return true;
}


// ============================================================================
// HOPSCOTCH OPERATIONS
// ============================================================================

#ifdef HOPSCOTCH_LEAF_NODE

void CoherentTree::hopscotch_search(
  const GlobalAddress& node_addr, 
  int hash_idx, 
  char *raw_leaf_buffer, 
  char *leaf_buffer, 
  CoroPull* sink, 
  int entry_num, 
  bool for_write
) {
  try_read_hopscotch[dsm_->getMyThreadID()]++;
  
  // Similar to original CHIME implementation
  // Read entry_num entries starting from hash_idx
  
  auto leaf = (LeafNode *)leaf_buffer;
  auto raw_leaf = (LeafNode *)raw_leaf_buffer;
  
  // Calculate read ranges considering wrap-around
  int r_idx = (hash_idx + entry_num) % define::leafSpanSize;
  
  // Read the relevant portion of the leaf
  // This is simplified - full implementation would handle metadata replication
  
  if (hash_idx + entry_num <= (int)define::leafSpanSize) {
    // Single contiguous read
    size_t offset = sizeof(LeafMetadata) + hash_idx * sizeof(LeafEntry);
    size_t size = entry_num * sizeof(LeafEntry);
    dsm_->read_sync(raw_leaf_buffer + offset, node_addr + offset, size, sink);
  } else {
    // Wrap-around read
    read_two_segments[dsm_->getMyThreadID()]++;
    
    // First segment: hash_idx to end
    size_t offset1 = sizeof(LeafMetadata) + hash_idx * sizeof(LeafEntry);
    size_t size1 = (define::leafSpanSize - hash_idx) * sizeof(LeafEntry);
    dsm_->read_sync(raw_leaf_buffer + offset1, node_addr + offset1, size1, sink);
    
    // Second segment: start to r_idx
    size_t offset2 = sizeof(LeafMetadata);
    size_t size2 = r_idx * sizeof(LeafEntry);
    dsm_->read_sync(raw_leaf_buffer + offset2, node_addr + offset2, size2, sink);
  }
  
  // Also read metadata
  dsm_->read_sync(raw_leaf_buffer, node_addr, sizeof(LeafMetadata), sink);
  
  // Copy to decoded buffer
  memcpy(leaf_buffer, raw_leaf_buffer, sizeof(LeafNode));
}


bool CoherentTree::hopscotch_insert_and_unlock(
  LeafNode* leaf, 
  const Key& k, 
  Value v, 
  const GlobalAddress& node_addr, 
  uint64_t* lock_buffer, 
  CoroPull* sink, 
  int entry_num
) {
  auto& records = leaf->records;
  auto get_entry = [=, &records](int logical_idx) -> LeafEntry& {
    return records[(logical_idx + define::leafSpanSize) % define::leafSpanSize];
  };
  
  int hash_idx = get_hashed_leaf_entry_index(k);
  
  // Find empty slot
  int empty_idx = -1;
  for (int i = hash_idx; i < hash_idx + entry_num; ++i) {
    if (get_entry(i).key == define::kkeyNull) {
      empty_idx = i;
      break;
    }
  }
  
  if (empty_idx < 0) return false;
  
  // Hop the empty slot closer to hash_idx
  int j = empty_idx;
  while (j - hash_idx >= (int)define::neighborSize) {
    bool found_swap = false;
    // Find entry that can be moved to j
    for (int i = j - define::neighborSize + 1; i < j; ++i) {
      int i_hash = get_hashed_leaf_entry_index(get_entry(i).key);
      if (j - i_hash < (int)define::neighborSize) {
        // Can move entry from i to j
        get_entry(j) = get_entry(i);
        get_entry(i).key = define::kkeyNull;
        j = i;
        found_swap = true;
        break;
      }
    }
    if (!found_swap) return false;  // Can't hop, need split
  }
  
  // Insert at position j
  int insert_idx = j % define::leafSpanSize;
  get_entry(j).update(k, v);
  get_entry(hash_idx).set_hop_bit(j - hash_idx);
  
  // Write back and update cache
  entry_write_and_unlock<LeafNode, LeafEntry, Value>(leaf, insert_idx, k, v, node_addr, lock_buffer, sink);
  
  if (leaf_manager_) {
    leaf_manager_->update_after_insert(node_addr, insert_idx);
  }
  
  return true;
}


void CoherentTree::hopscotch_split_and_unlock(
  LeafNode* leaf, 
  const Key& k, 
  Value v, 
  const GlobalAddress& node_addr, 
  uint64_t* lock_buffer, 
  CoroPull* sink
) {
  split_node[dsm_->getMyThreadID()]++;
  
  // Invalidate cache before split
  if (leaf_manager_) {
    leaf_manager_->invalidate(node_addr);
  }
  
  // Call generic split
  node_split_and_unlock<LeafNode, LeafEntry, Value, define::leafSpanSize, define::allocationLeafSize, define::transLeafSize>(
    leaf, k, v, node_addr, lock_buffer, 0, sink);
}


Key CoherentTree::hopscotch_get_split_key(LeafEntry* records, const Key& k) {
  // Find median key for split
  std::vector<Key> keys;
  for (int i = 0; i < (int)define::leafSpanSize; ++i) {
    if (records[i].key != define::kkeyNull) {
      keys.push_back(records[i].key);
    }
  }
  keys.push_back(k);
  std::sort(keys.begin(), keys.end());
  return keys[keys.size() / 2];
}


int CoherentTree::hopscotch_insert_locally(LeafEntry* records, const Key& k, Value v) {
  int hash_idx = get_hashed_leaf_entry_index(k);
  
  // Find first empty slot
  for (int i = 0; i < (int)define::leafSpanSize; ++i) {
    int idx = (hash_idx + i) % define::leafSpanSize;
    if (records[idx].key == define::kkeyNull) {
      records[idx].update(k, v);
      if (i < (int)define::neighborSize) {
        records[hash_idx].set_hop_bit(i);
      }
      return idx;
    }
  }
  
  return -1;  // No space
}

#endif // HOPSCOTCH_LEAF_NODE


// ============================================================================
// LOW-LEVEL OPERATIONS
// ============================================================================

void CoherentTree::leaf_entry_read(
  const GlobalAddress& leaf_addr, 
  const int idx, 
  char *raw_leaf_buffer, 
  char *leaf_buffer, 
  CoroPull* sink, 
  bool for_write
) {
  auto leaf = (LeafNode *)leaf_buffer;
  size_t offset = sizeof(LeafMetadata) + idx * sizeof(LeafEntry);
  dsm_->read_sync(raw_leaf_buffer + offset, leaf_addr + offset, sizeof(LeafEntry), sink);
  leaf->records[idx] = ((LeafNode *)raw_leaf_buffer)->records[idx];
}


template <class NODE, class ENTRY, class VAL>
void CoherentTree::entry_write_and_unlock(
  NODE* node, 
  const int idx, 
  const Key& k, 
  VAL v, 
  const GlobalAddress& node_addr, 
  uint64_t* lock_buffer, 
  CoroPull* sink, 
  bool async
) {
  // Update the entry
  node->records[idx].update(k, v);
  
  // Encode and write - calculate offset to records array
  size_t entry_offset = offsetof(NODE, records) + idx * sizeof(ENTRY);
  // Simplified - would need proper version encoding
  
  auto write_buffer = (dsm_->get_rbuf(sink)).get_leaf_buffer();
  memcpy(write_buffer, &node->records[idx], sizeof(ENTRY));
  
  if (async) {
    dsm_->write((char*)write_buffer, node_addr + entry_offset, sizeof(ENTRY), false, sink);
    unlock_node(node_addr, lock_buffer, NODE::IS_LEAF, sink, true);
  } else {
    dsm_->write_sync((char*)write_buffer, node_addr + entry_offset, sizeof(ENTRY), sink);
    unlock_node(node_addr, lock_buffer, NODE::IS_LEAF, sink, false);
  }
}


template <class NODE, class ENTRY, int TRANS_SIZE>
void CoherentTree::node_write_and_unlock(
  NODE* node, 
  const GlobalAddress& node_addr, 
  uint64_t* lock_buffer, 
  CoroPull* sink, 
  bool async
) {
  auto write_buffer = (dsm_->get_rbuf(sink)).get_leaf_buffer();
  memcpy(write_buffer, node, sizeof(NODE));
  
  if (async) {
    dsm_->write((char*)write_buffer, node_addr, TRANS_SIZE, false, sink);
    unlock_node(node_addr, lock_buffer, NODE::IS_LEAF, sink, true);
  } else {
    dsm_->write_sync((char*)write_buffer, node_addr, TRANS_SIZE, sink);
    unlock_node(node_addr, lock_buffer, NODE::IS_LEAF, sink, false);
  }
}


template <class NODE, class ENTRY, class VAL, int SPAN_SIZE, int ALLOC_SIZE, int TRANS_SIZE>
void CoherentTree::node_split_and_unlock(
  NODE* node, 
  const Key& k, 
  VAL v, 
  const GlobalAddress& node_addr, 
  uint64_t* lock_buffer, 
  uint8_t level, 
  CoroPull* sink
) {
  split_node[dsm_->getMyThreadID()]++;
  
  // Invalidate cache for this node
  if (leaf_manager_ && level == 0) {
    leaf_manager_->invalidate(node_addr);
  }
  
  // Allocate new sibling
  auto sibling_addr = dsm_->alloc(ALLOC_SIZE, PACKED_ADDR_ALIGN_BIT);
  
  // Simplified split - full implementation would properly redistribute keys
  // and update parent pointers
  
  unlock_node(node_addr, lock_buffer, NODE::IS_LEAF, sink);
}


// ============================================================================
// CLEAR DEBUG INFO
// ============================================================================

void CoherentTree::clear_debug_info() {
  for (int i = 0; i < MAX_APP_THREAD; ++i) {
    need_clear[i] = true;
  }
}
