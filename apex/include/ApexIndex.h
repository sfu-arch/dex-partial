#ifndef __APEX_INDEX_H__
#define __APEX_INDEX_H__

#include "Common.h"
#include "DSM.h"
#include "CompressedPrefixTrie.h"
#include "AdaptiveSlotMap.h"
#include "ValueEmbeddedASM.h"
#include "VersionChainSync.h"
#include "LeafPage.h"
#include "Timer.h"

#include <vector>
#include <atomic>

namespace apex {

// ─── APEX Index ────────────────────────────────────────────────────
//
// The main index class that orchestrates all components:
//   ValueCache → CPT → ASM → Targeted RDMA
//
// Lookup cascade (per operation):
//   1. Per-thread value cache check   (~5ns,  0 RDMA)   → hit? done
//   2. CPT prefix traversal           (~80ns, 0 RDMA)   → leaf_addr + leaf_id
//   3. ASM slot lookup                (~10ns, 0 RDMA)   → phys_pos
//   4. Targeted RDMA read             (~2μs, 14 bytes)  → value
//
// All RDMA operations go through the DSM layer.
//

// ─── Statistics counters (per-thread, cache-line padded) ───────────
struct alignas(64) ThreadStats {
  uint64_t cache_hits;       // Value cache hits (0 RDMA)
  uint64_t asm_hits;         // ASM targeted reads (1 RDMA, 14 bytes)
  uint64_t full_page_reads;  // Full page reads (1 RDMA, 4 KB)
  uint64_t negative_local;   // Negative lookups resolved locally (0 RDMA)
  uint64_t negative_rdma;    // Negative lookups that needed RDMA
  uint64_t version_resyncs;  // Version chain re-reads due to staleness
  uint64_t inserts;
  uint64_t updates;
  uint64_t deletes;
  uint64_t range_scans;
  uint64_t rdma_reads;       // Total RDMA read count
  uint64_t rdma_bytes;       // Total RDMA bytes read
};


// ─── Per-Thread Direct-Mapped Value Cache ──────────────────────────
//
// Caches recent (Key → Value) results per thread.
// On a hit, lookup completes in ~5ns with 0 RDMA — matching DEX's
// swizzled pointer performance for hot keys.
//
// Design: direct-mapped hash table (8192 slots × 16B = 128 KB/thread).
// Under Zipf(0.99), captures >95% of hot-key accesses.
// No thread safety needed — each thread owns its cache exclusively.
//
struct alignas(64) ThreadValueCache {
  static constexpr uint32_t kSlots = 8192;   // power of 2
  static constexpr uint32_t kMask  = kSlots - 1;

  struct Entry {
    Key   key;     // 8B
    Value value;   // 8B
  };

  Entry slots[kSlots];   // 128 KB total

  void init() { memset(slots, 0, sizeof(slots)); }

  bool lookup(Key key, Value* result) const {
    if (key == 0) return false;  // key=0 is our "empty" sentinel
    uint32_t idx = fast_hash(key) & kMask;
    const Entry& e = slots[idx];
    if (e.key == key) {
      *result = e.value;
      return true;
    }
    return false;
  }

  void insert(Key key, Value value) {
    if (key == 0) return;
    uint32_t idx = fast_hash(key) & kMask;
    slots[idx] = {key, value};
  }

  void invalidate(Key key) {
    if (key == 0) return;
    uint32_t idx = fast_hash(key) & kMask;
    if (slots[idx].key == key) {
      slots[idx].key = 0;
    }
  }

  static uint32_t fast_hash(Key key) {
    // MurmurHash3 64-bit finalizer
    uint64_t h = key;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return (uint32_t)h;
  }
};


class ApexIndex {
public:
  // ─── Construction ────────────────────────────────────────────
  ApexIndex(DSM* dsm, uint16_t tree_id = 0)
    : dsm_(dsm),
      tree_id_(tree_id),
      asm_mgr_(512),        // 512 MB budget — fits all ~39K ASMs for 10M keys
      veasm_mgr_(5),
      next_leaf_id_(0),
      num_leaves_(0) {
    memset(stats_, 0, sizeof(stats_));
    for (int i = 0; i < MAX_APP_THREAD; i++) {
      value_cache_[i].init();
    }
  }

