/**
 * RDMA Compatibility Layer for CHIME
 * 
 * This header provides compatibility wrappers for experimental RDMA verbs
 * (ibv_exp_*) that may not be available on all systems. It enables CHIME
 * to build and run on systems without MLNX_OFED by falling back to:
 * 
 * 1. Standard libibverbs API instead of experimental verbs
 * 2. RC (Reliably Connected) transport instead of DC (Dynamically Connected)
 * 3. Software emulation for masked atomics and device memory
 * 
 * Usage:
 *   - If USE_MLNX_OFED is defined, uses native experimental verbs
 *   - Otherwise, uses compatibility wrappers
 */

#ifndef _RDMA_COMPAT_H__
#define _RDMA_COMPAT_H__

#include <infiniband/verbs.h>
#include <cstring>
#include <cstdint>

// ============================================================================
// Feature Detection
// ============================================================================

// Build-time override takes priority (set by CMakeLists.txt)
#ifdef FORCE_NO_EXP_VERBS
  #define HAS_EXP_VERBS 0
#elif defined(FORCE_USE_EXP_VERBS)
  #define HAS_EXP_VERBS 1
#elif defined(__has_include)
  #if __has_include(<infiniband/verbs_exp.h>)
    #define HAS_EXP_VERBS 1
  #else
    #define HAS_EXP_VERBS 0
  #endif
#else
  // Assume not available if we can't check
  #define HAS_EXP_VERBS 0
#endif

// ============================================================================
// Experimental Verbs Header (if available)
// ============================================================================

#if HAS_EXP_VERBS
  #include <infiniband/verbs_exp.h>
  #define USE_DC_TRANSPORT 1
  #define USE_DEVICE_MEMORY 1
  #define USE_MASKED_ATOMICS 1
#else
  #define USE_DC_TRANSPORT 0
  #define USE_DEVICE_MEMORY 0
  #define USE_MASKED_ATOMICS 0
#endif

// ============================================================================
// Compatibility Definitions (when experimental verbs not available)
// ============================================================================

#if !HAS_EXP_VERBS

// --- DC Transport Types (fallback to RC) ---

// DC QP type - map to RC when DC not available
#ifndef IBV_EXP_QPT_DC_INI
  #define IBV_EXP_QPT_DC_INI IBV_QPT_RC
#endif

// DCT (DC Target) - stub structure
struct ibv_exp_dct {
  uint32_t dct_num;  // Simulated DCT number
  void* context;     // Placeholder
};

struct ibv_exp_dct_init_attr {
  struct ibv_pd* pd;
  struct ibv_cq* cq;
  struct ibv_srq* srq;
  uint64_t dc_key;
  uint8_t port;
  uint32_t access_flags;
  uint8_t min_rnr_timer;
  uint8_t tclass;
  uint32_t flow_label;
  enum ibv_mtu mtu;
  uint16_t pkey_index;
  uint8_t hop_limit;
  uint32_t create_flags;
  uint32_t inline_size;
};

// --- Extended QP Init Attributes ---

struct ibv_exp_qp_init_attr {
  // Standard fields
  void* qp_context;
  struct ibv_cq* send_cq;
  struct ibv_cq* recv_cq;
  struct ibv_srq* srq;
  struct ibv_qp_cap cap;
  enum ibv_qp_type qp_type;
  int sq_sig_all;
  
  // Extended fields
  struct ibv_pd* pd;
  uint32_t comp_mask;
  uint32_t max_atomic_arg;
  uint32_t create_flags;
};

// Comp mask flags
#define IBV_EXP_QP_INIT_ATTR_PD           (1 << 0)
#define IBV_EXP_QP_INIT_ATTR_CREATE_FLAGS (1 << 1)
#define IBV_EXP_QP_INIT_ATTR_ATOMICS_ARG  (1 << 2)

// --- Extended Send Work Request ---

struct ibv_exp_send_wr {
  uint64_t wr_id;
  struct ibv_exp_send_wr* next;
  struct ibv_sge* sg_list;
  int num_sge;
  enum ibv_wr_opcode exp_opcode;
  int exp_send_flags;
  uint32_t imm_data;
  
