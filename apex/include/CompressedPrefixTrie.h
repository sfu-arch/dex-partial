#ifndef __APEX_CPT_H__
#define __APEX_CPT_H__

#include "Common.h"
#include "GlobalAddress.h"
#include "LeafPage.h"

#include <cstring>
#include <vector>
#include <mutex>
#include <memory>
#include <algorithm>

namespace apex {

// ─── Compressed Prefix Trie (CPT) ──────────────────────────────────
//
// Maps each key's prefix → the GlobalAddress of a remote leaf page.
// Fully local on the compute node. Never requires RDMA.
//
// Path compression: chains of single-child nodes are collapsed.
// Example: keys "apple", "application" share prefix "appl"
//          → one compressed edge "appl" rather than 4 nodes.
//
// For uint64_t keys, we decompose the key into bytes (big-endian)
// and build the trie over those bytes. With path compression and
// fanout-256 at each level, the trie is very compact.
//

// ─── Trie Node ─────────────────────────────────────────────────────
struct TrieNode {
  // Compressed path: bytes shared by all keys under this node
  uint8_t  path[8];          // max 8 bytes of compressed path
  uint8_t  path_len;         // how many bytes of path[] are valid

  // Children: indexed by the next byte of the key
  // nullptr means no child at that byte value
  TrieNode* children[256];

  // If this is a leaf trie node (maps to a remote leaf page)
  bool            is_leaf;
  GlobalAddress   leaf_addr;    // remote address of the leaf page
  uint32_t        leaf_id;      // unique leaf page ID

  // Fence keys for this leaf's range
  Key             min_key;      // minimum key in this leaf
  Key             max_key;      // maximum key in this leaf

  // Sibling pointers for range scan (linked list of leaves in key order)
  TrieNode*       next_leaf;    // next leaf in sorted order
  TrieNode*       prev_leaf;    // previous leaf in sorted order

  TrieNode() {
    memset(path, 0, sizeof(path));
    path_len = 0;
    memset(children, 0, sizeof(children));
    is_leaf = false;
    leaf_addr = GlobalAddress::Null();
    leaf_id = 0;
    min_key = 0;
    max_key = 0;
    next_leaf = nullptr;
    prev_leaf = nullptr;
  }

  ~TrieNode() {
    for (int i = 0; i < 256; i++) {
      delete children[i];
      children[i] = nullptr;
    }
  }

  int child_count() const {
    int cnt = 0;
    for (int i = 0; i < 256; i++) {
      if (children[i]) cnt++;
    }
    return cnt;
  }
};


// ─── Key Decomposition ─────────────────────────────────────────────
// Convert a uint64_t key into its big-endian byte representation.
// Byte 0 is the most significant byte.
inline void key_to_bytes(Key key, uint8_t bytes[8]) {
  for (int i = 7; i >= 0; i--) {
    bytes[i] = key & 0xFF;
    key >>= 8;
  }
}

inline Key bytes_to_key(const uint8_t bytes[8]) {
  Key key = 0;
  for (int i = 0; i < 8; i++) {
    key = (key << 8) | bytes[i];
  }
  return key;
}

// Extract the suffix from a key given the prefix length (in bytes)
inline uint32_t extract_suffix(Key key, int prefix_bytes) {
  // Suffix = lower (8 - prefix_bytes) bytes, truncated to 4 bytes
  int suffix_bytes = 8 - prefix_bytes;
  if (suffix_bytes <= 0) return 0;
  if (suffix_bytes > 4) suffix_bytes = 4;

  uint32_t suffix = 0;
  for (int i = 0; i < suffix_bytes; i++) {
    int byte_idx = prefix_bytes + i;
    uint8_t b = (key >> (8 * (7 - byte_idx))) & 0xFF;
    suffix = (suffix << 8) | b;
  }
  return suffix;
}


// ─── Compressed Prefix Trie Class ──────────────────────────────────
class CompressedPrefixTrie {
public:
  CompressedPrefixTrie() : root_(new TrieNode()), num_leaves_(0),
                           leaf_list_head_(nullptr), leaf_list_tail_(nullptr) {}

  ~CompressedPrefixTrie() {
    delete root_;
  }