  // ─── Bulk Load ───────────────────────────────────────────────
  // Loads sorted key-value pairs into leaf pages on the remote
  // memory node, builds the CPT, and pre-populates all ASMs.
  void bulk_load(Key* keys, Value* values, uint64_t count) {
    uint64_t num_pages = (count + define::kMaxEntriesPerLeaf - 1) / define::kMaxEntriesPerLeaf;

    // Pre-allocate metadata vectors (O(1) indexed access, thread-safe reads)
    num_leaves_ = (uint32_t)num_pages;
    leaf_addrs_.resize(num_pages);
    leaf_prefix_depth_.resize(num_pages);

    // Pre-size version tracker
    version_tracker_.resize(num_pages);

    char* buf = dsm_->get_rdma_buffer();

    for (uint64_t p = 0; p < num_pages; p++) {
      uint64_t start = p * define::kMaxEntriesPerLeaf;
      uint64_t end = std::min(start + define::kMaxEntriesPerLeaf, count);
      uint32_t n_entries = (uint32_t)(end - start);

      // Allocate remote page (leaf + version chain)
      GlobalAddress page_addr = dsm_->alloc(kTotalLeafAllocation);

      uint32_t leaf_id = next_leaf_id_++;

      // Build the leaf page locally
      LeafPage page;
      memset(&page, 0, sizeof(page));

      page.header.page_id = leaf_id;
      page.header.page_version = 1;
      page.header.num_entries = n_entries;
      page.header.num_tombstones = 0;
      page.header.split_flag = 0;

      Key min_key = keys[start];
      Key max_key = keys[end - 1];
      int prefix_bytes = common_prefix_bytes(min_key, max_key);

      // Fill entries in sorted order
      for (uint32_t i = 0; i < n_entries; i++) {
        uint32_t suffix = extract_suffix(keys[start + i], prefix_bytes);
        page.entries[i].suffix = suffix;
        page.entries[i].value = values[start + i];
        page.entries[i].version = 1;
        page.slot_array.slots[i] = i;  // sorted = physical for bulk load
        page.bloom_add(suffix);
      }

      // Set fence keys
      page.fence_min_suffix = page.entries[0].suffix;
      page.fence_max_suffix = page.entries[n_entries - 1].suffix;

      // Initialize version chain
      VersionChain vc;
      vc.init();

      // *** Combined RDMA write: leaf page + version chain in one shot ***
      memcpy(buf, &page, sizeof(LeafPage));
      memcpy(buf + kVersionChainOffset, &vc, sizeof(VersionChain));
      dsm_->write_sync(buf, page_addr, kTotalLeafAllocation);

      // Insert into CPT
      Key representative = keys[start];
      cpt_.insert_leaf(representative, page_addr, leaf_id, min_key, max_key);

      // Store metadata in pre-sized vectors (O(1) access)
      leaf_addrs_[leaf_id] = page_addr;
      leaf_prefix_depth_[leaf_id] = prefix_bytes;

      // Set initial version
      version_tracker_.set_version(leaf_id, 1);

      // *** KEY FIX: Pre-populate ASM from the locally-built page ***
      // No extra RDMA needed — we have the page data right here.
      asm_mgr_.create_asm(&page);
    }

    printf("[APEX] Bulk load: %u leaves, %zu ASMs pre-populated (%.1f MB)\n",
           num_leaves_, asm_mgr_.num_asms(),
           asm_mgr_.used_memory() / (double)define::MB);
  }


  // ═══════════════════════════════════════════════════════════════
  //  POINT LOOKUP  (hot path — every instruction counts)
  // ═══════════════════════════════════════════════════════════════
  //
  //  Cascade: ValueCache → CPT → ASM → Targeted RDMA
  //

  bool lookup(Key key, Value* result) {
    int tid = dsm_->getMyThreadID();
    auto& stats = stats_[tid];

    // ── Step 0: Per-thread value cache (0 RDMA, ~5ns) ─────────
    // Direct-mapped hash check. Under Zipf(0.99), >95% hit rate
    // after warmup. Matches DEX's swizzled-pointer 0-RDMA path.
    auto& cache = value_cache_[tid];
    if (cache.lookup(key, result)) {
      stats.cache_hits++;
      return true;
    }

    // ── Step 1: CPT traversal (LOCAL, ~80ns) ──────────────────
    // Returns leaf_addr + leaf_id directly — no reverse-map lookup.
    int prefix_depth;
    uint32_t leaf_id;
    GlobalAddress leaf_addr = cpt_.lookup(key, prefix_depth, leaf_id);

    if (leaf_addr == GlobalAddress::Null()) {
      stats.negative_local++;
      return false;
    }

    uint32_t suffix = extract_suffix(key, prefix_depth);

    // ── Step 2: ASM lookup (LOCAL, ~10ns) ─────────────────────
    // Pre-populated during bulk_load — no full-page fetch needed.
    LeafASM* asm_ptr = asm_mgr_.get_asm(leaf_id);

    if (asm_ptr && asm_ptr->populated) {
      int phys_pos = asm_ptr->lookup(suffix);

      if (phys_pos < 0) {
        // Suffix not in populated ASM → key doesn't exist
        stats.negative_local++;
        return false;
      }

      // ── Step 3: Targeted RDMA read (14 bytes) ──────────────
      uint64_t entry_offset = sizeof(LeafHeader) + sizeof(SlotArray) +
                              phys_pos * sizeof(LeafEntry);
      char* buf = dsm_->get_rdma_buffer();
      dsm_->read_sync(buf, leaf_addr + entry_offset, sizeof(LeafEntry));
      stats.rdma_reads++;
      stats.rdma_bytes += sizeof(LeafEntry);

      LeafEntry* entry = (LeafEntry*)buf;

      // Verify suffix matches (ASM might be stale after remote writes)
      if (entry->suffix == suffix && !entry->is_tombstone()) {
        *result = entry->value;
        stats.asm_hits++;

        // Cache the result for future 0-RDMA lookups
        cache.insert(key, entry->value);
        return true;
      }

      // Suffix mismatch → ASM stale, need resync
      stats.version_resyncs++;
      return resync_and_lookup(leaf_id, leaf_addr, suffix, key, result, stats);
    }

    // ── Step 4: Full page fetch (should only happen for newly
    //    inserted pages that weren't part of bulk_load) ─────────
    return full_page_fetch_and_lookup(leaf_id, leaf_addr, suffix, key, result, stats);
  }


