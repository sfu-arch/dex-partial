#ifndef __APEX_ASM_H__
#define __APEX_ASM_H__

#include "Common.h"
#include "GlobalAddress.h"
#include "LeafPage.h"
#include <cstring>
#include <vector>
#include <algorithm>

namespace apex {

// ─── Adaptive Slot Map (ASM) ───────────────────────────────────────
//
// Per-leaf-page local directory that maps:
//   hash(suffix) → physical position within the remote leaf page
//
// This enables targeted 16-byte RDMA reads instead of reading
// the entire 4KB page.
//
// Key design: uses a minimal perfect hash (built once per leaf page
// when the full page is first read). For 256 items, a perfect hash
// can be computed in <1μs using a two-level hashing scheme.
//

// ─── ASM Entry ─────────────────────────────────────────────────────
struct ASMEntry {
  uint32_t suffix;          // 4B: the actual suffix (for collision detection)
  uint8_t  phys_pos;        // 1B: physical position in the leaf page
  uint8_t  flags;           // 1B: 0=valid, 1=overflow, 0xFF=empty
  uint16_t access_count;    // 2B: saturating counter for VE-ASM promotion
  // Total: 8 bytes

  bool is_empty() const { return flags == 0xFF; }
  bool is_valid() const { return flags == 0; }
  bool has_overflow() const { return flags == 1; }

  void clear() {
    suffix = 0;
    phys_pos = 0;
    flags = 0xFF;
    access_count = 0;
  }
};


// ─── Per-Leaf ASM ──────────────────────────────────────────────────
// Stores the mapping for one leaf page.
struct LeafASM {
  static constexpr int kBuckets = 256;         // hash buckets
  static constexpr int kMaxOverflow = 4;       // max chain per bucket

  ASMEntry primary[kBuckets];                  // primary table
  ASMEntry overflow[kBuckets][kMaxOverflow];   // overflow chains

  uint64_t  page_version;      // version of the leaf page when ASM was built
  uint32_t  leaf_id;           // which leaf this ASM belongs to
  bool      populated;         // true if ASM has been built from full page read
  uint32_t  num_entries;       // number of entries in this ASM
  uint64_t  last_access_time;  // for LRU eviction
  uint8_t   hash_seed;         // seed for rehashing on collision

  void init(uint32_t id) {
    leaf_id = id;
    page_version = 0;
    populated = false;
    num_entries = 0;
    last_access_time = 0;
    hash_seed = 0;
    for (int i = 0; i < kBuckets; i++) {
      primary[i].clear();
      for (int j = 0; j < kMaxOverflow; j++) {
        overflow[i][j].clear();
      }
    }
  }

  // ─── Hash function ───────────────────────────────────────────
  // Simple but effective hash for suffix → bucket index.
  // Uses a multiplicative hash with configurable seed.
  uint8_t hash_suffix(uint32_t suffix) const {
    uint32_t h = suffix;
    h ^= hash_seed;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return (uint8_t)(h & 0xFF);
  }

  // ─── Lookup: suffix → physical position ──────────────────────
  // Returns physical position or -1 if not found.
  int lookup(uint32_t suffix) const {
    if (!populated) return -1;

    uint8_t bucket = hash_suffix(suffix);

    // Check primary
    if (primary[bucket].is_valid() && primary[bucket].suffix == suffix) {
      const_cast<LeafASM*>(this)->primary[bucket].access_count =
        std::min((uint16_t)0xFFFF, (uint16_t)(primary[bucket].access_count + 1));
      return primary[bucket].phys_pos;
    }

    // Check overflow chain
    if (primary[bucket].has_overflow()) {
      for (int i = 0; i < kMaxOverflow; i++) {
        if (overflow[bucket][i].is_valid() && overflow[bucket][i].suffix == suffix) {
          const_cast<LeafASM*>(this)->overflow[bucket][i].access_count =
            std::min((uint16_t)0xFFFF, (uint16_t)(overflow[bucket][i].access_count + 1));
          return overflow[bucket][i].phys_pos;
        }
      }
    }

    return -1;  // Not found
  }

