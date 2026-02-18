#if !defined(_TRIE_CACHE_ENTRY_H_)
#define _TRIE_CACHE_ENTRY_H_

#include "Common.h"
#include "InternalNode.h"
#include "Key.h"

#include <atomic>
#include <vector>
#include <cstdint>

/**
 * TrieCacheEntry - Entry structure for the trie-based internal node cache.
 * 
 * Each entry caches an internal node with its key range and frequency counter.
 * The entry also maintains version information for coherence.
 */
struct TrieCacheEntry {
  Key from;                       // Lower bound of key range (inclusive)
  Key to;                         // Upper bound of key range (inclusive, stored as to-1 of actual range)
  mutable std::atomic<uint64_t> access_count;  // Frequency counter for LFU eviction
  mutable std::atomic<uint64_t> version;       // Version for coherence validation  
  mutable InternalNode *node_ptr;              // Cached internal node pointer
  
  TrieCacheEntry() : from(), to(), access_count(0), version(0), node_ptr(nullptr) {}
  
  TrieCacheEntry(const Key& from_, const Key& to_, InternalNode* ptr_, uint64_t ver = 0) 
    : from(from_), to(to_), access_count(1), version(ver), node_ptr(ptr_) {}
  
  // Copy constructor (needed for tbb containers)
  TrieCacheEntry(const TrieCacheEntry& other) 
    : from(other.from), to(other.to), 
      access_count(other.access_count.load()),
      version(other.version.load()),
      node_ptr(other.node_ptr) {}
  
  TrieCacheEntry& operator=(const TrieCacheEntry& other) {
    from = other.from;
    to = other.to;
    access_count.store(other.access_count.load());
    version.store(other.version.load());
    node_ptr = other.node_ptr;
    return *this;
  }
  
  // Increment access count (thread-safe)
  void touch() const {
    access_count.fetch_add(1, std::memory_order_relaxed);
  }
  
  // Get access count
  uint64_t get_frequency() const {
    return access_count.load(std::memory_order_relaxed);
  }
  
  // Check if entry covers key k
  bool covers(const Key& k) const {
    return k >= from && k <= to;
  }
  
  // Memory footprint of this cached entry
  size_t memory_size() const {
    return sizeof(TrieCacheEntry) + (node_ptr ? sizeof(InternalNode) : 0);
  }
} __attribute__((packed));

static_assert(sizeof(TrieCacheEntry) == 2 * sizeof(Key) + sizeof(std::atomic<uint64_t>) * 2 + sizeof(InternalNode*));


/**
 * TrieNodeType - Types of nodes in the ART-style trie cache
 */
enum class TrieNodeType : uint8_t {
  NODE_4 = 0,    // 4-way node (small)
  NODE_16 = 1,   // 16-way node  
  NODE_48 = 2,   // 48-way node with indirect index
  NODE_256 = 3   // 256-way node (full radix)
};


/**
 * TrieNodeHeader - Common header for all trie node types
 */
class TrieNodeHeader {
public:
  TrieNodeType type;           // Node type
  uint8_t depth;               // Depth in trie (key byte index)
  uint8_t num_children;        // Number of valid children
  uint8_t partial_len;         // Length of compressed prefix
  
  // Compressed prefix for path compression (max 8 bytes)
  uint8_t partial[8];
  
  TrieNodeHeader() : type(TrieNodeType::NODE_4), depth(0), num_children(0), partial_len(0) {
    memset(partial, 0, sizeof(partial));
  }
  
  TrieNodeHeader(TrieNodeType t, uint8_t d, uint8_t plen, const uint8_t* p) 
    : type(t), depth(d), num_children(0), partial_len(plen) {
    memcpy(partial, p, std::min((size_t)plen, sizeof(partial)));
  }
  
  // Check if partial prefix matches key bytes at depth
  bool check_partial(const Key& key, int check_len) const {
    for (int i = 0; i < check_len && i < partial_len; i++) {
      if (partial[i] != key[depth + i]) {
        return false;
      }
    }
    return true;
  }
  
  // Find first mismatch index in partial prefix
  int partial_mismatch(const Key& key) const {
    for (int i = 0; i < partial_len; i++) {
      if (partial[i] != key[depth + i]) {
        return i;
      }
    }
    return partial_len;
  }
};


/**
 * TrieNode4 - 4-way trie node for sparse portions of keyspace
 */
class TrieNode4 {
public:
  TrieNodeHeader header;
  uint8_t keys[4];              // Key bytes for each child
  void* children[4];            // Child pointers (TrieNode* or TrieCacheEntry*)
  TrieCacheEntry* entries[4];   // Direct cache entries at this node
  
  TrieNode4() : header() {
    memset(keys, 0, sizeof(keys));
    memset(children, 0, sizeof(children));
    memset(entries, 0, sizeof(entries));
  }
  
  // Find child index for key byte, returns -1 if not found
  int find_child(uint8_t key_byte) const {
    for (int i = 0; i < header.num_children; i++) {
      if (keys[i] == key_byte) {
        return i;
      }
    }
    return -1;
  }
  
  // Add a child at key_byte position
  bool add_child(uint8_t key_byte, void* child) {
    if (header.num_children >= 4) {
      return false;  // Need to grow to Node16
    }
    keys[header.num_children] = key_byte;
    children[header.num_children] = child;
    header.num_children++;
    return true;
  }
  
  bool is_full() const { return header.num_children >= 4; }
  
  size_t memory_size() const {
    return sizeof(TrieNode4);
  }
};


