#include "CoherentTree.h"
#include "TrieCache.h"
#include "ComputeSideBitmap.h"
#include "CacheCoherence.h"
#include "CoherentLeafOperations.h"

#include <iostream>
#include <cassert>
#include <vector>
#include <random>
#include <chrono>

/**
 * test_coherent_tree.cpp - Test suite for CoherentTree with trie cache and bitmap.
 * 
 * This tests:
 * 1. TrieCache basic operations (insert, search, range)
 * 2. ComputeSideBitmap operations (cache, lookup, invalidate)
 * 3. CacheCoherence operations (state transitions)
 * 4. Integrated CoherentLeafOperations
 * 
 * Build with: -DUSE_COHERENT_TREE=ON
 */

using namespace std;
using namespace std::chrono;

// ============================================================================
// TEST UTILITIES
// ============================================================================

Key make_key(uint64_t val) {
  Key k;
  for (int i = sizeof(Key) - 1; i >= 0; i--) {
    k[i] = val & 0xFF;
    val >>= 8;
  }
  return k;
}

uint64_t key_to_int(const Key& k) {
  uint64_t val = 0;
  for (size_t i = 0; i < sizeof(Key); i++) {
    val = (val << 8) | k[i];
  }
  return val;
}

void print_key(const Key& k) {
  cout << "Key[";
  for (size_t i = 0; i < sizeof(Key); i++) {
    printf("%02x", k[i]);
  }
  cout << "]";
}

#define TEST_ASSERT(cond, msg) do { \
  if (!(cond)) { \
    cerr << "FAIL: " << msg << " at " << __FILE__ << ":" << __LINE__ << endl; \
    return false; \
  } \
} while(0)

#define TEST_PASS(name) do { \
  cout << "PASS: " << name << endl; \
  return true; \
} while(0)

// ============================================================================
// TRIE CACHE TESTS
// ============================================================================

bool test_trie_cache_entry_coverage() {
  TrieCacheEntry entry;
  entry.from = make_key(100);
  entry.to = make_key(200);
  entry.node_ptr = nullptr;
  
  TEST_ASSERT(entry.covers(make_key(100)), "Should cover lower bound");
  TEST_ASSERT(entry.covers(make_key(150)), "Should cover middle");
  TEST_ASSERT(entry.covers(make_key(200)), "Should cover upper bound");
  TEST_ASSERT(!entry.covers(make_key(99)), "Should not cover below");
  TEST_ASSERT(!entry.covers(make_key(201)), "Should not cover above");
  
  TEST_PASS("TrieCacheEntry coverage");
}

bool test_trie_node_types() {
  // Test Node4
  TrieNode4 n4;
  TEST_ASSERT(n4.header.type == TrieNodeType::NODE_4, "Node4 type");
  TEST_ASSERT(n4.add_child(0x10, (void*)0x1000), "Node4 add child 1");
  TEST_ASSERT(n4.add_child(0x20, (void*)0x2000), "Node4 add child 2");
  TEST_ASSERT(n4.add_child(0x30, (void*)0x3000), "Node4 add child 3");
  TEST_ASSERT(n4.add_child(0x40, (void*)0x4000), "Node4 add child 4");
  TEST_ASSERT(!n4.add_child(0x50, (void*)0x5000), "Node4 should be full");
  TEST_ASSERT(n4.find_child(0x20) == 1, "Node4 find child");
  TEST_ASSERT(n4.find_child(0xFF) == -1, "Node4 find missing");
  
  // Test Node16 grow from Node4
  TrieNode16 n16(n4);
  TEST_ASSERT(n16.header.type == TrieNodeType::NODE_16, "Node16 type");
  TEST_ASSERT(n16.header.num_children == 4, "Node16 inherited children");
  TEST_ASSERT(n16.find_child(0x20) >= 0, "Node16 find inherited");
  TEST_ASSERT(n16.add_child(0x50, (void*)0x5000), "Node16 add new");
  
  // Test Node48 grow from Node16
  // Fill Node16 first
  for (int i = 5; i <= 16; i++) {
    n16.add_child(0x50 + i, (void*)(uintptr_t)(0x5000 + i));
  }
  
  TrieNode48 n48(n16);
  TEST_ASSERT(n48.header.type == TrieNodeType::NODE_48, "Node48 type");
  TEST_ASSERT(n48.find_child(0x10) >= 0, "Node48 find inherited");
  
  // Test Node256 grow from Node48
  TrieNode256 n256(n48);
  TEST_ASSERT(n256.header.type == TrieNodeType::NODE_256, "Node256 type");
  TEST_ASSERT(!n256.is_full(), "Node256 never full");
  
  TEST_PASS("Trie node type evolution");
}

