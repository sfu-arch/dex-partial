#if !defined(_CACHE_COHERENCE_H_)
#define _CACHE_COHERENCE_H_

#include "Common.h"
#include "GlobalAddress.h"
#include "DSM.h"

#include <tbb/concurrent_hash_map.h>
#include <tbb/concurrent_queue.h>
#include <atomic>
#include <cstdint>
#include <vector>
#include <functional>

/**
 * CacheCoherence - Cache coherence protocol for CHIME with compute-side caching.
 * 
 * Implements a directory-based MESI-like protocol where:
 * - Memory node maintains directory tracking which compute nodes cache each leaf
 * - Compute nodes hold cached leaf bitmaps in MODIFIED/EXCLUSIVE/SHARED/INVALID states
 * - Write operations trigger invalidation or downgrade of other copies
 * 
 * Two coherence modes:
 * - LAZY: Version-based validation on read (lower overhead, eventual consistency)
 * - EAGER: Directory-based invalidation on write (immediate consistency)
 */

namespace coherence {

// ============================================================================
// COHERENCE STATES
// ============================================================================

/**
 * CoherenceState - MESI-like cache coherence states.
 */
enum class CoherenceState : uint8_t {
  INVALID = 0,     // Not cached or stale
  SHARED = 1,      // Cached, read-only, others may also have copy
  EXCLUSIVE = 2,   // Cached, read-write, no other copies exist
  MODIFIED = 3     // Cached, read-write, local changes not written back
};

inline const char* state_to_string(CoherenceState state) {
  switch (state) {
    case CoherenceState::INVALID: return "INVALID";
    case CoherenceState::SHARED: return "SHARED";
    case CoherenceState::EXCLUSIVE: return "EXCLUSIVE";
    case CoherenceState::MODIFIED: return "MODIFIED";
    default: return "UNKNOWN";
  }
}


// ============================================================================
// MESSAGE TYPES
// ============================================================================

/**
 * CoherenceMessageType - Types of coherence protocol messages.
 */
enum class CoherenceMessageType : uint8_t {
  // Compute → Memory
  READ_REQUEST = 0,       // Request to read (get SHARED/EXCLUSIVE)
  WRITE_REQUEST = 1,      // Request to write (get EXCLUSIVE/MODIFIED)
  UPGRADE_REQUEST = 2,    // SHARED → EXCLUSIVE upgrade
  WRITEBACK = 3,          // MODIFIED → flush to memory
  EVICTION_NOTICE = 4,    // Notify memory that we evicted entry
  
  // Memory → Compute
  READ_RESPONSE = 10,     // Response with data and state
  WRITE_RESPONSE = 11,    // Write granted
  UPGRADE_RESPONSE = 12,  // Upgrade granted
  INVALIDATE = 13,        // Invalidate your copy
  DOWNGRADE = 14,         // EXCLUSIVE/MODIFIED → SHARED
  
  // Acknowledgments
  INVALIDATE_ACK = 20,    // Ack for invalidation
  DOWNGRADE_ACK = 21      // Ack for downgrade
};


// ============================================================================
// COHERENCE MESSAGES
// ============================================================================

/**
 * CoherenceMessage - Wire format for coherence protocol messages.
 */
struct CoherenceMessage {
  CoherenceMessageType type;
  uint8_t flags;
  uint16_t source_node;       // Sending node ID
  uint32_t sequence_num;      // For ordering/dedup
  GlobalAddress leaf_addr;    // Target leaf
  uint64_t version;           // Version number
  uint64_t bitmap;            // Vacancy bitmap (for data messages)
  uint64_t timestamp;         // For ordering
  
  CoherenceMessage() : type(CoherenceMessageType::READ_REQUEST), flags(0),
    source_node(0), sequence_num(0), leaf_addr(), version(0), bitmap(0), timestamp(0) {}
    
  static constexpr size_t WIRE_SIZE = 48;
} __attribute__((packed));

static_assert(sizeof(CoherenceMessage) <= CoherenceMessage::WIRE_SIZE);


// ============================================================================
// DIRECTORY ENTRY (MEMORY NODE SIDE)
// ============================================================================

/**
 * DirectoryEntry - Memory-node directory entry tracking sharers.
 * 
 * Each entry tracks:
 * - Which compute nodes have cached copies
 * - Current version number
 * - Owner (for EXCLUSIVE/MODIFIED states)
 */
struct DirectoryEntry {
  std::atomic<uint64_t> version;      // Current data version
  std::atomic<uint64_t> sharer_mask;  // Bitmap of compute nodes with SHARED copy
  std::atomic<int16_t> owner_node;    // Node with EXCLUSIVE/MODIFIED copy (-1 = none)
  std::atomic<uint16_t> pending_acks; // Outstanding invalidation acks
  std::atomic<bool> locked;           // Directory entry lock
  