  // ─── Lookup: key → leaf page address ─────────────────────────
  // Returns the GlobalAddress of the leaf page containing this key.
  // Returns GlobalAddress::Null() if no matching prefix exists.
  // Also sets prefix_depth to the depth at which the leaf was found.
  GlobalAddress lookup(Key key, int &prefix_depth) const {
    uint8_t key_bytes[8];
    key_to_bytes(key, key_bytes);

    const TrieNode* node = root_;
    int depth = 0;

    while (node && depth < 8) {
      // Check compressed path
      for (int i = 0; i < node->path_len; i++) {
        if (depth >= 8) break;
        if (key_bytes[depth] != node->path[i]) {
          // Mismatch in compressed path → key not found
          prefix_depth = depth;
          return GlobalAddress::Null();
        }
        depth++;
      }

      // If this is a leaf node, we found the matching leaf page
      if (node->is_leaf) {
        prefix_depth = depth;
        return node->leaf_addr;
      }

      // Follow child edge
      if (depth < 8) {
        uint8_t next_byte = key_bytes[depth];
        node = node->children[next_byte];
        depth++;
      }
    }

    // If we consumed all 8 bytes and landed on a leaf
    if (node && node->is_leaf) {
      prefix_depth = depth;
      return node->leaf_addr;
    }

    prefix_depth = depth;
    return GlobalAddress::Null();
  }

  // Simplified lookup (no prefix_depth output)
  GlobalAddress lookup(Key key) const {
    int depth;
    return lookup(key, depth);
  }

  // ─── Lower bound: find the first leaf page >= key ────────────
  // Used for range scans.
  TrieNode* lower_bound(Key key) const {
    uint8_t key_bytes[8];
    key_to_bytes(key, key_bytes);

    // Walk the trie to find the closest leaf
    const TrieNode* node = root_;
    int depth = 0;
    TrieNode* last_leaf = nullptr;

    // Simple approach: find exact match or the next leaf in sorted order
    while (node && depth < 8) {
      for (int i = 0; i < node->path_len && depth < 8; i++, depth++) {
        if (key_bytes[depth] != node->path[i]) {
          // Mismatch — find the first leaf in this subtree if key < path,
          // or the next sibling leaf if key > path
          if (key_bytes[depth] < node->path[i]) {
            return find_leftmost_leaf(node);
          } else {
            return nullptr;  // Caller should advance to next sibling
          }
        }
      }

      if (node->is_leaf) {
        return const_cast<TrieNode*>(node);
      }

      if (depth < 8) {
        uint8_t next_byte = key_bytes[depth];
        // Look for child at next_byte or the first child after it
        for (int c = next_byte; c < 256; c++) {
          if (node->children[c]) {
            if (c == next_byte) {
              node = node->children[c];
              depth++;
              break;
            } else {
              // First child > next_byte — return its leftmost leaf
              return find_leftmost_leaf(node->children[c]);
            }
          }
          if (c == 255) return nullptr;  // No more children
        }
      }
    }

    if (node && node->is_leaf) {
      return const_cast<TrieNode*>(node);
    }
    return nullptr;
  }

  // ─── Insert a leaf page into the trie ────────────────────────
  // Maps key range [min_key, max_key] → leaf_addr with given leaf_id.
  void insert_leaf(Key representative_key, GlobalAddress leaf_addr,
                   uint32_t leaf_id, Key min_key, Key max_key) {
    uint8_t key_bytes[8];
    key_to_bytes(representative_key, key_bytes);

    TrieNode* node = root_;
    int depth = 0;

    // Navigate to the insertion point
    while (depth < 8) {
      // Try to match compressed path
      if (node->path_len > 0) {
        int match_len = 0;
        for (int i = 0; i < node->path_len && depth + i < 8; i++) {
          if (key_bytes[depth + i] == node->path[i]) {
            match_len++;
          } else {
            // Split the compressed path
            split_node(node, match_len, depth);
            break;
          }
        }
        depth += match_len;
      }

      if (depth >= 8 || node->is_leaf) break;

      uint8_t next_byte = key_bytes[depth];
      if (!node->children[next_byte]) {
        // Create new leaf node
        TrieNode* leaf = new TrieNode();
        leaf->is_leaf = true;
        leaf->leaf_addr = leaf_addr;
        leaf->leaf_id = leaf_id;
        leaf->min_key = min_key;
        leaf->max_key = max_key;

        // Compress remaining bytes into the path
        int remaining = 8 - depth - 1;
        if (remaining > 0) {
          memcpy(leaf->path, key_bytes + depth + 1, remaining);
          leaf->path_len = remaining;
        }

        node->children[next_byte] = leaf;
        insert_into_leaf_list(leaf);
        num_leaves_++;
        return;
      }

      node = node->children[next_byte];
      depth++;
    }

    // If we reached a non-leaf node at depth 8, make it a leaf
    if (!node->is_leaf) {
      node->is_leaf = true;
      node->leaf_addr = leaf_addr;
      node->leaf_id = leaf_id;
      node->min_key = min_key;
      node->max_key = max_key;
      insert_into_leaf_list(node);
      num_leaves_++;
    }
  }