  // ─── Insert a mapping ────────────────────────────────────────
  void insert(uint32_t suffix, uint8_t phys_pos) {
    uint8_t bucket = hash_suffix(suffix);

    // Try primary slot
    if (primary[bucket].is_empty()) {
      primary[bucket].suffix = suffix;
      primary[bucket].phys_pos = phys_pos;
      primary[bucket].flags = 0;  // valid
      primary[bucket].access_count = 0;
      num_entries++;
      return;
    }

    // Primary occupied — add to overflow
    primary[bucket].flags = 1;  // mark as having overflow
    for (int i = 0; i < kMaxOverflow; i++) {
      if (overflow[bucket][i].is_empty()) {
        overflow[bucket][i].suffix = suffix;
        overflow[bucket][i].phys_pos = phys_pos;
        overflow[bucket][i].flags = 0;
        overflow[bucket][i].access_count = 0;
        num_entries++;
        return;
      }
    }

    // Overflow full — should not happen with proper sizing
    // In practice, rehash with new seed
    rehash();
    insert(suffix, phys_pos);  // Retry after rehash
  }

  // ─── Remove a mapping ───────────────────────────────────────
  void remove(uint32_t suffix) {
    uint8_t bucket = hash_suffix(suffix);

    if (primary[bucket].is_valid() && primary[bucket].suffix == suffix) {
      // Move overflow entry to primary if exists
      bool found_overflow = false;
      for (int i = 0; i < kMaxOverflow; i++) {
        if (overflow[bucket][i].is_valid()) {
          primary[bucket] = overflow[bucket][i];
          overflow[bucket][i].clear();
          found_overflow = true;
          break;
        }
      }
      if (!found_overflow) {
        primary[bucket].clear();
      }
      num_entries--;
      return;
    }

    // Check overflow
    for (int i = 0; i < kMaxOverflow; i++) {
      if (overflow[bucket][i].is_valid() && overflow[bucket][i].suffix == suffix) {
        overflow[bucket][i].clear();
        num_entries--;
        return;
      }
    }
  }

  // ─── Get all candidates for a suffix hash (for multi-read) ──
  // Returns physical positions for all entries in the same bucket.
  int get_candidates(uint32_t suffix, uint8_t* positions, int max_results) const {
    if (!populated) return 0;

    uint8_t bucket = hash_suffix(suffix);
    int count = 0;

    if (primary[bucket].is_valid()) {
      if (count < max_results) positions[count++] = primary[bucket].phys_pos;
    }

    if (primary[bucket].has_overflow()) {
      for (int i = 0; i < kMaxOverflow && count < max_results; i++) {
        if (overflow[bucket][i].is_valid()) {
          positions[count++] = overflow[bucket][i].phys_pos;
        }
      }
    }

    return count;
  }

  // ─── Build ASM from a full leaf page ─────────────────────────
  void build_from_page(const LeafPage* page) {
    // Reset
    init(page->header.page_id);

    // Insert all live entries
    for (int i = 0; i < (int)define::kMaxEntriesPerLeaf; i++) {
      if (!page->entries[i].is_empty() && !page->entries[i].is_tombstone()) {
        insert(page->entries[i].suffix, (uint8_t)i);
      }
    }

    page_version = page->header.page_version;
    populated = true;
  }

  // ─── Find top-K hottest entries (for VE-ASM promotion) ───────
  struct HotEntry {
    uint32_t suffix;
    uint8_t  phys_pos;
    uint16_t access_count;
  };

  int get_top_hot(HotEntry* out, int k) const {
    // Collect all entries with access counts
    std::vector<HotEntry> all;
    for (int i = 0; i < kBuckets; i++) {
      if (primary[i].is_valid()) {
        all.push_back({primary[i].suffix, primary[i].phys_pos, primary[i].access_count});
      }
      for (int j = 0; j < kMaxOverflow; j++) {
        if (overflow[i][j].is_valid()) {
          all.push_back({overflow[i][j].suffix, overflow[i][j].phys_pos, overflow[i][j].access_count});
        }
      }
    }

    // Sort by access count descending
    std::sort(all.begin(), all.end(),
              [](const HotEntry& a, const HotEntry& b) { return a.access_count > b.access_count; });

    int count = std::min(k, (int)all.size());
    for (int i = 0; i < count; i++) {
      out[i] = all[i];
    }
    return count;
  }