  // ═══════════════════════════════════════════════════════════════
  //  NEGATIVE LOOKUP (exists check)
  // ═══════════════════════════════════════════════════════════════

  bool exists(Key key) {
    int tid = dsm_->getMyThreadID();
    auto& stats = stats_[tid];

    // Check value cache first
    Value dummy;
    auto& cache = value_cache_[tid];
    if (cache.lookup(key, &dummy)) {
      return true;
    }

    // CPT check
    int prefix_depth;
    uint32_t leaf_id;
    GlobalAddress leaf_addr = cpt_.lookup(key, prefix_depth, leaf_id);
    if (leaf_addr == GlobalAddress::Null()) {
      stats.negative_local++;
      return false;
    }

    uint32_t suffix = extract_suffix(key, prefix_depth);

    // ASM check
    LeafASM* asm_ptr = asm_mgr_.get_asm(leaf_id);
    if (asm_ptr && asm_ptr->populated) {
      int phys_pos = asm_ptr->lookup(suffix);
      if (phys_pos < 0) {
        stats.negative_local++;
        return false;
      }
      return true;  // Exists in ASM
    }

    // Must do full page fetch
    return full_page_fetch_and_lookup(leaf_id, leaf_addr, suffix, key, &dummy, stats);
  }


  // ═══════════════════════════════════════════════════════════════
  //  RANGE SCAN
  // ═══════════════════════════════════════════════════════════════

  int range_scan(Key start_key, int count, std::pair<Key, Value>* results) {
    int tid = dsm_->getMyThreadID();
    auto& stats = stats_[tid];
    stats.range_scans++;

    TrieNode* leaf_node = cpt_.lower_bound(start_key);
    int found = 0;

    while (leaf_node && found < count) {
      GlobalAddress leaf_addr = leaf_node->leaf_addr;
      uint32_t leaf_id = leaf_node->leaf_id;
      int prefix_depth = (leaf_id < (uint32_t)leaf_prefix_depth_.size())
          ? leaf_prefix_depth_[leaf_id] : 0;

      // Read full leaf page (range scan needs contiguous access)
      char* buf = dsm_->get_rdma_buffer();
      dsm_->read_sync(buf, leaf_addr, sizeof(LeafPage));
      stats.rdma_reads++;
      stats.rdma_bytes += sizeof(LeafPage);

      LeafPage* page = (LeafPage*)buf;

      // Refresh ASM from full page while we have it
      asm_mgr_.create_asm(page);

      // Scan in sorted order via slot array
      for (int i = 0; i < page->header.num_entries && found < count; i++) {
        uint8_t phys = page->slot_array.get_physical_pos(i);
        if (phys == 0xFF) break;

        LeafEntry& entry = page->entries[phys];
        if (entry.is_empty() || entry.is_tombstone()) continue;

        Key full_key = reconstruct_key(leaf_node->min_key, entry.suffix, prefix_depth);

        if (full_key >= start_key) {
          Value val = entry.value;
          results[found++] = {full_key, val};
        }
      }

      leaf_node = leaf_node->next_leaf;
    }

    return found;
  }


  // ═══════════════════════════════════════════════════════════════
  //  INSERT
  // ═══════════════════════════════════════════════════════════════

