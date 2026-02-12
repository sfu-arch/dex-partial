#ifndef __APEX_VCS_H__
#define __APEX_VCS_H__

#include "Common.h"
#include "GlobalAddress.h"
#include "LeafPage.h"
#include <cstring>
#include <vector>

namespace apex {

// ─── Version-Chained Sync (VCS) ───────────────────────────────────
//
// Instead of broadcasting cache invalidations (DEX, O(N) per write)
// or using lease-based validation (CHIME, adds latency), APEX uses
// an append-only delta log per leaf page on the remote memory node.
//
// Writers append deltas. Readers lazily discover changes by checking
// the version chain when they detect a stale entry.
//

// ─── Version Chain Delta Entry ─────────────────────────────────────
// Stored on the REMOTE memory node, appended to each leaf page.
struct __attribute__((packed)) VersionDelta {
  uint8_t  phys_pos;       // 1B: which physical position changed
  uint8_t  op_type;        // 1B: 0=update, 1=insert, 2=delete
  uint16_t _pad;           // 2B: alignment
  uint32_t suffix;         // 4B: the suffix that was affected
  Value    new_value;      // 8B: new value (0 for delete)
  uint64_t new_version;    // 8B: new version number
  // Total: 24 bytes
};
static_assert(sizeof(VersionDelta) == 24, "VersionDelta must be 24 bytes");

// Op types
constexpr uint8_t VCS_OP_UPDATE = 0;
constexpr uint8_t VCS_OP_INSERT = 1;
constexpr uint8_t VCS_OP_DELETE = 2;


// ─── Version Chain (remote, per leaf page) ─────────────────────────
//
// Layout on remote memory (appended after the leaf page):
//
//   Leaf Page:   [0, 4096)
//   VC Header:   [4096, 4112)   16 bytes
//   VC Entries:  [4112, 5648)   64 × 24 = 1536 bytes
//
//   Total allocation per leaf: 4096 + 16 + 1536 = 5648 bytes
//
struct __attribute__((packed)) VersionChainHeader {
  uint64_t head_version;   // 8B: version of the newest delta
  uint16_t num_entries;    // 2B: number of deltas in the chain
  uint16_t head_idx;       // 2B: circular buffer write position
  uint32_t _pad;           // 4B: alignment
  // Total: 16 bytes
};
static_assert(sizeof(VersionChainHeader) == 16, "VersionChainHeader must be 16 bytes");


struct VersionChain {
  static constexpr int kCapacity = 64;

  VersionChainHeader header;
  VersionDelta       deltas[kCapacity];

  void init() {
    memset(&header, 0, sizeof(header));
    memset(deltas, 0, sizeof(deltas));
  }

  // Append a new delta (called by writer node)
  void append(uint8_t phys_pos, uint8_t op_type, uint32_t suffix,
              Value new_value, uint64_t new_version) {
    uint16_t idx = header.head_idx;
    deltas[idx].phys_pos = phys_pos;
    deltas[idx].op_type = op_type;
    deltas[idx].suffix = suffix;
    deltas[idx].new_value = new_value;
    deltas[idx].new_version = new_version;

    header.head_idx = (idx + 1) % kCapacity;
    header.num_entries = std::min((uint16_t)(header.num_entries + 1), (uint16_t)kCapacity);
    header.head_version = new_version;
  }

  // Get all deltas newer than a given version
  // Returns them in chronological order.
  int get_deltas_since(uint64_t since_version, VersionDelta* out, int max_results) const {
    int count = 0;
    int start = (header.head_idx - header.num_entries + kCapacity) % kCapacity;

    for (int i = 0; i < header.num_entries && count < max_results; i++) {
      int idx = (start + i) % kCapacity;
      if (deltas[idx].new_version > since_version) {
        out[count++] = deltas[idx];
      }
    }
    return count;
  }
};

constexpr uint32_t kVersionChainOffset = define::kLeafPageSize;  // starts right after leaf page
constexpr uint32_t kTotalLeafAllocation = define::kLeafPageSize + sizeof(VersionChain);


// ─── Local Version Tracker ─────────────────────────────────────────
// Kept on the compute node. Tracks which version of each leaf page
// we've synced to, so we know whether our ASM/VE-ASM is stale.
// Uses a pre-sized vector indexed by leaf_id for O(1) thread-safe access.
class VersionTracker {
public:
  void resize(uint32_t num_leaves) {
    versions_.resize(num_leaves, 0);
  }

  void set_version(uint32_t leaf_id, uint64_t version) {
    if (leaf_id < versions_.size()) versions_[leaf_id] = version;
  }

  uint64_t get_version(uint32_t leaf_id) const {
    if (leaf_id < versions_.size()) return versions_[leaf_id];
    return 0;  // Never synced
  }

  void clear(uint32_t leaf_id) {
    if (leaf_id < versions_.size()) versions_[leaf_id] = 0;
  }

private:
  std::vector<uint64_t> versions_;
};

}  // namespace apex

#endif /* __APEX_VCS_H__ */
