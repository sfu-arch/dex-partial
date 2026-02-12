#ifndef __APEX_LEAF_PAGE_H__
#define __APEX_LEAF_PAGE_H__

#include "Common.h"
#include "GlobalAddress.h"
#include <cstring>
#include <algorithm>

namespace apex {

// ─── Leaf Entry: what's stored in each slot of a remote leaf page ───
// Physical entries NEVER move once written (slot-array indirection).
struct __attribute__((packed)) LeafEntry {
  uint32_t suffix;       // 4 bytes: key suffix (prefix stripped by trie)
  Value    value;        // 8 bytes: the payload
  uint16_t version;      // 2 bytes: per-entry version for staleness detection
  // Total: 14 bytes

  bool is_empty() const { return suffix == 0 && value == 0 && version == 0; }
  bool is_tombstone() const { return version == 0xFFFF; }
};
static_assert(sizeof(LeafEntry) == 14, "LeafEntry must be 14 bytes");


// ─── Leaf Page Header ──────────────────────────────────────────────
// Stored at the beginning of each remote 4KB page.
struct __attribute__((packed)) LeafHeader {
  uint64_t page_version;       // 8B: incremented on every structural change
  uint16_t num_entries;        // 2B: current number of live entries
  uint16_t num_tombstones;     // 2B: number of tombstoned entries
  uint32_t page_id;            // 4B: unique page identifier

  // Split information (for forwarding stale readers)
  uint8_t  split_flag;         // 1B: 0=no split, 1=split occurred
  uint8_t  _pad1;              // 1B: alignment
  uint32_t split_suffix;       // 4B: split key suffix (keys > this moved to new page)
  uint64_t split_new_page;     // 8B: GlobalAddress of the new sibling page

  // Total: 30 bytes, padded to 32
  uint16_t _pad2;
};
static_assert(sizeof(LeafHeader) == 32, "LeafHeader must be 32 bytes");


// ─── Slot Array ────────────────────────────────────────────────────
// Maps logical sorted position → physical position within the entry array.
// This indirection means physical entries NEVER move on insert/delete.
// Only the slot array (256 bytes, cheap to re-read) changes order.
struct SlotArray {
  uint8_t slots[define::kMaxEntriesPerLeaf];  // 256 bytes

  // slot[i] = physical position of the i-th key in sorted order
  // 0xFF = unused slot
  void init() { memset(slots, 0xFF, sizeof(slots)); }

  uint8_t get_physical_pos(uint8_t logical_pos) const {
    return slots[logical_pos];
  }
};
static_assert(sizeof(SlotArray) == 256, "SlotArray must be 256 bytes");


// ─── Full Leaf Page Layout (4 KB) ──────────────────────────────────
//
//   Bytes 0-31:      LeafHeader (32 bytes)
//   Bytes 32-287:    SlotArray (256 bytes)
//   Bytes 288-3871:  LeafEntry[256] (14 × 256 = 3584 bytes)
//   Bytes 3872-3935: Bloom filter (64 bytes)
//   Bytes 3936-4095: Fence keys + padding (160 bytes)
//
struct __attribute__((packed)) LeafPage {
  LeafHeader header;                                     // 32 bytes
  SlotArray  slot_array;                                 // 256 bytes
  LeafEntry  entries[define::kMaxEntriesPerLeaf];        // 3584 bytes
  uint8_t    bloom_filter[64];                           // 64 bytes
  uint32_t   fence_min_suffix;                           // 4 bytes
  uint32_t   fence_max_suffix;                           // 4 bytes
  uint8_t    _padding[152];                              // fill to 4096

  // ─── Local operations (used when we read the full page) ──────

  // Find physical position of a suffix via linear scan of entries
  int find_entry(uint32_t suffix) const {
    for (int i = 0; i < header.num_entries + header.num_tombstones; i++) {
      if (!entries[i].is_tombstone() && entries[i].suffix == suffix) {
        return i;
      }
    }
    return -1;
  }

  // Find first free physical slot
  int find_free_slot() const {
    int total = header.num_entries + header.num_tombstones;
    for (int i = 0; i < (int)define::kMaxEntriesPerLeaf; i++) {
      if (entries[i].is_empty() || entries[i].is_tombstone()) {
        return i;
      }
    }
    return -1;
  }

  // Build sorted slot array from current entries
  void rebuild_slot_array() {
    // Collect all live (phys_pos, suffix) pairs
    struct PosKey { uint8_t pos; uint32_t suffix; };
    PosKey live[define::kMaxEntriesPerLeaf];
    int count = 0;

    for (int i = 0; i < (int)define::kMaxEntriesPerLeaf; i++) {
      if (!entries[i].is_empty() && !entries[i].is_tombstone()) {
        live[count++] = {(uint8_t)i, entries[i].suffix};
      }
    }

    // Sort by suffix
    std::sort(live, live + count,
              [](const PosKey &a, const PosKey &b) { return a.suffix < b.suffix; });

    // Fill slot array
    slot_array.init();
    for (int i = 0; i < count; i++) {
      slot_array.slots[i] = live[i].pos;
    }
  }

  // Set bloom filter bits for a suffix
  void bloom_add(uint32_t suffix) {
    uint32_t h1 = suffix & 0x1FF;         // 9 bits → bit index 0-511
    uint32_t h2 = (suffix >> 9) & 0x1FF;
    bloom_filter[h1 >> 3] |= (1u << (h1 & 7));
    bloom_filter[h2 >> 3] |= (1u << (h2 & 7));
  }

  // Check bloom filter
  bool bloom_maybe_contains(uint32_t suffix) const {
    uint32_t h1 = suffix & 0x1FF;
    uint32_t h2 = (suffix >> 9) & 0x1FF;
    return (bloom_filter[h1 >> 3] & (1u << (h1 & 7))) &&
           (bloom_filter[h2 >> 3] & (1u << (h2 & 7)));
  }

  // Rebuild bloom filter from entries
  void rebuild_bloom() {
    memset(bloom_filter, 0, sizeof(bloom_filter));
    for (int i = 0; i < (int)define::kMaxEntriesPerLeaf; i++) {
      if (!entries[i].is_empty() && !entries[i].is_tombstone()) {
        bloom_add(entries[i].suffix);
      }
    }
  }

  // Update fence keys
  void update_fences() {
    fence_min_suffix = UINT32_MAX;
    fence_max_suffix = 0;
    for (int i = 0; i < (int)define::kMaxEntriesPerLeaf; i++) {
      if (!entries[i].is_empty() && !entries[i].is_tombstone()) {
        fence_min_suffix = std::min(fence_min_suffix, entries[i].suffix);
        fence_max_suffix = std::max(fence_max_suffix, entries[i].suffix);
      }
    }
  }
};
static_assert(sizeof(LeafPage) == 4096, "LeafPage must be 4096 bytes");


// ─── Mini read result (what we get from a 16-byte targeted RDMA read) ──
struct __attribute__((packed)) LeafReadResult {
  Value    value;    // 8 bytes
  uint16_t version;  // 2 bytes  (entry version)
  uint32_t suffix;   // 4 bytes  (for verification)
  uint16_t page_ver; // 2 bytes  (page version, low 16 bits — for staleness)
  // Total: 16 bytes
};
static_assert(sizeof(LeafReadResult) == 16, "LeafReadResult must be 16 bytes");

}  // namespace apex

#endif /* __APEX_LEAF_PAGE_H__ */
