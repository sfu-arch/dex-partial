# CHIME with Trie-Based Cache and Compute-Side Bitmap

This is an enhanced version of CHIME that replaces the SkipList-based internal node cache with a **trie-based (ART-style)** cache and moves the **vacancy bitmap from memory node to compute node** with a proper **cache coherence protocol**.

## Key Architectural Changes

### 1. Trie-Based Internal Node Cache (`TrieCache.h`)

**Original CHIME:** Uses `TreeCache` with an inline SkipList for caching internal nodes. Lookup is O(log n).

**New Design:** Uses `TrieCache` with ART-style adaptive radix trie:
- **O(key_length)** lookup instead of O(log n)
- **Node type evolution:** NODE_4 → NODE_16 → NODE_48 → NODE_256
- **Path compression** for memory efficiency
- **Better cache locality** for sequential key access

```
TrieCache Architecture:
                    [Root Node]
                   /    |    \
            [Node4]  [Node16] [Node48]
               |        |
         [Entry]   [Entry]
        (cached internal node)
```

### 2. Compute-Side Bitmap Cache (`ComputeSideBitmap.h`)

**Original CHIME:** Vacancy bitmap is stored in the lock word on the memory node. Every insert requires reading the lock word via RDMA.

**New Design:** Cache vacancy bitmaps at compute nodes:
- **8-way set-associative** cache (16K sets × 8 ways × 64B = 8MB)
- **64-byte cache line alignment** for each entry
- **LFU eviction** with two-random-choice
- Enables **free slot lookup without RDMA**

```
CachedBitmapEntry (64 bytes):
┌─────────────────┬────────────────┬─────────┬──────────────┐
│ leaf_addr (8B)  │ vacancy_bm (8B)│ ver (8B)│ access_cnt(8)│
├─────────────────┴────────────────┴─────────┴──────────────┤
│ fence_low (8B) │ fence_high (8B)│ hop_bm │ max_idx │flags│
└─────────────────┴────────────────┴────────┴─────────┴─────┘
```

### 3. Cache Coherence Protocol (`CacheCoherence.h`)

**Original CHIME:** No coherence needed since bitmaps aren't cached.

**New Design:** MESI-like directory-based coherence:

```
Coherence States:
┌─────────────────────────────────────────────────────────┐
│  INVALID ──→ SHARED ──→ EXCLUSIVE ──→ MODIFIED         │
│     ↑           │           │            │              │
│     └───────────┴───────────┴────────────┘              │
│              (invalidation/downgrade)                   │
└─────────────────────────────────────────────────────────┘
```

**Two coherence modes:**
- **LAZY (default):** Version-based validation on read. Lower overhead, eventual consistency.
- **EAGER:** Directory-based invalidation on write. Immediate consistency, higher message overhead.

## New Files

| File | Purpose |
|------|---------|
| `include/TrieCacheEntry.h` | Trie node types (NODE_4/16/48/256) and cache entry |
| `include/TrieCache.h` | ART-style trie cache replacing SkipList |
| `include/ComputeSideBitmap.h` | Compute-side bitmap cache with set-associative design |
| `include/CacheCoherence.h` | MESI-like coherence protocol |
| `include/CoherentLeafOperations.h` | Integrated leaf operations with caching |
| `include/CoherentTree.h` | Modified tree interface |
| `src/CoherentTree.cpp` | Implementation of coherent tree operations |

## Build Options

```bash
mkdir build && cd build

# Standard CHIME (original behavior)
cmake .. -DUSE_COHERENT_TREE=OFF

# Coherent CHIME with all features
cmake .. -DUSE_COHERENT_TREE=ON \
         -DUSE_TRIE_CACHE=ON \
         -DUSE_BITMAP_CACHE=ON \
         -DUSE_LAZY_COHERENCE=ON

make -j$(nproc)
```

## Performance Characteristics

### When Compute-Side Bitmap Helps

| Scenario | Benefit |
|----------|---------|
| **High insert rate** | Skip RDMA read for vacancy lookup |
| **Skewed workload** | Hot leaves stay cached |
| **Multiple compute nodes** | Coherence amortizes cost |

### When It Doesn't Help

| Scenario | Why |
|----------|-----|
| **Uniform random** | Each leaf accessed once, cache misses |
| **Range queries** | Bitmap doesn't help sequential scan |
| **Very large dataset** | Cache too small to cover working set |

## Memory Usage

```
Component                   Size
─────────────────────────────────────────
Trie Cache (internal nodes) ~220MB for 10M keys
Bitmap Cache                8MB (configurable)
Coherence Directory         ~5MB on memory node
─────────────────────────────────────────
Total Overhead              ~233MB
```

## Key Algorithms

### Free Slot Lookup (Cached)

```cpp
int find_free_slot(leaf_addr, hash_idx) {
  // 1. Check local bitmap cache
  auto* cached = bitmap_cache.lookup(leaf_addr);
  if (cached && cached->is_valid()) {
    // Find first free slot starting from hash_idx
    uint64_t mask = ~cached->vacancy_bitmap;
    uint64_t rotated = rotate_left(mask, hash_idx);
    if (rotated != 0) {
      return (hash_idx + ctz(rotated)) % LEAF_SIZE;
    }
    return -1;  // No free slot
  }
  
  // 2. Cache miss - fetch from memory
  return -2;  // Signal to do RDMA read
}
```

### Trie Cache Lookup

```cpp
TrieCacheEntry* trie_search(Key key) {
  void* node = root;
  int depth = 0;
  TrieCacheEntry* best = nullptr;
  
  while (node && depth < KEY_LEN) {
    // Check entries at this node
    for (auto& entry : node->entries) {
      if (entry && entry->covers(key)) {
        best = entry;  // Found better match
      }
    }
    
    // Navigate to child
    uint8_t byte = key[depth];
    node = node->children[byte];
    depth++;
  }
  
  return best;
}
```

### Coherence Invalidation (Eager Mode)

```cpp
void handle_write_request(leaf_addr, requestor) {
  auto* dir = directory.get(leaf_addr);
  
  // Collect sharers to invalidate
  for (int node : dir->get_sharers()) {
    if (node != requestor) {
      send_invalidate(node, leaf_addr);
    }
  }
  
  // Grant exclusive access
  dir->set_owner(requestor);
}
```

## Integration with Original CHIME

The new components are designed to be **drop-in compatible**:

1. **Original Tree.h** still works - use `-DUSE_COHERENT_TREE=OFF`
2. **CoherentTree.h** has the same API
3. Tests work with either version

```cpp
#ifdef USE_COHERENT_TREE
  #include "CoherentTree.h"
  using Tree = CoherentTree;
#else
  #include "Tree.h"
#endif
```

## Limitations

1. **Coherence overhead:** Each write requires coordination with memory node
2. **Memory overhead:** ~233MB additional for caching infrastructure
3. **Complexity:** More code paths, harder to debug
4. **Range queries:** Still requires sequential leaf traversal

## Future Work

- [ ] Prefetching for range queries based on trie structure
- [ ] Batched invalidation for bulk operations
- [ ] RDMA-based eager invalidation (vs. current RPC)
- [ ] Adaptive cache sizing based on workload

## References

- **ART (Adaptive Radix Tree):** Leis et al., ICDE 2013
- **MESI Protocol:** Papamarcos & Patel, ISCA 1984
- **CHIME:** Original paper