  bool insert(Key key, Value value) {
    int tid = dsm_->getMyThreadID();
    auto& stats = stats_[tid];
    stats.inserts++;

    int prefix_depth;
    uint32_t leaf_id;
    GlobalAddress leaf_addr = cpt_.lookup(key, prefix_depth, leaf_id);

    if (leaf_addr == GlobalAddress::Null()) {
      return insert_new_leaf(key, value);
    }

    uint32_t suffix = extract_suffix(key, prefix_depth);

    // Read current page header
    char* buf = dsm_->get_rdma_buffer();
    dsm_->read_sync(buf, leaf_addr, sizeof(LeafHeader));
    stats.rdma_reads++;
    stats.rdma_bytes += sizeof(LeafHeader);

    LeafHeader* hdr = (LeafHeader*)buf;

    if (hdr->num_entries >= define::kMaxEntriesPerLeaf) {
      return split_and_insert(leaf_id, leaf_addr, key, value, prefix_depth);
    }

    // Read full page to find free slot
    dsm_->read_sync(buf, leaf_addr, sizeof(LeafPage));
    stats.rdma_reads++;
    stats.rdma_bytes += sizeof(LeafPage);

    LeafPage* page = (LeafPage*)buf;

    int free_pos = page->find_free_slot();
    if (free_pos < 0) {
      return split_and_insert(leaf_id, leaf_addr, key, value, prefix_depth);
    }

    // Write new entry
    LeafEntry new_entry;
    new_entry.suffix = suffix;
    new_entry.value = value;
    new_entry.version = (uint16_t)(hdr->page_version + 1);

    uint64_t entry_offset = sizeof(LeafHeader) + sizeof(SlotArray) +
                            free_pos * sizeof(LeafEntry);
    memcpy(buf, &new_entry, sizeof(LeafEntry));
    dsm_->write_sync(buf, leaf_addr + entry_offset, sizeof(LeafEntry));

    // Update header + slot array
    page->header.page_version++;
    page->header.num_entries++;

    int sorted_pos = 0;
    for (int i = 0; i < page->header.num_entries - 1; i++) {
      uint8_t phys = page->slot_array.get_physical_pos(i);
      if (phys == 0xFF) break;
      if (page->entries[phys].suffix < suffix) {
        sorted_pos = i + 1;
      } else {
        break;
      }
    }

    for (int i = page->header.num_entries - 1; i > sorted_pos; i--) {
      page->slot_array.slots[i] = page->slot_array.slots[i - 1];
    }
    page->slot_array.slots[sorted_pos] = (uint8_t)free_pos;

    memcpy(buf, &page->header, sizeof(LeafHeader));
    memcpy(buf + sizeof(LeafHeader), &page->slot_array, sizeof(SlotArray));
    dsm_->write_sync(buf, leaf_addr, sizeof(LeafHeader) + sizeof(SlotArray));

    // Update local ASM
    LeafASM* asm_ptr = asm_mgr_.get_asm(leaf_id);
    if (asm_ptr) {
      asm_ptr->insert(suffix, (uint8_t)free_pos);
      asm_ptr->page_version = page->header.page_version;
    }

    // Invalidate value cache (stale after write)
    value_cache_[tid].invalidate(key);

    // Append to version chain
    append_version_delta(leaf_addr, (uint8_t)free_pos, VCS_OP_INSERT,
                         suffix, value, page->header.page_version);

    return true;
  }


  // ═══════════════════════════════════════════════════════════════
  //  UPDATE
  // ═══════════════════════════════════════════════════════════════

  bool update(Key key, Value new_value) {
    int tid = dsm_->getMyThreadID();
    auto& stats = stats_[tid];
    stats.updates++;

    int prefix_depth;
    uint32_t leaf_id;
    GlobalAddress leaf_addr = cpt_.lookup(key, prefix_depth, leaf_id);
    if (leaf_addr == GlobalAddress::Null()) return false;

    uint32_t suffix = extract_suffix(key, prefix_depth);

    // Find physical position via ASM
    LeafASM* asm_ptr = asm_mgr_.get_asm(leaf_id);
    int phys_pos = -1;

    if (asm_ptr && asm_ptr->populated) {
      phys_pos = asm_ptr->lookup(suffix);
    }

    if (phys_pos < 0) {
      char* buf = dsm_->get_rdma_buffer();
      dsm_->read_sync(buf, leaf_addr, sizeof(LeafPage));
      stats.rdma_reads++;
      stats.rdma_bytes += sizeof(LeafPage);

      LeafPage* page = (LeafPage*)buf;
      phys_pos = page->find_entry(suffix);
      if (phys_pos < 0) return false;

      asm_mgr_.create_asm(page);
    }

    // Read current entry
    uint64_t entry_offset = sizeof(LeafHeader) + sizeof(SlotArray) +
                            phys_pos * sizeof(LeafEntry);
    char* buf = dsm_->get_rdma_buffer();
    dsm_->read_sync(buf, leaf_addr + entry_offset, sizeof(LeafEntry));
    stats.rdma_reads++;
    stats.rdma_bytes += sizeof(LeafEntry);

    LeafEntry* entry = (LeafEntry*)buf;
    uint16_t new_version = entry->version + 1;

    // Write new value
    entry->value = new_value;
    entry->version = new_version;
    dsm_->write_sync((char*)entry, leaf_addr + entry_offset, sizeof(LeafEntry));

    // Update page version
    uint64_t page_ver;
    dsm_->read_sync(buf, leaf_addr, sizeof(uint64_t));
    page_ver = *(uint64_t*)buf + 1;
    dsm_->write_sync((char*)&page_ver, leaf_addr, sizeof(uint64_t));

    // Invalidate value cache (stale after write)
    value_cache_[tid].invalidate(key);

    // Append to version chain
    append_version_delta(leaf_addr, (uint8_t)phys_pos, VCS_OP_UPDATE,
                         suffix, new_value, page_ver);

    return true;
  }