  // ─── Handle a leaf split ─────────────────────────────────────
  // When a remote leaf page splits, update the trie:
  //   old_leaf now covers [old_min, split_key)
  //   new_leaf covers [split_key, old_max]
  void handle_split(uint32_t leaf_id, Key split_key,
                    GlobalAddress new_leaf_addr, uint32_t new_leaf_id) {
    TrieNode* old_node = find_leaf_by_id(leaf_id);
    if (!old_node) return;

    Key old_max = old_node->max_key;
    old_node->max_key = split_key - 1;

    // Insert the new leaf
    insert_leaf(split_key, new_leaf_addr, new_leaf_id, split_key, old_max);
  }

  // ─── Accessors ───────────────────────────────────────────────
  uint32_t num_leaves() const { return num_leaves_; }
  TrieNode* leaf_list_head() const { return leaf_list_head_; }

  // Find leaf node by ID (used for split handling)
  TrieNode* find_leaf_by_id(uint32_t leaf_id) const {
    TrieNode* cur = leaf_list_head_;
    while (cur) {
      if (cur->leaf_id == leaf_id) return cur;
      cur = cur->next_leaf;
    }
    return nullptr;
  }

private:
  TrieNode* root_;
  uint32_t  num_leaves_;
  TrieNode* leaf_list_head_;  // sorted linked list of leaves
  TrieNode* leaf_list_tail_;

  // Find the leftmost (minimum key) leaf in a subtree
  TrieNode* find_leftmost_leaf(const TrieNode* node) const {
    if (!node) return nullptr;
    if (node->is_leaf) return const_cast<TrieNode*>(node);
    for (int i = 0; i < 256; i++) {
      if (node->children[i]) {
        TrieNode* result = find_leftmost_leaf(node->children[i]);
        if (result) return result;
      }
    }
    return nullptr;
  }

  // Split a compressed path at position split_pos
  void split_node(TrieNode* node, int split_pos, int depth) {
    if (split_pos <= 0 || split_pos >= node->path_len) return;

    // Create a new intermediate node
    TrieNode* intermediate = new TrieNode();

    // The intermediate gets the second half of the path
    memcpy(intermediate->path, node->path + split_pos, node->path_len - split_pos);
    intermediate->path_len = node->path_len - split_pos;

    // Copy children and leaf status to intermediate
    memcpy(intermediate->children, node->children, sizeof(node->children));
    intermediate->is_leaf = node->is_leaf;
    intermediate->leaf_addr = node->leaf_addr;
    intermediate->leaf_id = node->leaf_id;
    intermediate->min_key = node->min_key;
    intermediate->max_key = node->max_key;
    intermediate->next_leaf = node->next_leaf;
    intermediate->prev_leaf = node->prev_leaf;

    // The original node keeps only the first split_pos bytes of path
    node->path_len = split_pos;
    memset(node->children, 0, sizeof(node->children));
    node->is_leaf = false;
    node->leaf_addr = GlobalAddress::Null();

    // Hook intermediate as child of the split byte
    uint8_t split_byte = intermediate->path[0];
    // Shift intermediate path by 1
    memmove(intermediate->path, intermediate->path + 1, intermediate->path_len - 1);
    intermediate->path_len--;
    node->children[split_byte] = intermediate;

    // Fix leaf list pointers
    if (intermediate->is_leaf) {
      if (intermediate->prev_leaf) intermediate->prev_leaf->next_leaf = intermediate;
      if (intermediate->next_leaf) intermediate->next_leaf->prev_leaf = intermediate;
      if (leaf_list_head_ == node) leaf_list_head_ = intermediate;
      if (leaf_list_tail_ == node) leaf_list_tail_ = intermediate;
    }
  }

  // Insert a leaf node into the sorted linked list
  void insert_into_leaf_list(TrieNode* leaf) {
    if (!leaf_list_head_) {
      leaf_list_head_ = leaf;
      leaf_list_tail_ = leaf;
      leaf->prev_leaf = nullptr;
      leaf->next_leaf = nullptr;
      return;
    }

    // Find insertion point
    TrieNode* cur = leaf_list_head_;
    while (cur && cur->min_key < leaf->min_key) {
      cur = cur->next_leaf;
    }

    if (!cur) {
      // Insert at end
      leaf->prev_leaf = leaf_list_tail_;
      leaf->next_leaf = nullptr;
      leaf_list_tail_->next_leaf = leaf;
      leaf_list_tail_ = leaf;
    } else if (cur == leaf_list_head_) {
      // Insert at beginning
      leaf->prev_leaf = nullptr;
      leaf->next_leaf = leaf_list_head_;
      leaf_list_head_->prev_leaf = leaf;
      leaf_list_head_ = leaf;
    } else {
      // Insert before cur
      leaf->prev_leaf = cur->prev_leaf;
      leaf->next_leaf = cur;
      cur->prev_leaf->next_leaf = leaf;
      cur->prev_leaf = leaf;
    }
  }
};

}  // namespace apex

#endif /* __APEX_CPT_H__ */