  union {
    struct {
      uint64_t remote_addr;
      uint32_t rkey;
    } rdma;
    struct {
      uint64_t remote_addr;
      uint64_t compare_add;
      uint64_t swap;
      uint32_t rkey;
    } atomic;
  } wr;
  
  // Extended atomic operations
  struct {
    struct {
      int log_arg_sz;
      uint64_t remote_addr;
      uint32_t rkey;
      struct {
        struct {
          union {
            struct {
              uint64_t add_val;
              uint64_t field_boundary;
            } fetch_add;
            struct {
              uint64_t compare_val;
              uint64_t swap_val;
              uint64_t compare_mask;
              uint64_t swap_mask;
            } cmp_swap;
          } op;
        } inline_data;
      } wr_data;
    } masked_atomics;
    struct {
      struct ibv_ah* ah;
      uint32_t dct_number;
      uint64_t dc_key;
    } dc;
  } ext_op;
};

// Extended opcodes - cast to ibv_wr_opcode to avoid conversion errors
#define IBV_EXP_WR_EXT_MASKED_ATOMIC_CMP_AND_SWP  ((ibv_wr_opcode)100)
#define IBV_EXP_WR_EXT_MASKED_ATOMIC_FETCH_AND_ADD ((ibv_wr_opcode)101)
#define IBV_EXP_WR_RDMA_READ  ((ibv_wr_opcode)IBV_WR_RDMA_READ)
#define IBV_EXP_WR_RDMA_WRITE ((ibv_wr_opcode)IBV_WR_RDMA_WRITE)

// Extended send flags
#define IBV_EXP_SEND_SIGNALED       IBV_SEND_SIGNALED
#define IBV_EXP_SEND_EXT_ATOMIC_INLINE 0x1000

// --- Device Memory Types ---

struct ibv_exp_dm {
  void* dm_ptr;       // Simulated with regular memory
  size_t length;
};

struct ibv_exp_alloc_dm_attr {
  size_t length;
  uint32_t comp_mask;
};

struct ibv_exp_reg_mr_in {
  struct ibv_pd* pd;
  void* addr;
  size_t length;
  int exp_access;
  uint32_t create_flags;
  uint32_t comp_mask;
  struct ibv_exp_dm* dm;
};

#define IBV_EXP_REG_MR_DM (1 << 0)

struct ibv_exp_memcpy_dm_attr {
  void* host_addr;
  size_t length;
  size_t dm_offset;
  int memcpy_dir;
};

#define IBV_EXP_DM_CPY_TO_DEVICE   1
#define IBV_EXP_DM_CPY_FROM_DEVICE 2

// ============================================================================
// Compatibility Function Implementations
// ============================================================================

/**
 * Create QP using standard or experimental API
 */
static inline struct ibv_qp* compat_create_qp(
    struct ibv_context* ctx,
    struct ibv_exp_qp_init_attr* exp_attr)
{
  // Convert to standard ibv_qp_init_attr
  struct ibv_qp_init_attr attr;
  memset(&attr, 0, sizeof(attr));
  
  attr.qp_context = exp_attr->qp_context;
  attr.send_cq = exp_attr->send_cq;
  attr.recv_cq = exp_attr->recv_cq;
  attr.srq = exp_attr->srq;
  attr.cap = exp_attr->cap;
  attr.qp_type = exp_attr->qp_type;
  attr.sq_sig_all = exp_attr->sq_sig_all;
  
  // DC not supported - use RC
  if (attr.qp_type == IBV_EXP_QPT_DC_INI) {
    attr.qp_type = IBV_QPT_RC;
  }
  
  return ibv_create_qp(exp_attr->pd, &attr);
}

/**
 * Create DC Target (stub - returns fake DCT for RC fallback)
 */