  // ═══════════════════════════════════════════════════════════════
  //  DELETE
  // ═══════════════════════════════════════════════════════════════

  bool remove(Key key) {
    int tid = dsm_->getMyThreadID();
    auto& stats = stats_[tid];
    stats.deletes++;

    int prefix_depth;
    uint32_t leaf_id;
    GlobalAddress leaf_addr = cpt_.lookup(key, prefix_depth, leaf_id);
    if (leaf_addr == GlobalAddress::Null()) return false;

    uint32_t suffix = extract_suffix(key, prefix_depth);

    LeafASM* asm_ptr = asm_mgr_.get_asm(leaf_id);
    int phys_pos = -1;
    if (asm_ptr && asm_ptr->populated) {
      phys_pos = asm_ptr->lookup(suffix);
    }

    if (phys_pos < 0) {
      char* buf = dsm_->get_rdma_buffer();
      dsm_->read_sync(buf, leaf_addr, sizeof(LeafPage));
      stats.rdma_reads++;
      stats.rdma_bytes += sizeof(LeafPage);
      LeafPage* page = (LeafPage*)buf;
      phys_pos = page->find_entry(suffix);
      if (phys_pos < 0) return false;
      asm_mgr_.create_asm(page);
    }

    // Tombstone the entry
    uint64_t entry_offset = sizeof(LeafHeader) + sizeof(SlotArray) +
                            phys_pos * sizeof(LeafEntry);
    char* buf = dsm_->get_rdma_buffer();

    LeafEntry tombstone;
    tombstone.suffix = suffix;
    tombstone.value = 0;
    tombstone.version = 0xFFFF;  // tombstone marker
    memcpy(buf, &tombstone, sizeof(LeafEntry));
    dsm_->write_sync(buf, leaf_addr + entry_offset, sizeof(LeafEntry));

    // Update local ASM
    if (asm_ptr) {
      asm_ptr->remove(suffix);
    }

    // Invalidate value cache
    value_cache_[tid].invalidate(key);

    // Update page version
    dsm_->read_sync(buf, leaf_addr, sizeof(uint64_t));
    stats.rdma_reads++;
    stats.rdma_bytes += sizeof(uint64_t);
    uint64_t page_ver = *(uint64_t*)buf + 1;
    dsm_->write_sync((char*)&page_ver, leaf_addr, sizeof(uint64_t));

    // Append to version chain
    append_version_delta(leaf_addr, (uint8_t)phys_pos, VCS_OP_DELETE,
                         suffix, 0, page_ver);

    return true;
  }


  // ═══════════════════════════════════════════════════════════════
  //  STATISTICS
  // ═══════════════════════════════════════════════════════════════

  ThreadStats* get_stats() { return stats_; }

  void print_stats() {
    uint64_t total_cache = 0, total_asm = 0, total_full = 0;
    uint64_t total_neg_local = 0, total_neg_rdma = 0, total_resync = 0;
    uint64_t total_rdma = 0, total_bytes = 0;
    uint64_t total_inserts = 0, total_updates = 0, total_deletes = 0, total_scans = 0;

    for (int i = 0; i < MAX_APP_THREAD; i++) {
      total_cache += stats_[i].cache_hits;
      total_asm += stats_[i].asm_hits;
      total_full += stats_[i].full_page_reads;
      total_neg_local += stats_[i].negative_local;
      total_neg_rdma += stats_[i].negative_rdma;
      total_resync += stats_[i].version_resyncs;
      total_rdma += stats_[i].rdma_reads;
      total_bytes += stats_[i].rdma_bytes;
      total_inserts += stats_[i].inserts;
      total_updates += stats_[i].updates;
      total_deletes += stats_[i].deletes;
      total_scans += stats_[i].range_scans;
    }

    uint64_t total_ops = total_cache + total_asm + total_full + total_neg_local + total_neg_rdma;
    printf("=== APEX Statistics ===\n");
    printf("  Value cache hits:  %lu (%.1f%%)\n", total_cache, 100.0 * total_cache / std::max(total_ops, 1UL));
    printf("  ASM hits:          %lu (%.1f%%)\n", total_asm, 100.0 * total_asm / std::max(total_ops, 1UL));
    printf("  Full page reads:   %lu (%.1f%%)\n", total_full, 100.0 * total_full / std::max(total_ops, 1UL));
    printf("  Negative (local):  %lu (%.1f%%)\n", total_neg_local, 100.0 * total_neg_local / std::max(total_ops, 1UL));
    printf("  Negative (RDMA):   %lu (%.1f%%)\n", total_neg_rdma, 100.0 * total_neg_rdma / std::max(total_ops, 1UL));
    printf("  Version resyncs:   %lu\n", total_resync);
    printf("  Total RDMA reads:  %lu\n", total_rdma);
    printf("  Total RDMA bytes:  %lu (%.2f MB)\n", total_bytes, total_bytes / (double)define::MB);
    printf("  Avg RDMA bytes/op: %.1f\n", total_rdma > 0 ? (double)total_bytes / total_rdma : 0.0);
    printf("  Inserts: %lu  Updates: %lu  Deletes: %lu  Scans: %lu\n",
           total_inserts, total_updates, total_deletes, total_scans);
    printf("  CPT leaves:        %u\n", cpt_.num_leaves());
    printf("  ASM count:         %zu (%.2f MB)\n", asm_mgr_.num_asms(), asm_mgr_.used_memory() / (double)define::MB);
  }