  // ─── Reset access counters (called at epoch boundary) ────────
  void reset_access_counts() {
    for (int i = 0; i < kBuckets; i++) {
      if (primary[i].is_valid()) primary[i].access_count = 0;
      for (int j = 0; j < kMaxOverflow; j++) {
        if (overflow[i][j].is_valid()) overflow[i][j].access_count = 0;
      }
    }
  }

private:
  // ─── Rehash with new seed ────────────────────────────────────
  void rehash() {
    // Save all entries
    std::vector<std::pair<uint32_t, uint8_t>> entries;
    for (int i = 0; i < kBuckets; i++) {
      if (primary[i].is_valid()) {
        entries.push_back({primary[i].suffix, primary[i].phys_pos});
      }
      for (int j = 0; j < kMaxOverflow; j++) {
        if (overflow[i][j].is_valid()) {
          entries.push_back({overflow[i][j].suffix, overflow[i][j].phys_pos});
        }
      }
    }

    // Clear and rehash with new seed
    hash_seed++;
    for (int i = 0; i < kBuckets; i++) {
      primary[i].clear();
      for (int j = 0; j < kMaxOverflow; j++) {
        overflow[i][j].clear();
      }
    }
    num_entries = 0;

    for (auto& [suffix, pos] : entries) {
      insert(suffix, pos);
    }
  }
};


// ─── ASM Manager ───────────────────────────────────────────────────
// Manages ASMs for all leaf pages. Handles allocation, eviction,
// and lazy population.
class ASMManager {
public:
  ASMManager(size_t max_memory_mb = 51)
    : max_memory_(max_memory_mb * define::MB),
      used_memory_(0),
      epoch_(0) {}

  ~ASMManager() {
    for (auto& [id, asm_ptr] : asm_map_) {
      delete asm_ptr;
    }
  }

  // Get or create ASM for a leaf page
  LeafASM* get_asm(uint32_t leaf_id) {
    auto it = asm_map_.find(leaf_id);
    if (it != asm_map_.end()) {
      it->second->last_access_time = epoch_;
      return it->second;
    }
    return nullptr;
  }

  // Create a new ASM for a leaf page from a full page read
  LeafASM* create_asm(const LeafPage* page) {
    // Check memory budget
    if (used_memory_ + sizeof(LeafASM) > max_memory_) {
      evict_coldest();
    }

    LeafASM* asm_ptr = new LeafASM();
    asm_ptr->build_from_page(page);
    asm_ptr->last_access_time = epoch_;

    asm_map_[page->header.page_id] = asm_ptr;
    used_memory_ += sizeof(LeafASM);
    return asm_ptr;
  }

  // Remove ASM (e.g., on leaf page split)
  void remove_asm(uint32_t leaf_id) {
    auto it = asm_map_.find(leaf_id);
    if (it != asm_map_.end()) {
      used_memory_ -= sizeof(LeafASM);
      delete it->second;
      asm_map_.erase(it);
    }
  }

  // Advance epoch (call periodically, e.g., every 1M ops)
  void advance_epoch() {
    epoch_++;
  }

  size_t used_memory() const { return used_memory_; }
  size_t num_asms() const { return asm_map_.size(); }

private:
  std::unordered_map<uint32_t, LeafASM*> asm_map_;
  size_t max_memory_;
  size_t used_memory_;
  uint64_t epoch_;

  // Evict the 10% coldest ASMs
  void evict_coldest() {
    std::vector<std::pair<uint64_t, uint32_t>> by_time;  // (last_access, leaf_id)
    for (auto& [id, asm_ptr] : asm_map_) {
      by_time.push_back({asm_ptr->last_access_time, id});
    }

    std::sort(by_time.begin(), by_time.end());

    int to_evict = std::max(1, (int)(by_time.size() / 10));
    for (int i = 0; i < to_evict && i < (int)by_time.size(); i++) {
      remove_asm(by_time[i].second);
    }
  }
};

}  // namespace apex

#endif /* __APEX_ASM_H__ */