static inline struct ibv_exp_dct* compat_create_dct(
    struct ibv_context* ctx,
    struct ibv_exp_dct_init_attr* attr)
{
  // DC not available - create a stub DCT
  // The actual connection will use RC QPs instead
  struct ibv_exp_dct* dct = (struct ibv_exp_dct*)malloc(sizeof(struct ibv_exp_dct));
  if (dct) {
    dct->dct_num = 0;  // Will be replaced with RC QP number
    dct->context = ctx;
  }
  return dct;
}

/**
 * Allocate Device Memory (fallback to host memory)
 */
static inline struct ibv_exp_dm* compat_alloc_dm(
    struct ibv_context* ctx,
    struct ibv_exp_alloc_dm_attr* attr)
{
  struct ibv_exp_dm* dm = (struct ibv_exp_dm*)malloc(sizeof(struct ibv_exp_dm));
  if (dm) {
    // Allocate regular host memory as fallback
    dm->dm_ptr = aligned_alloc(64, attr->length);
    dm->length = attr->length;
    if (!dm->dm_ptr) {
      free(dm);
      return nullptr;
    }
    memset(dm->dm_ptr, 0, attr->length);
  }
  return dm;
}

/**
 * Register Device Memory (fallback to regular MR)
 */
static inline struct ibv_mr* compat_reg_mr_dm(struct ibv_exp_reg_mr_in* in)
{
  if (in->dm) {
    // Register the simulated device memory
    return ibv_reg_mr(in->pd, in->dm->dm_ptr, in->length, in->exp_access);
  }
  return ibv_reg_mr(in->pd, in->addr, in->length, in->exp_access);
}

/**
 * Copy to/from Device Memory (fallback to memcpy)
 */
static inline int compat_memcpy_dm(
    struct ibv_exp_dm* dm,
    struct ibv_exp_memcpy_dm_attr* attr)
{
  if (!dm || !dm->dm_ptr) return -1;
  
  char* dm_base = (char*)dm->dm_ptr + attr->dm_offset;
  
  if (attr->memcpy_dir == IBV_EXP_DM_CPY_TO_DEVICE) {
    memcpy(dm_base, attr->host_addr, attr->length);
  } else {
    memcpy(attr->host_addr, dm_base, attr->length);
  }
  return 0;
}

/**
 * Post extended send (fallback for masked atomics)
 * 
 * NOTE: Masked atomics are NOT supported in standard verbs.
 * This function provides a best-effort fallback using regular CAS,
 * which may have different semantics for partial updates.
 */
static inline int compat_exp_post_send(
    struct ibv_qp* qp,
    struct ibv_exp_send_wr* wr,
    struct ibv_exp_send_wr** bad_wr)
{
  // Convert experimental WR to standard WR
  struct ibv_send_wr std_wr;
  struct ibv_send_wr* std_bad_wr;
  
  memset(&std_wr, 0, sizeof(std_wr));
  std_wr.wr_id = wr->wr_id;
  std_wr.sg_list = wr->sg_list;
  std_wr.num_sge = wr->num_sge;
  std_wr.send_flags = wr->exp_send_flags & 0xFF;  // Standard flags only
  
  switch (wr->exp_opcode) {
    case IBV_EXP_WR_EXT_MASKED_ATOMIC_CMP_AND_SWP:
      // Fall back to regular CAS (ignores masks)
      std_wr.opcode = IBV_WR_ATOMIC_CMP_AND_SWP;
      std_wr.wr.atomic.remote_addr = wr->ext_op.masked_atomics.remote_addr;
      std_wr.wr.atomic.rkey = wr->ext_op.masked_atomics.rkey;
      std_wr.wr.atomic.compare_add = 
          wr->ext_op.masked_atomics.wr_data.inline_data.op.cmp_swap.compare_val;
      std_wr.wr.atomic.swap = 
          wr->ext_op.masked_atomics.wr_data.inline_data.op.cmp_swap.swap_val;
      break;
      
    case IBV_EXP_WR_EXT_MASKED_ATOMIC_FETCH_AND_ADD:
      // Fall back to regular FAA (ignores boundary)
      std_wr.opcode = IBV_WR_ATOMIC_FETCH_AND_ADD;
      std_wr.wr.atomic.remote_addr = wr->ext_op.masked_atomics.remote_addr;
      std_wr.wr.atomic.rkey = wr->ext_op.masked_atomics.rkey;
      std_wr.wr.atomic.compare_add = 
          wr->ext_op.masked_atomics.wr_data.inline_data.op.fetch_add.add_val;
      break;
      
    default:
      // Unknown opcode
      *bad_wr = wr;
      return -1;
  }
  