  void reset_stats() {
    memset(stats_, 0, sizeof(stats_));
  }

  CompressedPrefixTrie& get_trie() { return cpt_; }

private:
  DSM* dsm_;
  uint16_t tree_id_;

  // APEX components
  CompressedPrefixTrie cpt_;
  ASMManager asm_mgr_;
  VEASMManager veasm_mgr_;    // kept for future use, not in hot path
  VersionTracker version_tracker_;

  // Pre-sized metadata (populated during bulk_load, read-only during benchmark)
  std::vector<GlobalAddress> leaf_addrs_;      // leaf_id → remote address
  std::vector<int> leaf_prefix_depth_;         // leaf_id → prefix bytes
  uint32_t num_leaves_;
  std::atomic<uint32_t> next_leaf_id_;

  // Per-thread value cache (0-RDMA hot-key lookups)
  ThreadValueCache value_cache_[MAX_APP_THREAD];

  // Per-thread stats
  ThreadStats stats_[MAX_APP_THREAD];


  // ─── Helper: common prefix bytes between two keys ────────────
  static int common_prefix_bytes(Key a, Key b) {
    uint8_t ba[8], bb[8];
    key_to_bytes(a, ba);
    key_to_bytes(b, bb);
    int common = 0;
    for (int i = 0; i < 8; i++) {
      if (ba[i] == bb[i]) common++;
      else break;
    }
    return common;
  }

  // ─── Helper: reconstruct full key from min_key + suffix ──────
  static Key reconstruct_key(Key min_key, uint32_t suffix, int prefix_depth) {
    uint8_t bytes[8];
    key_to_bytes(min_key, bytes);

    for (int i = 0; i < 4 && (prefix_depth + i) < 8; i++) {
      bytes[prefix_depth + i] = (suffix >> (8 * (3 - i))) & 0xFF;
    }

    return bytes_to_key(bytes);
  }

  // ─── Full page fetch and lookup ──────────────────────────────
  // Only called when ASM is not yet populated (newly inserted pages).
  bool full_page_fetch_and_lookup(uint32_t leaf_id, GlobalAddress leaf_addr,
                                  uint32_t suffix, Key original_key,
                                  Value* result, ThreadStats& stats) {
    char* buf = dsm_->get_rdma_buffer();
    dsm_->read_sync(buf, leaf_addr, sizeof(LeafPage));
    stats.rdma_reads++;
    stats.rdma_bytes += sizeof(LeafPage);
    stats.full_page_reads++;

    LeafPage* page = (LeafPage*)buf;

    // Build/refresh ASM from the full page
    asm_mgr_.create_asm(page);
    version_tracker_.set_version(leaf_id, page->header.page_version);

    // Search for the suffix
    int pos = page->find_entry(suffix);
    if (pos >= 0) {
      *result = page->entries[pos].value;

      // Cache the result
      int tid = dsm_->getMyThreadID();
      value_cache_[tid].insert(original_key, page->entries[pos].value);
      return true;
    }

    stats.negative_rdma++;
    return false;
  }

  // ─── Resync via version chain and retry lookup ───────────────
  bool resync_and_lookup(uint32_t leaf_id, GlobalAddress leaf_addr,
                         uint32_t suffix, Key original_key,
                         Value* result, ThreadStats& stats) {
    // Read version chain
    char* buf = dsm_->get_rdma_buffer();
    dsm_->read_sync(buf, leaf_addr + kVersionChainOffset, sizeof(VersionChain));
    stats.rdma_reads++;
    stats.rdma_bytes += sizeof(VersionChain);

    VersionChain* vc = (VersionChain*)buf;

    // Get deltas since our last known version
    uint64_t our_version = version_tracker_.get_version(leaf_id);
    VersionDelta deltas[64];
    int n_deltas = vc->get_deltas_since(our_version, deltas, 64);

    // Apply deltas to local ASM
    LeafASM* asm_ptr = asm_mgr_.get_asm(leaf_id);
    if (asm_ptr) {
      for (int i = 0; i < n_deltas; i++) {
        switch (deltas[i].op_type) {
          case VCS_OP_INSERT:
            asm_ptr->insert(deltas[i].suffix, deltas[i].phys_pos);
            break;
          case VCS_OP_DELETE:
            asm_ptr->remove(deltas[i].suffix);
            break;
          case VCS_OP_UPDATE:
            // No ASM change needed for updates
            break;
        }
      }
      asm_ptr->page_version = vc->header.head_version;
    }

    version_tracker_.set_version(leaf_id, vc->header.head_version);

    // Retry lookup with refreshed ASM
    if (asm_ptr) {
      int phys_pos = asm_ptr->lookup(suffix);
      if (phys_pos >= 0) {
        uint64_t entry_offset = sizeof(LeafHeader) + sizeof(SlotArray) +
                                phys_pos * sizeof(LeafEntry);
        dsm_->read_sync(buf, leaf_addr + entry_offset, sizeof(LeafEntry));
        stats.rdma_reads++;
        stats.rdma_bytes += sizeof(LeafEntry);

        LeafEntry* entry = (LeafEntry*)buf;
        if (entry->suffix == suffix && !entry->is_tombstone()) {
          *result = entry->value;
          stats.asm_hits++;

          // Cache the result
          int tid = dsm_->getMyThreadID();
          value_cache_[tid].insert(original_key, entry->value);
          return true;
        }
      }
    }

    // Last resort: full page fetch
    return full_page_fetch_and_lookup(leaf_id, leaf_addr, suffix, original_key, result, stats);
  }