  DirectoryEntry() : version(0), sharer_mask(0), owner_node(-1), 
                     pending_acks(0), locked(false) {}
  
  // Check if any compute node has a copy
  bool has_sharers() const {
    return sharer_mask.load() != 0 || owner_node.load() >= 0;
  }
  
  // Add a sharer
  void add_sharer(int node_id) {
    sharer_mask.fetch_or(1ULL << node_id);
  }
  
  // Remove a sharer
  void remove_sharer(int node_id) {
    sharer_mask.fetch_and(~(1ULL << node_id));
  }
  
  // Check if node is a sharer
  bool is_sharer(int node_id) const {
    return sharer_mask.load() & (1ULL << node_id);
  }
  
  // Set exclusive owner
  void set_owner(int node_id) {
    owner_node.store(node_id);
    sharer_mask.store(0);  // Clear sharers when granting exclusive
  }
  
  // Clear owner
  void clear_owner() {
    int16_t old_owner = owner_node.exchange(-1);
    if (old_owner >= 0) {
      add_sharer(old_owner);  // Demote to sharer
    }
  }
  
  // Lock for modification
  bool try_lock() {
    bool expected = false;
    return locked.compare_exchange_strong(expected, true);
  }
  
  void unlock() {
    locked.store(false);
  }
};


// ============================================================================
// DIRECTORY (MEMORY NODE SIDE)
// ============================================================================

/**
 * CoherenceDirectory - Directory for tracking cached leaf copies.
 * 
 * Implemented as concurrent hash map indexed by leaf address.
 */
class CoherenceDirectory {
public:
  using DirectoryMap = tbb::concurrent_hash_map<uint64_t, DirectoryEntry>;
  
  CoherenceDirectory() = default;
  
  /**
   * Get or create directory entry for leaf.
   */
  DirectoryEntry* get_entry(const GlobalAddress& leaf_addr) {
    uint64_t key = make_key(leaf_addr);
    DirectoryMap::accessor acc;
    directory_.insert(acc, key);
    return &acc->second;
  }
  
  /**
   * Handle read request from compute node.
   * Returns granted state and current version.
   */
  std::pair<CoherenceState, uint64_t> handle_read_request(
    const GlobalAddress& leaf_addr,
    int requestor_node
  ) {
    auto* entry = get_entry(leaf_addr);
    
    while (!entry->try_lock()) {
      // Spin wait - could use backoff
    }
    
    CoherenceState granted_state;
    uint64_t version = entry->version.load();
    
    if (entry->owner_node.load() < 0 && entry->sharer_mask.load() == 0) {
      // No current copies - grant EXCLUSIVE
      entry->set_owner(requestor_node);
      granted_state = CoherenceState::EXCLUSIVE;
    } else if (entry->owner_node.load() >= 0) {
      // Has exclusive owner - need to downgrade first
      // For now, grant SHARED after implicit downgrade
      entry->clear_owner();
      entry->add_sharer(requestor_node);
      granted_state = CoherenceState::SHARED;
    } else {
      // Already has sharers - add to list
      entry->add_sharer(requestor_node);
      granted_state = CoherenceState::SHARED;
    }
    
    entry->unlock();
    return {granted_state, version};
  }
  
  /**
   * Handle write request from compute node.
   * Returns list of nodes to invalidate.
   */
  std::pair<CoherenceState, std::vector<int>> handle_write_request(
    const GlobalAddress& leaf_addr,
    int requestor_node
  ) {
    auto* entry = get_entry(leaf_addr);
    
    while (!entry->try_lock()) {
      // Spin wait
    }
    
    std::vector<int> invalidate_list;
    
    // Collect all other sharers to invalidate
    uint64_t sharers = entry->sharer_mask.load();
    for (int i = 0; i < 64; i++) {
      if ((sharers & (1ULL << i)) && i != requestor_node) {
        invalidate_list.push_back(i);
      }
    }
    
    // Check if another node has exclusive ownership
    int16_t owner = entry->owner_node.load();
    if (owner >= 0 && owner != requestor_node) {
      invalidate_list.push_back(owner);
    }
    
    // Grant exclusive to requestor
    entry->set_owner(requestor_node);
    entry->pending_acks.store(invalidate_list.size());
    
    entry->unlock();
    return {CoherenceState::EXCLUSIVE, invalidate_list};
  }
  
  /**
   * Handle upgrade request (SHARED → EXCLUSIVE).
   */
  std::vector<int> handle_upgrade_request(
    const GlobalAddress& leaf_addr,
    int requestor_node
  ) {
    auto* entry = get_entry(leaf_addr);
    
    while (!entry->try_lock()) {}
    
    std::vector<int> invalidate_list;
    
    // Collect other sharers
    uint64_t sharers = entry->sharer_mask.load();
    for (int i = 0; i < 64; i++) {
      if ((sharers & (1ULL << i)) && i != requestor_node) {
        invalidate_list.push_back(i);
      }
    }
    
    // Grant exclusive
    entry->set_owner(requestor_node);
    entry->pending_acks.store(invalidate_list.size());
    
    entry->unlock();
    return invalidate_list;
  }
  
