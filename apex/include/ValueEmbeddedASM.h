#ifndef __APEX_VEASM_H__
#define __APEX_VEASM_H__

#include "Common.h"
#include "AdaptiveSlotMap.h"
#include <cstring>
#include <unordered_map>
#include <algorithm>

namespace apex {

// ─── Value-Embedded ASM (VE-ASM) ──────────────────────────────────
//
// For the HOTTEST keys, stores the actual VALUE locally on the
// compute node. This eliminates RDMA entirely — the lookup
// completes in ~80ns using only L3 cache.
//
// Budget: ~5 MB total (16 entries × 24 bytes × ~13,000 active leaves)
// This fits entirely inside the L3 cache (~30-45 MB on servers).
//

// ─── VE-ASM Entry ──────────────────────────────────────────────────
struct VEASMEntry {
  Value    value;          // 8B: the actual cached value
  uint64_t version;        // 8B: version when this was cached
  uint32_t suffix;         // 4B: key suffix
  uint16_t access_count;   // 2B: access frequency
  uint8_t  valid;          // 1B: 0=invalid, 1=valid
  uint8_t  _pad;           // 1B: alignment
  // Total: 24 bytes (naturally aligned, no padding)
};
static_assert(sizeof(VEASMEntry) == 24, "VEASMEntry must be 24 bytes");


// ─── Per-Leaf VE-ASM ───────────────────────────────────────────────
// Stores the top-16 hottest keys for one leaf page.
struct LeafVEASM {
  static constexpr int kMaxHot = 16;

  VEASMEntry entries[kMaxHot];
  uint32_t   leaf_id;
  uint8_t    num_valid;
  uint8_t    _pad[3];

  void init(uint32_t id) {
    leaf_id = id;
    num_valid = 0;
    memset(entries, 0, sizeof(entries));
  }

  // ─── Lookup: suffix → cached value ──────────────────────────
  // Returns true if found (and sets *value, *version).
  // Returns false if the suffix is not in VE-ASM.
  bool lookup(uint32_t suffix, Value* value, uint64_t* version) const {
    for (int i = 0; i < num_valid; i++) {
      if (entries[i].valid && entries[i].suffix == suffix) {
        *value = entries[i].value;
        *version = entries[i].version;
        const_cast<VEASMEntry&>(entries[i]).access_count++;
        return true;
      }
    }
    return false;
  }

  // ─── Update a cached value ──────────────────────────────────
  // Returns true if the entry was found and updated.
  bool update(uint32_t suffix, Value new_value, uint64_t new_version) {
    for (int i = 0; i < num_valid; i++) {
      if (entries[i].valid && entries[i].suffix == suffix) {
        entries[i].value = new_value;
        entries[i].version = new_version;
        return true;
      }
    }
    return false;
  }

  // ─── Promote a hot key into VE-ASM ──────────────────────────
  // If VE-ASM is full, evicts the least-accessed entry.
  void promote(uint32_t suffix, Value value, uint64_t version) {
    // Check if already present
    for (int i = 0; i < num_valid; i++) {
      if (entries[i].valid && entries[i].suffix == suffix) {
        entries[i].value = value;
        entries[i].version = version;
        return;
      }
    }

    // Find a free slot
    if (num_valid < kMaxHot) {
      entries[num_valid].suffix = suffix;
      entries[num_valid].value = value;
      entries[num_valid].version = version;
      entries[num_valid].access_count = 0;
      entries[num_valid].valid = 1;
      num_valid++;
      return;
    }

    // Full — evict the least accessed
    int min_idx = 0;
    uint16_t min_count = entries[0].access_count;
    for (int i = 1; i < kMaxHot; i++) {
      if (entries[i].access_count < min_count) {
        min_count = entries[i].access_count;
        min_idx = i;
      }
    }

    entries[min_idx].suffix = suffix;
    entries[min_idx].value = value;
    entries[min_idx].version = version;
    entries[min_idx].access_count = 0;
    entries[min_idx].valid = 1;
  }

  // ─── Demote (invalidate) a stale entry ──────────────────────
  void demote(uint32_t suffix) {
    for (int i = 0; i < num_valid; i++) {
      if (entries[i].valid && entries[i].suffix == suffix) {
        entries[i].valid = 0;
        // Compact: move last valid entry here
        if (i < num_valid - 1) {
          entries[i] = entries[num_valid - 1];
        }
        num_valid--;
        return;
      }
    }
  }

  // ─── Check if suffix is cached ──────────────────────────────
  bool contains(uint32_t suffix) const {
    for (int i = 0; i < num_valid; i++) {
      if (entries[i].valid && entries[i].suffix == suffix) return true;
    }
    return false;
  }