  // ─── Append to remote version chain ──────────────────────────
  void append_version_delta(GlobalAddress leaf_addr, uint8_t phys_pos,
                            uint8_t op_type, uint32_t suffix,
                            Value new_value, uint64_t new_version) {
    int tid = dsm_->getMyThreadID();
    auto& stats = stats_[tid];
    char* buf = dsm_->get_rdma_buffer();

    // Read current VC header
    GlobalAddress vc_addr = leaf_addr + kVersionChainOffset;
    dsm_->read_sync(buf, vc_addr, sizeof(VersionChainHeader));
    stats.rdma_reads++;
    stats.rdma_bytes += sizeof(VersionChainHeader);

    VersionChainHeader* hdr = (VersionChainHeader*)buf;

    // Build delta
    VersionDelta delta;
    delta.phys_pos = phys_pos;
    delta.op_type = op_type;
    delta._pad = 0;
    delta.suffix = suffix;
    delta.new_value = new_value;
    delta.new_version = new_version;

    // Write delta at head_idx position
    uint64_t delta_offset = sizeof(VersionChainHeader) + hdr->head_idx * sizeof(VersionDelta);
    memcpy(buf, &delta, sizeof(VersionDelta));
    dsm_->write_sync(buf, vc_addr + delta_offset, sizeof(VersionDelta));

    // Update header
    hdr->head_idx = (hdr->head_idx + 1) % VersionChain::kCapacity;
    hdr->num_entries = std::min((uint16_t)(hdr->num_entries + 1), (uint16_t)VersionChain::kCapacity);
    hdr->head_version = new_version;
    memcpy(buf, hdr, sizeof(VersionChainHeader));
    dsm_->write_sync(buf, vc_addr, sizeof(VersionChainHeader));
  }

  // ─── Insert into a new leaf page ─────────────────────────────
  bool insert_new_leaf(Key key, Value value) {
    int tid = dsm_->getMyThreadID();
    auto& stats = stats_[tid];

    GlobalAddress page_addr = dsm_->alloc(kTotalLeafAllocation);
    uint32_t leaf_id = next_leaf_id_++;

    // Grow metadata vectors if needed
    if (leaf_id >= leaf_addrs_.size()) {
      leaf_addrs_.resize(leaf_id + 1);
      leaf_prefix_depth_.resize(leaf_id + 1);
    }

    // Build leaf page
    LeafPage page;
    memset(&page, 0, sizeof(page));
    page.header.page_id = leaf_id;
    page.header.page_version = 1;
    page.header.num_entries = 1;

    uint32_t suffix = (uint32_t)(key & 0xFFFFFFFF);
    page.entries[0].suffix = suffix;
    page.entries[0].value = value;
    page.entries[0].version = 1;
    page.slot_array.init();
    page.slot_array.slots[0] = 0;
    page.bloom_add(suffix);
    page.fence_min_suffix = suffix;
    page.fence_max_suffix = suffix;

    // Init version chain
    VersionChain vc;
    vc.init();

    // Combined write
    char* buf = dsm_->get_rdma_buffer();
    memcpy(buf, &page, sizeof(LeafPage));
    memcpy(buf + kVersionChainOffset, &vc, sizeof(VersionChain));
    dsm_->write_sync(buf, page_addr, kTotalLeafAllocation);

    // Insert into CPT
    cpt_.insert_leaf(key, page_addr, leaf_id, key, key);
    leaf_addrs_[leaf_id] = page_addr;
    leaf_prefix_depth_[leaf_id] = 0;

    // Pre-populate ASM
    asm_mgr_.create_asm(&page);

    return true;
  }