// ============================================================================
// BITMAP CACHE TESTS
// ============================================================================

bool test_cached_bitmap_entry() {
  CachedBitmapEntry entry;
  entry.leaf_addr = GlobalAddress{1, 0x1000};
  entry.vacancy_bitmap = 0b1010101010101010;  // Alternating occupied/free
  entry.flags = CachedBitmapEntry::FLAG_VALID;
  
  TEST_ASSERT(entry.is_valid(), "Entry should be valid");
  TEST_ASSERT(!entry.is_dirty(), "Entry should not be dirty");
  
  // Test free slot finding
  int first_free = entry.find_free_slot(0);
  TEST_ASSERT(first_free == 0, "First free should be 0");
  
  // Mark slot 0 as occupied
  entry.set_occupied(0);
  TEST_ASSERT(entry.vacancy_bitmap & (1ULL << 0), "Slot 0 should be occupied");
  TEST_ASSERT(entry.is_dirty(), "Entry should be dirty after modification");
  
  // Find next free
  int next_free = entry.find_free_slot(0);
  TEST_ASSERT(next_free == 2, "Next free should be 2");
  
  // Test free slot counting
  entry.vacancy_bitmap = 0xFFFFFFFFFFFFFFFF;  // All occupied
  TEST_ASSERT(entry.count_free_slots() == 0, "No free slots");
  
  entry.vacancy_bitmap = 0;  // All free
  TEST_ASSERT(entry.count_free_slots() == define::leafSpanSize, "All slots free");
  
  TEST_PASS("CachedBitmapEntry operations");
}

bool test_bitmap_cache_set() {
  BitmapCacheSet set;
  
  // Insert entries
  CachedBitmapEntry e1(GlobalAddress{1, 0x1000}, 0xFF, 1);
  CachedBitmapEntry evicted;
  
  auto* cached1 = set.insert(e1, &evicted);
  TEST_ASSERT(cached1 != nullptr, "Insert should succeed");
  TEST_ASSERT(cached1->leaf_addr == e1.leaf_addr, "Address should match");
  
  // Find entry
  auto* found = set.find(GlobalAddress{1, 0x1000});
  TEST_ASSERT(found != nullptr, "Should find inserted entry");
  TEST_ASSERT(found->vacancy_bitmap == 0xFF, "Bitmap should match");
  
  // Invalidate
  TEST_ASSERT(set.invalidate(GlobalAddress{1, 0x1000}), "Invalidate should succeed");
  TEST_ASSERT(set.find(GlobalAddress{1, 0x1000}) == nullptr, "Should not find after invalidate");
  
  TEST_PASS("BitmapCacheSet operations");
}

bool test_compute_side_bitmap_cache() {
  ComputeSideBitmapCache cache;
  
  // Cache a bitmap
  auto* entry = cache.cache_bitmap(
    GlobalAddress{1, 0x2000},
    0b11110000,  // Lower 4 slots free, upper 4 occupied
    1,
    make_key(0),
    make_key(1000)
  );
  
  TEST_ASSERT(entry != nullptr, "Cache should succeed");
  
  // Lookup
  auto* found = cache.lookup(GlobalAddress{1, 0x2000});
  TEST_ASSERT(found != nullptr, "Lookup should succeed");
  TEST_ASSERT(found->vacancy_bitmap == 0b11110000, "Bitmap should match");
  
  // Find free slot
  int free_slot = cache.find_free_slot_cached(GlobalAddress{1, 0x2000}, 0);
  TEST_ASSERT(free_slot == 0, "First free slot should be 0");
  
  // Update bitmap
  TEST_ASSERT(cache.update_bitmap(GlobalAddress{1, 0x2000}, 0, true), "Update should succeed");
  found = cache.lookup(GlobalAddress{1, 0x2000});
  TEST_ASSERT(found->vacancy_bitmap & 1, "Slot 0 should now be occupied");
  
  // Invalidate
  TEST_ASSERT(cache.invalidate(GlobalAddress{1, 0x2000}), "Invalidate should succeed");
  TEST_ASSERT(cache.lookup(GlobalAddress{1, 0x2000}) == nullptr, "Should not find after invalidate");
  
  cache.print_stats();
  
  TEST_PASS("ComputeSideBitmapCache operations");
}