/**
 * TrieNode16 - 16-way trie node
 */
class TrieNode16 {
public:
  TrieNodeHeader header;
  uint8_t keys[16];
  void* children[16];
  TrieCacheEntry* entries[16];
  
  TrieNode16() : header() {
    header.type = TrieNodeType::NODE_16;
    memset(keys, 0, sizeof(keys));
    memset(children, 0, sizeof(children));
    memset(entries, 0, sizeof(entries));
  }
  
  // Construct from Node4 (grow operation)
  TrieNode16(const TrieNode4& n4) : header(n4.header) {
    header.type = TrieNodeType::NODE_16;
    memset(keys, 0, sizeof(keys));
    memset(children, 0, sizeof(children));
    memset(entries, 0, sizeof(entries));
    for (int i = 0; i < n4.header.num_children; i++) {
      keys[i] = n4.keys[i];
      children[i] = n4.children[i];
      entries[i] = n4.entries[i];
    }
  }
  
  int find_child(uint8_t key_byte) const {
    // Linear search with potential SIMD optimization
    for (int i = 0; i < header.num_children; i++) {
      if (keys[i] == key_byte) {
        return i;
      }
    }
    return -1;
  }
  
  bool add_child(uint8_t key_byte, void* child) {
    if (header.num_children >= 16) {
      return false;  // Need to grow to Node48
    }
    keys[header.num_children] = key_byte;
    children[header.num_children] = child;
    header.num_children++;
    return true;
  }
  
  bool is_full() const { return header.num_children >= 16; }
  
  size_t memory_size() const {
    return sizeof(TrieNode16);
  }
};


/**
 * TrieNode48 - 48-way trie node with indirect index
 */
class TrieNode48 {
public:
  TrieNodeHeader header;
  uint8_t child_index[256];      // Maps key byte to child slot (255 = invalid)
  void* children[48];
  TrieCacheEntry* entries[48];
  
  static constexpr uint8_t EMPTY_SLOT = 255;
  
  TrieNode48() : header() {
    header.type = TrieNodeType::NODE_48;
    memset(child_index, EMPTY_SLOT, sizeof(child_index));
    memset(children, 0, sizeof(children));
    memset(entries, 0, sizeof(entries));
  }
  
  // Construct from Node16 (grow operation)
  TrieNode48(const TrieNode16& n16) : header(n16.header) {
    header.type = TrieNodeType::NODE_48;
    memset(child_index, EMPTY_SLOT, sizeof(child_index));
    memset(children, 0, sizeof(children));
    memset(entries, 0, sizeof(entries));
    for (int i = 0; i < n16.header.num_children; i++) {
      child_index[n16.keys[i]] = i;
      children[i] = n16.children[i];
      entries[i] = n16.entries[i];
    }
  }
  
  int find_child(uint8_t key_byte) const {
    return child_index[key_byte] == EMPTY_SLOT ? -1 : child_index[key_byte];
  }
  
  bool add_child(uint8_t key_byte, void* child) {
    if (header.num_children >= 48) {
      return false;  // Need to grow to Node256
    }
    uint8_t slot = header.num_children;
    child_index[key_byte] = slot;
    children[slot] = child;
    header.num_children++;
    return true;
  }
  
  bool is_full() const { return header.num_children >= 48; }
  
  size_t memory_size() const {
    return sizeof(TrieNode48);
  }
};


/**
 * TrieNode256 - Full 256-way radix node
 */
class TrieNode256 {
public:
  TrieNodeHeader header;
  void* children[256];
  TrieCacheEntry* entries[256];
  
  TrieNode256() : header() {
    header.type = TrieNodeType::NODE_256;
    memset(children, 0, sizeof(children));
    memset(entries, 0, sizeof(entries));
  }
  
  // Construct from Node48 (grow operation)
  TrieNode256(const TrieNode48& n48) : header(n48.header) {
    header.type = TrieNodeType::NODE_256;
    memset(children, 0, sizeof(children));
    memset(entries, 0, sizeof(entries));
    for (int i = 0; i < 256; i++) {
      if (n48.child_index[i] != TrieNode48::EMPTY_SLOT) {
        uint8_t slot = n48.child_index[i];
        children[i] = n48.children[slot];
        entries[i] = n48.entries[slot];
      }
    }
    header.num_children = n48.header.num_children;
  }
  
  int find_child(uint8_t key_byte) const {
    return children[key_byte] ? key_byte : -1;
  }
  
  bool add_child(uint8_t key_byte, void* child) {
    if (children[key_byte] == nullptr) {
      header.num_children++;
    }
    children[key_byte] = child;
    return true;
  }
  
  bool is_full() const { return false; }  // Never full
  
  size_t memory_size() const {
    return sizeof(TrieNode256);
  }
};


/**
 * TrieComparator - Comparator for range-based cache entry lookup
 */
struct TrieCacheEntryComparator {
  typedef TrieCacheEntry DecodedType;
  
  static DecodedType decode_key(const char* b) { 
    return *(TrieCacheEntry*)b; 
  }
  
  // Compare by (to, from) for range containment search
  int cmp(const TrieCacheEntry& a, const TrieCacheEntry& b) const {
    if (a.to < b.to) return -1;
    if (a.to > b.to) return +1;
    if (a.from < b.from) return +1;
    if (a.from > b.from) return -1;
    return 0;
  }
  
  int operator()(const TrieCacheEntry* a, const TrieCacheEntry* b) const {
    return cmp(*a, *b);
  }
};

#endif // _TRIE_CACHE_ENTRY_H_