  // ─── Split a full leaf and insert ────────────────────────────
  bool split_and_insert(uint32_t leaf_id, GlobalAddress leaf_addr,
                        Key key, Value value, int prefix_depth) {
    int tid = dsm_->getMyThreadID();
    auto& stats = stats_[tid];

    // Read full page
    char* buf = dsm_->get_rdma_buffer();
    dsm_->read_sync(buf, leaf_addr, sizeof(LeafPage));
    stats.rdma_reads++;
    stats.rdma_bytes += sizeof(LeafPage);

    LeafPage* old_page = (LeafPage*)buf;

    // Collect all entries sorted
    struct KV { uint32_t suffix; Value value; uint16_t version; };
    std::vector<KV> all_entries;
    for (int i = 0; i < old_page->header.num_entries; i++) {
      uint8_t phys = old_page->slot_array.get_physical_pos(i);
      if (phys == 0xFF) break;
      auto& e = old_page->entries[phys];
      if (!e.is_empty() && !e.is_tombstone()) {
        all_entries.push_back({e.suffix, e.value, e.version});
      }
    }

    uint32_t new_suffix = extract_suffix(key, prefix_depth);
    all_entries.push_back({new_suffix, value, 1});

    std::sort(all_entries.begin(), all_entries.end(),
              [](const KV& a, const KV& b) { return a.suffix < b.suffix; });

    int mid = all_entries.size() / 2;
    uint32_t split_suffix = all_entries[mid].suffix;

    // Rebuild old page with first half
    LeafPage new_old_page;
    memset(&new_old_page, 0, sizeof(LeafPage));
    new_old_page.header.page_id = leaf_id;
    new_old_page.header.page_version = old_page->header.page_version + 1;
    new_old_page.slot_array.init();

    for (int i = 0; i < mid; i++) {
      new_old_page.entries[i].suffix = all_entries[i].suffix;
      new_old_page.entries[i].value = all_entries[i].value;
      new_old_page.entries[i].version = all_entries[i].version;
      new_old_page.slot_array.slots[i] = i;
      new_old_page.bloom_add(all_entries[i].suffix);
    }
    new_old_page.header.num_entries = mid;
    new_old_page.update_fences();

    // Create new page with second half
    GlobalAddress new_addr = dsm_->alloc(kTotalLeafAllocation);
    uint32_t new_leaf_id = next_leaf_id_++;

    // Grow metadata vectors if needed
    if (new_leaf_id >= leaf_addrs_.size()) {
      leaf_addrs_.resize(new_leaf_id + 1);
      leaf_prefix_depth_.resize(new_leaf_id + 1);
    }

    LeafPage new_page;
    memset(&new_page, 0, sizeof(LeafPage));
    new_page.header.page_id = new_leaf_id;
    new_page.header.page_version = 1;
    new_page.slot_array.init();

    int new_count = all_entries.size() - mid;
    for (int i = 0; i < new_count; i++) {
      new_page.entries[i].suffix = all_entries[mid + i].suffix;
      new_page.entries[i].value = all_entries[mid + i].value;
      new_page.entries[i].version = all_entries[mid + i].version;
      new_page.slot_array.slots[i] = i;
      new_page.bloom_add(all_entries[mid + i].suffix);
    }
    new_page.header.num_entries = new_count;
    new_page.update_fences();

    // Set split info for forwarding
    new_old_page.header.split_flag = 1;
    new_old_page.header.split_suffix = split_suffix;
    new_old_page.header.split_new_page = new_addr.val;

    // Write old page (combined with VC)
    VersionChain vc;
    vc.init();

    char* buf2 = dsm_->get_rdma_buffer();
    memcpy(buf2, &new_old_page, sizeof(LeafPage));
    memcpy(buf2 + kVersionChainOffset, &vc, sizeof(VersionChain));
    dsm_->write_sync(buf2, leaf_addr, kTotalLeafAllocation);

    // Write new page (combined with VC)
    memcpy(buf2, &new_page, sizeof(LeafPage));
    memcpy(buf2 + kVersionChainOffset, &vc, sizeof(VersionChain));
    dsm_->write_sync(buf2, new_addr, kTotalLeafAllocation);

    // Update CPT
    TrieNode* old_node = cpt_.find_leaf_by_id(leaf_id);
    Key old_max = 0;
    if (old_node) {
      old_max = old_node->max_key;
      Key split_key = reconstruct_key(old_node->min_key, split_suffix, prefix_depth);
      old_node->max_key = split_key - 1;
      cpt_.insert_leaf(split_key, new_addr, new_leaf_id, split_key, old_max);
    }

    // Update local metadata
    leaf_addrs_[new_leaf_id] = new_addr;
    leaf_prefix_depth_[new_leaf_id] = prefix_depth;

    // Rebuild ASMs for both pages
    asm_mgr_.remove_asm(leaf_id);
    asm_mgr_.create_asm(&new_old_page);
    asm_mgr_.create_asm(&new_page);

    // Invalidate value cache for the inserted key
    value_cache_[tid].invalidate(key);

    return true;
  }
};

}  // namespace apex

#endif /* __APEX_INDEX_H__ */