// ============================================================================
// COHERENCE TESTS
// ============================================================================

bool test_coherence_states() {
  using namespace coherence;
  
  TEST_ASSERT(state_to_string(CoherenceState::INVALID) != nullptr, "State names");
  TEST_ASSERT(state_to_string(CoherenceState::SHARED) != nullptr, "State names");
  TEST_ASSERT(state_to_string(CoherenceState::EXCLUSIVE) != nullptr, "State names");
  TEST_ASSERT(state_to_string(CoherenceState::MODIFIED) != nullptr, "State names");
  
  TEST_PASS("Coherence state definitions");
}

bool test_directory_entry() {
  using namespace coherence;
  
  DirectoryEntry entry;
  
  // Initial state
  TEST_ASSERT(!entry.has_sharers(), "Initially no sharers");
  TEST_ASSERT(entry.owner_node.load() == -1, "Initially no owner");
  
  // Add sharers
  entry.add_sharer(0);
  entry.add_sharer(3);
  TEST_ASSERT(entry.is_sharer(0), "Node 0 is sharer");
  TEST_ASSERT(entry.is_sharer(3), "Node 3 is sharer");
  TEST_ASSERT(!entry.is_sharer(1), "Node 1 is not sharer");
  
  // Set exclusive owner
  entry.set_owner(5);
  TEST_ASSERT(entry.owner_node.load() == 5, "Owner should be 5");
  TEST_ASSERT(entry.sharer_mask.load() == 0, "Sharers cleared on exclusive");
  
  // Clear owner (demote to sharer)
  entry.clear_owner();
  TEST_ASSERT(entry.owner_node.load() == -1, "Owner should be cleared");
  TEST_ASSERT(entry.is_sharer(5), "Former owner should be sharer");
  
  // Locking
  TEST_ASSERT(entry.try_lock(), "Lock should succeed");
  TEST_ASSERT(!entry.try_lock(), "Second lock should fail");
  entry.unlock();
  TEST_ASSERT(entry.try_lock(), "Lock after unlock should succeed");
  
  TEST_PASS("DirectoryEntry operations");
}

bool test_coherence_directory() {
  using namespace coherence;
  
  CoherenceDirectory directory;
  GlobalAddress leaf1{1, 0x1000};
  GlobalAddress leaf2{1, 0x2000};
  
  // Read request - should grant EXCLUSIVE when no other sharers
  auto [state1, ver1] = directory.handle_read_request(leaf1, 0);
  TEST_ASSERT(state1 == CoherenceState::EXCLUSIVE, "First read gets EXCLUSIVE");
  
  // Second read from different node - should get SHARED
  auto [state2, ver2] = directory.handle_read_request(leaf1, 1);
  TEST_ASSERT(state2 == CoherenceState::SHARED, "Second read gets SHARED");
  
  // Write request - should return nodes to invalidate
  auto [state3, inv_list] = directory.handle_write_request(leaf1, 2);
  TEST_ASSERT(state3 == CoherenceState::EXCLUSIVE, "Write gets EXCLUSIVE");
  TEST_ASSERT(inv_list.size() >= 1, "Should have nodes to invalidate");
  
  // Handle writeback
  directory.handle_writeback(leaf2, 0, 100);
  
  // Handle eviction
  directory.handle_eviction(leaf1, 2);
  
  TEST_PASS("CoherenceDirectory operations");
}

// ============================================================================
// INTEGRATED TESTS
// ============================================================================