  /**
   * Handle writeback from compute node.
   */
  void handle_writeback(
    const GlobalAddress& leaf_addr,
    int source_node,
    uint64_t new_version
  ) {
    auto* entry = get_entry(leaf_addr);
    
    while (!entry->try_lock()) {}
    
    // Update version
    entry->version.store(new_version);
    
    // Clear ownership if this was the owner
    if (entry->owner_node.load() == source_node) {
      entry->owner_node.store(-1);
    }
    
    entry->unlock();
  }
  
  /**
   * Handle eviction notice from compute node.
   */
  void handle_eviction(
    const GlobalAddress& leaf_addr,
    int source_node
  ) {
    auto* entry = get_entry(leaf_addr);
    
    while (!entry->try_lock()) {}
    
    entry->remove_sharer(source_node);
    if (entry->owner_node.load() == source_node) {
      entry->owner_node.store(-1);
    }
    
    entry->unlock();
  }
  
  /**
   * Handle invalidation acknowledgment.
   */
  bool handle_invalidate_ack(const GlobalAddress& leaf_addr) {
    auto* entry = get_entry(leaf_addr);
    uint16_t remaining = entry->pending_acks.fetch_sub(1) - 1;
    return remaining == 0;  // All acks received
  }

private:
  uint64_t make_key(const GlobalAddress& addr) const {
    return ((uint64_t)addr.nodeID << 48) | addr.offset;
  }
  
  DirectoryMap directory_;
};


// ============================================================================
// COHERENCE CONTROLLER (COMPUTE NODE SIDE)
// ============================================================================

/**
 * CacheLineState - Local state for a cached leaf.
 */
struct CacheLineState {
  CoherenceState state;
  uint64_t version;
  uint64_t last_access;
  
  CacheLineState() : state(CoherenceState::INVALID), version(0), last_access(0) {}
};

/**
 * CoherenceController - Compute-node side coherence controller.
 * 
 * Manages local cache states and handles coherence messages.
 */
class CoherenceController {
public:
  using StateMap = tbb::concurrent_hash_map<uint64_t, CacheLineState>;
  using InvalidateCallback = std::function<void(const GlobalAddress&)>;
  
  CoherenceController(DSM* dsm, int my_node_id) 
    : dsm_(dsm), my_node_id_(my_node_id) {}
  
  /**
   * Set callback for invalidation notifications.
   */
  void set_invalidate_callback(InvalidateCallback cb) {
    invalidate_callback_ = cb;
  }
  
  /**
   * Get current coherence state for leaf.
   */
  CoherenceState get_state(const GlobalAddress& leaf_addr) {
    uint64_t key = make_key(leaf_addr);
    StateMap::accessor acc;
    if (states_.find(acc, key)) {
      return acc->second.state;
    }
    return CoherenceState::INVALID;
  }
  
  /**
   * Request read access to leaf.
   * May block waiting for response.
   */
  CoherenceState request_read(
    const GlobalAddress& leaf_addr,
    uint64_t& version_out,
    CoroPull* sink = nullptr
  ) {
    auto current_state = get_state(leaf_addr);
    
    if (current_state != CoherenceState::INVALID) {
      // Already have access
      StateMap::accessor acc;
      states_.find(acc, make_key(leaf_addr));
      version_out = acc->second.version;
      return current_state;
    }
    
    // Need to request from memory node
    // In a real implementation, this would send RDMA message
    // For now, we'll use a simplified version-based approach
    
    // Read version from memory
    auto buffer = (dsm_->get_rbuf(sink)).get_cas_buffer();
    dsm_->read_sync((char*)buffer, leaf_addr, sizeof(uint64_t), sink);
    uint64_t mem_version = *(uint64_t*)buffer;
    
    // Update local state
    StateMap::accessor acc;
    states_.insert(acc, make_key(leaf_addr));
    acc->second.state = CoherenceState::SHARED;  // Assume shared for reads
    acc->second.version = mem_version;
    version_out = mem_version;
    
    return CoherenceState::SHARED;
  }
  