  int ret = ibv_post_send(qp, &std_wr, &std_bad_wr);
  if (ret != 0) {
    *bad_wr = wr;
  }
  return ret;
}

// ============================================================================
// API Mapping Macros
// ============================================================================

// Map experimental functions to compatibility wrappers
#define ibv_exp_create_qp(ctx, attr) compat_create_qp(ctx, attr)
#define ibv_exp_create_dct(ctx, attr) compat_create_dct(ctx, attr)
#define ibv_exp_alloc_dm(ctx, attr) compat_alloc_dm(ctx, attr)
#define ibv_exp_reg_mr(in) compat_reg_mr_dm(in)
#define ibv_exp_memcpy_dm(dm, attr) compat_memcpy_dm(dm, attr)
#define ibv_exp_post_send(qp, wr, bad) compat_exp_post_send(qp, wr, bad)

#endif // !HAS_EXP_VERBS

// ============================================================================
// Runtime Feature Detection
// ============================================================================

/**
 * Check if DC transport is available at runtime
 */
static inline bool rdma_has_dc_transport() {
#if USE_DC_TRANSPORT
  return true;
#else
  return false;
#endif
}

/**
 * Check if device memory is available at runtime
 */
static inline bool rdma_has_device_memory() {
#if USE_DEVICE_MEMORY
  return true;
#else
  return false;
#endif
}

/**
 * Check if masked atomics are available at runtime
 */
static inline bool rdma_has_masked_atomics() {
#if USE_MASKED_ATOMICS
  return true;
#else
  return false;
#endif
}

/**
 * Print RDMA capability summary
 */
static inline void rdma_print_capabilities() {
  printf("RDMA Compatibility Layer Status:\n");
  printf("  Experimental Verbs: %s\n", HAS_EXP_VERBS ? "Available" : "Emulated");
  printf("  DC Transport:       %s\n", rdma_has_dc_transport() ? "Available" : "Using RC fallback");
  printf("  Device Memory:      %s\n", rdma_has_device_memory() ? "Available" : "Using host memory");
  printf("  Masked Atomics:     %s\n", rdma_has_masked_atomics() ? "Available" : "Using standard atomics");
}

// ============================================================================
// Additional Compatibility for DC State Transitions
// ============================================================================

#if !HAS_EXP_VERBS

// Extended QP attributes for DC
struct ibv_exp_qp_attr {
  enum ibv_qp_state qp_state;
  enum ibv_qp_state cur_qp_state;
  enum ibv_mtu path_mtu;
  enum ibv_mig_state path_mig_state;
  uint32_t qkey;
  uint32_t rq_psn;
  uint32_t sq_psn;
  uint32_t dest_qp_num;
  int qp_access_flags;
  struct ibv_qp_cap cap;
  struct ibv_ah_attr ah_attr;
  struct ibv_ah_attr alt_ah_attr;
  uint16_t pkey_index;
  uint16_t alt_pkey_index;
  uint8_t en_sqd_async_notify;
  uint8_t sq_draining;
  uint8_t max_rd_atomic;
  uint8_t max_dest_rd_atomic;
  uint8_t min_rnr_timer;
  uint8_t port_num;
  uint8_t timeout;
  uint8_t retry_cnt;
  uint8_t rnr_retry;
  uint8_t alt_port_num;
  uint8_t alt_timeout;
  // DC-specific
  uint64_t dct_key;
};