bool test_coherent_leaf_helper() {
  // This test requires DSM which isn't available in unit tests
  // Just verify the helper classes compile correctly
  
  cout << "Note: Full integrated tests require DSM connection" << endl;
  
  TEST_PASS("CoherentLeafOperations compilation");
}

// ============================================================================
// PERFORMANCE TESTS
// ============================================================================

bool test_trie_node_performance() {
  const int NUM_OPS = 100000;
  
  // Test Node256 lookup performance (should be constant time)
  TrieNode256 n256;
  for (int i = 0; i < 256; i++) {
    n256.add_child(i, (void*)(uintptr_t)(i + 1));
  }
  
  auto start = high_resolution_clock::now();
  volatile int found = 0;
  for (int i = 0; i < NUM_OPS; i++) {
    int slot = n256.find_child(i % 256);
    if (slot >= 0) found++;
  }
  auto end = high_resolution_clock::now();
  
  auto duration_ns = duration_cast<nanoseconds>(end - start).count();
  double ns_per_op = (double)duration_ns / NUM_OPS;
  
  cout << "  Node256 lookup: " << ns_per_op << " ns/op" << endl;
  TEST_ASSERT(ns_per_op < 100, "Node256 lookup should be fast");
  
  TEST_PASS("Trie node performance");
}

bool test_bitmap_cache_performance() {
  const int NUM_OPS = 100000;
  const int NUM_ENTRIES = 1000;
  
  ComputeSideBitmapCache cache;
  
  // Populate cache
  for (int i = 0; i < NUM_ENTRIES; i++) {
    cache.cache_bitmap(
      GlobalAddress{0, (uint64_t)(i * 1024)},
      (uint64_t)i,
      1,
      make_key(i * 100),
      make_key((i + 1) * 100)
    );
  }
  
  // Benchmark lookups
  auto start = high_resolution_clock::now();
  volatile int hits = 0;
  for (int i = 0; i < NUM_OPS; i++) {
    auto* entry = cache.lookup(GlobalAddress{0, (uint64_t)((i % NUM_ENTRIES) * 1024)});
    if (entry) hits++;
  }
  auto end = high_resolution_clock::now();
  
  auto duration_ns = duration_cast<nanoseconds>(end - start).count();
  double ns_per_op = (double)duration_ns / NUM_OPS;
  
  cout << "  Bitmap cache lookup: " << ns_per_op << " ns/op (hit rate: " 
       << (100.0 * hits / NUM_OPS) << "%)" << endl;
  TEST_ASSERT(ns_per_op < 500, "Bitmap lookup should be fast");
  
  TEST_PASS("Bitmap cache performance");
}

// ============================================================================
// MAIN
// ============================================================================

int main(int argc, char* argv[]) {
  cout << "========================================" << endl;
  cout << "CoherentTree Test Suite" << endl;
  cout << "========================================" << endl;
  
  int passed = 0;
  int failed = 0;
  
  auto run_test = [&](bool (*test)(), const char* name) {
    cout << "\nRunning: " << name << endl;
    if (test()) {
      passed++;
    } else {
      failed++;
    }
  };
  
  // Trie cache tests
  run_test(test_trie_cache_entry_coverage, "TrieCacheEntry coverage");
  run_test(test_trie_node_types, "Trie node types");
  
  // Bitmap cache tests
  run_test(test_cached_bitmap_entry, "CachedBitmapEntry");
  run_test(test_bitmap_cache_set, "BitmapCacheSet");
  run_test(test_compute_side_bitmap_cache, "ComputeSideBitmapCache");
  
  // Coherence tests
  run_test(test_coherence_states, "Coherence states");
  run_test(test_directory_entry, "DirectoryEntry");
  run_test(test_coherence_directory, "CoherenceDirectory");
  
  // Integrated tests
  run_test(test_coherent_leaf_helper, "CoherentLeafOperations");
  
  // Performance tests
  run_test(test_trie_node_performance, "Trie node performance");
  run_test(test_bitmap_cache_performance, "Bitmap cache performance");
  
  cout << "\n========================================" << endl;
  cout << "Results: " << passed << " passed, " << failed << " failed" << endl;
  cout << "========================================" << endl;
  
  return failed > 0 ? 1 : 0;
}