  // ─── Reset access counters (epoch boundary) ─────────────────
  void reset_access_counts() {
    for (int i = 0; i < num_valid; i++) {
      entries[i].access_count = 0;
    }
  }
};


// ─── VE-ASM Manager ───────────────────────────────────────────────
// Manages VE-ASM entries across all leaf pages.
class VEASMManager {
public:
  VEASMManager(size_t max_memory_mb = 5)
    : max_memory_(max_memory_mb * define::MB),
      used_memory_(0),
      epoch_(0),
      ops_since_epoch_(0),
      kEpochOps(1000000) {}  // 1M ops per epoch

  ~VEASMManager() {
    for (auto& [id, veasm] : veasm_map_) {
      delete veasm;
    }
  }

  // ─── Lookup a value directly (0 RDMA if cached) ─────────────
  bool lookup(uint32_t leaf_id, uint32_t suffix, Value* value, uint64_t* version) {
    auto it = veasm_map_.find(leaf_id);
    if (it == veasm_map_.end()) return false;
    return it->second->lookup(suffix, value, version);
  }

  // ─── Update a cached value after a write ─────────────────────
  bool update(uint32_t leaf_id, uint32_t suffix, Value new_value, uint64_t new_version) {
    auto it = veasm_map_.find(leaf_id);
    if (it == veasm_map_.end()) return false;
    return it->second->update(suffix, new_value, new_version);
  }

  // ─── Promote hot keys from ASM into VE-ASM ──────────────────
  // Called at epoch boundary. Gets top-16 hot keys from ASM and
  // promotes them to VE-ASM with their current values.
  void promote_from_asm(uint32_t leaf_id, LeafASM* asm_ptr,
                        const LeafPage* page_data) {
    LeafASM::HotEntry hot[16];
    int n = asm_ptr->get_top_hot(hot, 16);
    if (n == 0) return;

    LeafVEASM* veasm = get_or_create(leaf_id);
    for (int i = 0; i < n; i++) {
      // Find the value from the page data
      if (hot[i].phys_pos < define::kMaxEntriesPerLeaf) {
        const LeafEntry& entry = page_data->entries[hot[i].phys_pos];
        if (!entry.is_empty() && !entry.is_tombstone()) {
          veasm->promote(entry.suffix, entry.value, entry.version);
        }
      }
    }
  }

  // ─── Demote a stale entry ────────────────────────────────────
  void demote(uint32_t leaf_id, uint32_t suffix) {
    auto it = veasm_map_.find(leaf_id);
    if (it != veasm_map_.end()) {
      it->second->demote(suffix);
    }
  }

  // ─── Check if a key is in VE-ASM ────────────────────────────
  bool contains(uint32_t leaf_id, uint32_t suffix) {
    auto it = veasm_map_.find(leaf_id);
    if (it == veasm_map_.end()) return false;
    return it->second->contains(suffix);
  }

  // ─── Tick operation counter; trigger epoch if needed ─────────
  void tick() {
    ops_since_epoch_++;
    if (ops_since_epoch_ >= kEpochOps) {
      advance_epoch();
      ops_since_epoch_ = 0;
    }
  }

  size_t used_memory() const { return used_memory_; }

private:
  std::unordered_map<uint32_t, LeafVEASM*> veasm_map_;
  size_t max_memory_;
  size_t used_memory_;
  uint64_t epoch_;
  uint64_t ops_since_epoch_;
  const uint64_t kEpochOps;

  LeafVEASM* get_or_create(uint32_t leaf_id) {
    auto it = veasm_map_.find(leaf_id);
    if (it != veasm_map_.end()) return it->second;

    // Check memory budget
    if (used_memory_ + sizeof(LeafVEASM) > max_memory_) {
      evict_coldest();
    }

    LeafVEASM* veasm = new LeafVEASM();
    veasm->init(leaf_id);
    veasm_map_[leaf_id] = veasm;
    used_memory_ += sizeof(LeafVEASM);
    return veasm;
  }

  void evict_coldest() {
    // Simple: remove VE-ASMs with lowest total access counts
    uint32_t min_id = 0;
    uint32_t min_total = UINT32_MAX;
    for (auto& [id, veasm] : veasm_map_) {
      uint32_t total = 0;
      for (int i = 0; i < veasm->num_valid; i++) {
        total += veasm->entries[i].access_count;
      }
      if (total < min_total) {
        min_total = total;
        min_id = id;
      }
    }

    if (min_id != 0 || veasm_map_.count(0)) {
      auto it = veasm_map_.find(min_id);
      if (it != veasm_map_.end()) {
        used_memory_ -= sizeof(LeafVEASM);
        delete it->second;
        veasm_map_.erase(it);
      }
    }
  }

  void advance_epoch() {
    epoch_++;
    // Reset access counters in all VE-ASMs
    for (auto& [id, veasm] : veasm_map_) {
      veasm->reset_access_counts();
    }
  }
};

}  // namespace apex

#endif /* __APEX_VEASM_H__ */