// Extended QP attr masks
#define IBV_EXP_QP_STATE             IBV_QP_STATE
#define IBV_EXP_QP_PKEY_INDEX        IBV_QP_PKEY_INDEX
#define IBV_EXP_QP_PORT              IBV_QP_PORT
#define IBV_EXP_QP_DC_KEY            (1 << 20)
#define IBV_EXP_QP_PATH_MTU          IBV_QP_PATH_MTU
#define IBV_EXP_QP_AV                IBV_QP_AV
#define IBV_EXP_QP_TIMEOUT           IBV_QP_TIMEOUT
#define IBV_EXP_QP_RETRY_CNT         IBV_QP_RETRY_CNT
#define IBV_EXP_QP_RNR_RETRY         IBV_QP_RNR_RETRY
#define IBV_EXP_QP_MAX_QP_RD_ATOMIC  IBV_QP_MAX_QP_RD_ATOMIC

/**
 * Modify QP using extended attributes (compatibility wrapper)
 * Falls back to standard ibv_modify_qp, ignoring DC-specific options
 */
static inline int compat_exp_modify_qp(
    struct ibv_qp* qp,
    struct ibv_exp_qp_attr* exp_attr,
    uint64_t exp_attr_mask)
{
  // Convert to standard attr mask (strip DC-specific bits)
  int std_mask = (int)(exp_attr_mask & 0xFFFF);
  
  // Convert to standard attributes
  struct ibv_qp_attr attr;
  memset(&attr, 0, sizeof(attr));
  
  attr.qp_state = exp_attr->qp_state;
  attr.cur_qp_state = exp_attr->cur_qp_state;
  attr.path_mtu = exp_attr->path_mtu;
  attr.path_mig_state = exp_attr->path_mig_state;
  attr.qkey = exp_attr->qkey;
  attr.rq_psn = exp_attr->rq_psn;
  attr.sq_psn = exp_attr->sq_psn;
  attr.dest_qp_num = exp_attr->dest_qp_num;
  attr.qp_access_flags = exp_attr->qp_access_flags;
  attr.cap = exp_attr->cap;
  attr.ah_attr = exp_attr->ah_attr;
  attr.alt_ah_attr = exp_attr->alt_ah_attr;
  attr.pkey_index = exp_attr->pkey_index;
  attr.alt_pkey_index = exp_attr->alt_pkey_index;
  attr.en_sqd_async_notify = exp_attr->en_sqd_async_notify;
  attr.sq_draining = exp_attr->sq_draining;
  attr.max_rd_atomic = exp_attr->max_rd_atomic;
  attr.max_dest_rd_atomic = exp_attr->max_dest_rd_atomic;
  attr.min_rnr_timer = exp_attr->min_rnr_timer;
  attr.port_num = exp_attr->port_num;
  attr.timeout = exp_attr->timeout;
  attr.retry_cnt = exp_attr->retry_cnt;
  attr.rnr_retry = exp_attr->rnr_retry;
  attr.alt_port_num = exp_attr->alt_port_num;
  attr.alt_timeout = exp_attr->alt_timeout;
  
  return ibv_modify_qp(qp, &attr, std_mask);
}

#define ibv_exp_modify_qp(qp, attr, mask) compat_exp_modify_qp(qp, attr, mask)

// --- Device Query Compatibility ---

struct ibv_exp_device_attr {
  uint64_t comp_mask;
  uint64_t max_dm_size;
  // Add other fields as needed
};

#define IBV_EXP_DEVICE_ATTR_UMR        (1 << 0)
#define IBV_EXP_DEVICE_ATTR_MAX_DM_SIZE (1 << 1)

/**
 * Query device attributes (compatibility wrapper)
 * Returns 0 but device memory size will be 0 (not supported)
 */
static inline int compat_exp_query_device(
    struct ibv_context* ctx,
    struct ibv_exp_device_attr* attr)
{
  // Device memory not supported in fallback mode
  attr->max_dm_size = 0;
  // Clear the comp_mask bit to indicate DM not supported
  attr->comp_mask &= ~IBV_EXP_DEVICE_ATTR_MAX_DM_SIZE;
  return 0;
}

#define ibv_exp_query_device(ctx, attr) compat_exp_query_device(ctx, attr)

#endif // !HAS_EXP_VERBS

#endif // _RDMA_COMPAT_H__