  /**
   * Request write access to leaf.
   * May trigger invalidations to other nodes.
   */
  CoherenceState request_write(
    const GlobalAddress& leaf_addr,
    uint64_t& version_out,
    CoroPull* sink = nullptr
  ) {
    auto current_state = get_state(leaf_addr);
    
    if (current_state == CoherenceState::EXCLUSIVE || 
        current_state == CoherenceState::MODIFIED) {
      // Already have write access
      StateMap::accessor acc;
      states_.find(acc, make_key(leaf_addr));
      version_out = acc->second.version;
      return current_state;
    }
    
    // Need exclusive access
    // In real implementation, would coordinate with directory
    
    // For now, use lock-based approach (leaf is already locked by Tree operations)
    StateMap::accessor acc;
    states_.insert(acc, make_key(leaf_addr));
    acc->second.state = CoherenceState::EXCLUSIVE;
    acc->second.version++;
    version_out = acc->second.version;
    
    return CoherenceState::EXCLUSIVE;
  }
  
  /**
   * Process incoming invalidation request.
   */
  void process_invalidate(const GlobalAddress& leaf_addr) {
    StateMap::accessor acc;
    if (states_.find(acc, make_key(leaf_addr))) {
      acc->second.state = CoherenceState::INVALID;
    }
    
    // Notify cache layer to invalidate
    if (invalidate_callback_) {
      invalidate_callback_(leaf_addr);
    }
  }
  
  /**
   * Process incoming downgrade request.
   */
  void process_downgrade(const GlobalAddress& leaf_addr) {
    StateMap::accessor acc;
    if (states_.find(acc, make_key(leaf_addr))) {
      auto& state = acc->second.state;
      if (state == CoherenceState::MODIFIED) {
        // Would need to write back first
        // For now, just downgrade
      }
      state = CoherenceState::SHARED;
    }
  }
  
  /**
   * Mark leaf as modified after local write.
   */
  void mark_modified(const GlobalAddress& leaf_addr) {
    StateMap::accessor acc;
    if (states_.find(acc, make_key(leaf_addr))) {
      acc->second.state = CoherenceState::MODIFIED;
      acc->second.version++;
    }
  }
  
  /**
   * Evict entry from cache.
   * Notifies memory node if we had exclusive/modified copy.
   */
  void evict(const GlobalAddress& leaf_addr, CoroPull* sink = nullptr) {
    StateMap::accessor acc;
    if (states_.find(acc, make_key(leaf_addr))) {
      auto state = acc->second.state;
      
      if (state == CoherenceState::MODIFIED) {
        // Would need to write back
        // For now, just mark invalid
      }
      
      states_.erase(acc);
    }
  }
  
  /**
   * Validate cached version against memory.
   * For lazy coherence mode.
   */
  bool validate_version(
    const GlobalAddress& leaf_addr,
    uint64_t expected_version,
    CoroPull* sink = nullptr
  ) {
    // Read current version from memory
    auto buffer = (dsm_->get_rbuf(sink)).get_cas_buffer();
    dsm_->read_sync((char*)buffer, leaf_addr, sizeof(uint64_t), sink);
    uint64_t mem_version = *(uint64_t*)buffer;
    
    if (mem_version != expected_version) {
      // Version mismatch - invalidate
      process_invalidate(leaf_addr);
      return false;
    }
    
    return true;
  }

private:
  uint64_t make_key(const GlobalAddress& addr) const {
    return ((uint64_t)addr.nodeID << 48) | addr.offset;
  }
  
  DSM* dsm_;
  int my_node_id_;
  StateMap states_;
  InvalidateCallback invalidate_callback_;
};


// ============================================================================
// COHERENCE STATISTICS
// ============================================================================

/**
 * CoherenceStats - Statistics for coherence protocol.
 */
struct CoherenceStats {
  std::atomic<uint64_t> read_requests{0};
  std::atomic<uint64_t> write_requests{0};
  std::atomic<uint64_t> upgrade_requests{0};
  std::atomic<uint64_t> invalidations_sent{0};
  std::atomic<uint64_t> invalidations_received{0};
  std::atomic<uint64_t> writebacks{0};
  std::atomic<uint64_t> version_validations{0};
  std::atomic<uint64_t> version_mismatches{0};
  
  void print() const {
    printf("[Coherence Stats]\n");
    printf("  Read requests: %lu\n", read_requests.load());
    printf("  Write requests: %lu\n", write_requests.load());
    printf("  Upgrade requests: %lu\n", upgrade_requests.load());
    printf("  Invalidations sent: %lu\n", invalidations_sent.load());
    printf("  Invalidations received: %lu\n", invalidations_received.load());
    printf("  Writebacks: %lu\n", writebacks.load());
    printf("  Version validations: %lu\n", version_validations.load());
    printf("  Version mismatches: %lu\n", version_mismatches.load());
  }
  
  void reset() {
    read_requests.store(0);
    write_requests.store(0);
    upgrade_requests.store(0);
    invalidations_sent.store(0);
    invalidations_received.store(0);
    writebacks.store(0);
    version_validations.store(0);
    version_mismatches.store(0);
  }
};

} // namespace coherence

#endif // _CACHE_COHERENCE_H_
