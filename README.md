# DEX vs CHIME: Disaggregated Memory B+-Tree Systems

This repository contains modified versions of **DEX** and **CHIME** - two disaggregated memory B+-tree implementations that use RDMA for remote memory access.

---

## 📋 Table of Contents

1. [What is RDMA and Disaggregated Memory?](#what-is-rdma-and-disaggregated-memory)
2. [How DEX Works](#how-dex-works)
3. [How CHIME Works](#how-chime-works)
4. [The RDMA Compatibility Problem](#the-rdma-compatibility-problem)
5. [My RDMA Wrapper Solution](#my-rdma-wrapper-solution)
6. [All Implementation Changes](#all-implementation-changes)
7. [Build and Run Instructions](#build-and-run-instructions)
8. [Troubleshooting](#troubleshooting)

---

## 🔌 What is RDMA and Disaggregated Memory?

### RDMA (Remote Direct Memory Access)

RDMA allows one computer to directly read/write memory on another computer **without involving the remote CPU**. This is extremely fast because:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    TRADITIONAL NETWORK vs RDMA                              │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   TRADITIONAL (TCP/IP):                                                     │
│   ┌──────────┐         ┌──────────┐         ┌──────────┐                   │
│   │  App     │ ──────► │  Kernel  │ ──────► │   NIC    │ ─────────┐        │
│   │  (CPU)   │         │  (CPU)   │         │          │          │        │
│   └──────────┘         └──────────┘         └──────────┘          │        │
│                                                                    │        │
│                                             Network                │        │
│                                                                    ▼        │
│   ┌──────────┐         ┌──────────┐         ┌──────────┐                   │
│   │  App     │ ◄────── │  Kernel  │ ◄────── │   NIC    │                   │
│   │  (CPU)   │         │  (CPU)   │         │          │                   │
│   └──────────┘         └──────────┘         └──────────┘                   │
│   ❌ Multiple CPU copies, high latency (~100μs)                            │
│                                                                             │
│   RDMA (ONE-SIDED):                                                         │
│   ┌──────────┐                              ┌──────────┐                   │
│   │  App     │ ─────────────────────────────│   NIC    │ ─────────┐        │
│   │  Posts   │      (bypass kernel)         │          │          │        │
│   │  WR      │                              └──────────┘          │        │
│   └──────────┘                                                    │        │
│                                             Network                │        │
│                                                                    ▼        │
│   ┌──────────┐                              ┌──────────┐                   │
│   │  Memory  │ ◄────────────────────────────│   NIC    │                   │
│   │  (Data)  │    (CPU not involved!)       │          │                   │
│   └──────────┘                              └──────────┘                   │
│   ✅ Zero CPU copies, low latency (~1-2μs)                                 │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Key RDMA Operations

| Operation | What it does | CPU Involvement |
|-----------|--------------|-----------------|
| `RDMA READ` | Read remote memory into local buffer | Only local CPU |
| `RDMA WRITE` | Write local buffer to remote memory | Only local CPU |
| `RDMA CAS` | Atomic Compare-And-Swap on remote memory | Only local CPU |
| `RDMA FAA` | Atomic Fetch-And-Add on remote memory | Only local CPU |

### Disaggregated Memory Architecture

In disaggregated memory, **compute nodes** (CPUs) are separated from **memory nodes** (RAM). They communicate via RDMA:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    DISAGGREGATED MEMORY ARCHITECTURE                        │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   COMPUTE NODE (runs queries)           MEMORY NODE (stores B+-tree)       │
│   ┌───────────────────────┐            ┌───────────────────────┐           │
│   │                       │            │                       │           │
│   │  ┌─────────────────┐  │   RDMA     │  ┌─────────────────┐  │           │
│   │  │  Application    │  │   READ     │  │   Root Node     │  │           │
│   │  │  Thread         │──┼───────────►│  │   [keys|ptrs]   │  │           │
│   │  └─────────────────┘  │            │  └────────┬────────┘  │           │
│   │          │            │            │           │           │           │
│   │  ┌───────▼─────────┐  │            │  ┌────────▼────────┐  │           │
│   │  │  Local Cache    │  │   RDMA     │  │ Internal Nodes  │  │           │
│   │  │  (hot nodes)    │  │   READ     │  │   [keys|ptrs]   │  │           │
│   │  └─────────────────┘  │◄───────────│  └────────┬────────┘  │           │
│   │          │            │            │           │           │           │
│   │  ┌───────▼─────────┐  │            │  ┌────────▼────────┐  │           │
│   │  │  RDMA Buffer    │  │   RDMA     │  │  Leaf Nodes     │  │           │
│   │  │  (registered)   │  │   WRITE    │  │  [key|value]... │  │           │
│   │  └─────────────────┘  │───────────►│  └─────────────────┘  │           │
│   │                       │            │                       │           │
│   └───────────────────────┘            └───────────────────────┘           │
│                                                                             │
│   Memory Pool: 8 GB (registered with RDMA NIC)                             │
│   RDMA Buffer: 1 GB (for read/write operations)                            │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 🔷 How DEX Works

### DEX Overview

DEX is a disaggregated B+-tree that uses **standard RDMA verbs** (`ibv_*` functions from `libibverbs`).

### DEX RDMA Implementation

DEX uses the standard RDMA API directly. Here's what happens in `dex/src/rdma/`:

```cpp
// dex/include/Rdma.h - Standard RDMA includes
#include <infiniband/verbs.h>  // Standard libibverbs

// dex/src/rdma/Resource.cpp - Memory Registration
ibv_mr *createMemoryRegion(uint64_t mm, uint64_t mmSize, RdmaContext *ctx) {
    // Register memory with RDMA NIC so it can be accessed remotely
    return ibv_reg_mr(ctx->pd, (void *)mm, mmSize,
                      IBV_ACCESS_LOCAL_WRITE |      // Local CPU can write
                      IBV_ACCESS_REMOTE_READ |      // Remote can RDMA READ
                      IBV_ACCESS_REMOTE_WRITE |     // Remote can RDMA WRITE
                      IBV_ACCESS_REMOTE_ATOMIC);    // Remote can do CAS/FAA
}

// dex/src/rdma/Operation.cpp - RDMA Operations
bool rdmaRead(ibv_qp *qp, uint64_t source, uint64_t dest, 
              uint64_t size, uint32_t lkey, uint32_t remoteRKey) {
    struct ibv_send_wr wr;
    wr.opcode = IBV_WR_RDMA_READ;           // One-sided read
    wr.wr.rdma.remote_addr = dest;          // Remote memory address
    wr.wr.rdma.rkey = remoteRKey;           // Remote memory key
    return ibv_post_send(qp, &wr, &wrBad);  // Post to queue
}
```

### DEX Data Flow

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         DEX SEARCH OPERATION                                │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   Step 1: Check IndexCache (local)                                         │
│   ┌─────────────────────────────────────────────────────────────────┐      │
│   │  IndexCache (SkipList-based)                                    │      │
│   │  ┌─────────┐   ┌─────────┐   ┌─────────┐                       │      │
│   │  │[10,50)  │──►│[50,100) │──►│[100,200)│  (cached internal     │      │
│   │  │ ptr=A   │   │ ptr=B   │   │ ptr=C   │   nodes by range)     │      │
│   │  └─────────┘   └─────────┘   └─────────┘                       │      │
│   └─────────────────────────────────────────────────────────────────┘      │
│           │ Cache HIT: skip to leaf                                        │
│           │ Cache MISS: traverse from root                                 │
│           ▼                                                                │
│   Step 2: RDMA READ remote node                                            │
│   ┌─────────────────────────────────────────────────────────────────┐      │
│   │  ibv_post_send(qp, RDMA_READ, remote_addr, size)                │      │
│   │       │                                                          │      │
│   │       ▼                                                          │      │
│   │  NIC fetches data directly from remote memory                   │      │
│   │       │                                                          │      │
│   │       ▼                                                          │      │
│   │  ibv_poll_cq() - wait for completion                            │      │
│   └─────────────────────────────────────────────────────────────────┘      │
│           │                                                                │
│           ▼                                                                │
│   Step 3: Process node, find next pointer, repeat until leaf               │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### DEX Key Files

| File | Purpose |
|------|---------|
| `dex/include/Rdma.h` | RDMA type definitions, function declarations |
| `dex/src/rdma/Resource.cpp` | `createContext()`, `createMemoryRegion()`, `createQueuePair()` |
| `dex/src/rdma/Operation.cpp` | `rdmaRead()`, `rdmaWrite()`, `rdmaCas()`, `rdmaFaa()` |
| `dex/src/rdma/StateTrans.cpp` | QP state transitions (INIT→RTR→RTS) |
| `dex/include/IndexCache.h` | SkipList cache for internal nodes |

---

## 🔶 How CHIME Works

### CHIME Overview

CHIME is a more advanced disaggregated B+-tree that uses **Mellanox experimental RDMA verbs** (`ibv_exp_*` functions). These provide:

1. **DC (Dynamically Connected) Transport** - More scalable than RC
2. **Device Memory** - On-NIC memory for ultra-low latency
3. **Masked Atomics** - Partial word atomic operations

### CHIME's Original RDMA Requirements

```cpp
// CHIME originally required these MLNX_OFED experimental APIs:
#include <infiniband/verbs_exp.h>  // Only in Mellanox OFED!

// DC Transport (Dynamically Connected)
ibv_exp_dct *dct;                           // DC Target
ibv_exp_create_dct(ctx, &dct_attr);         // Create DCT
qp_type = IBV_EXP_QPT_DC_INI;               // DC Initiator QP

// Device Memory (on-chip NIC memory)
ibv_exp_dm *dm = ibv_exp_alloc_dm(ctx, &dm_attr);
ibv_exp_reg_mr(&mr_in);                     // Register DM

// Masked Atomics (partial word CAS)
wr.exp_opcode = IBV_EXP_WR_EXT_MASKED_ATOMIC_CMP_AND_SWP;
wr.ext_op.masked_atomics.wr_data.inline_data.op.cmp_swap.compare_mask = 0xFF;
```

### CHIME Data Structures

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    CHIME LEAF NODE (Hopscotch Hashing)                      │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   Traditional B+-tree leaf:        CHIME Hopscotch leaf:                   │
│   ┌───┬───┬───┬───┬───┐            ┌───┬───────┬───┬───────┐              │
│   │K1 │K2 │K3 │K4 │K5 │            │K  │bitmap │K  │bitmap │              │
│   ├───┼───┼───┼───┼───┤            ├───┼───────┼───┼───────┤              │
│   │V1 │V2 │V3 │V4 │V5 │            │V  │       │V  │       │              │
│   └───┴───┴───┴───┴───┘            └───┴───────┴───┴───────┘              │
│   (sorted, binary search)          (hash-based, O(1) lookup)              │
│                                                                             │
│   LeafEntry structure (CHIME/include/LeafNode.h):                          │
│   ┌─────────────────────────────────────────────────────────────────┐      │
│   │  struct LeafEntry {                                             │      │
│   │    PackedVersion h_version;    // Version for consistency       │      │
│   │    uint16_t hop_bitmap;        // Hopscotch neighborhood bits   │      │
│   │    Key key;                    // 8-byte key                    │      │
│   │    Value value;                // 8-byte value                  │      │
│   │  };                                                             │      │
│   └─────────────────────────────────────────────────────────────────┘      │
│                                                                             │
│   hop_bitmap example (neighborhood size = 8):                              │
│   Entry at slot 5 has bitmap = 10110000                                    │
│   Meaning: keys hashing to slot 5 are at slots 5, 6, 8                     │
│   (relative positions where bit=1)                                         │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### CHIME Cache System (TreeCache)

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         CHIME CACHE ARCHITECTURE                            │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   TreeCache (range-based, CHIME/include/TreeCache.h):                      │
│   ┌─────────────────────────────────────────────────────────────────┐      │
│   │  InlineSkipList<TreeCacheEntry>                                 │      │
│   │                                                                  │      │
│   │  TreeCacheEntry {                                               │      │
│   │    Key from, to;           // Key range [from, to)              │      │
│   │    InternalNode* ptr;      // Cached node content               │      │
│   │    uint64_t cache_entry_freq;  // Access frequency for eviction │      │
│   │  }                                                              │      │
│   │                                                                  │      │
│   │  Operations:                                                    │      │
│   │  - search_from_cache(key) → find entry covering key range       │      │
│   │  - add_to_cache(node) → cache with [lowest, highest) range     │      │
│   │  - invalidate(entry) → remove stale entry                      │      │
│   └─────────────────────────────────────────────────────────────────┘      │
│                                                                             │
│   IdxCache (speculative read optimization):                                │
│   ┌─────────────────────────────────────────────────────────────────┐      │
│   │  Caches leaf location predictions                               │      │
│   │  On search: speculatively read leaf while traversing            │      │
│   │  If prediction correct: save one RDMA round trip                │      │
│   └─────────────────────────────────────────────────────────────────┘      │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## ⚠️ The RDMA Compatibility Problem

### The Problem

CHIME was written for **Mellanox OFED** (MLNX_OFED) which provides experimental RDMA extensions. Standard Linux systems only have **rdma-core** which doesn't include these:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    RDMA DRIVER COMPARISON                                   │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   MLNX_OFED (Mellanox proprietary):    rdma-core (standard Linux):         │
│   ┌─────────────────────────────┐      ┌─────────────────────────────┐     │
│   │ #include <verbs.h>          │      │ #include <verbs.h>          │     │
│   │ #include <verbs_exp.h>  ✓   │      │ // verbs_exp.h NOT FOUND ✗  │     │
│   │                             │      │                             │     │
│   │ ibv_create_qp()         ✓   │      │ ibv_create_qp()         ✓   │     │
│   │ ibv_exp_create_qp()     ✓   │      │ ibv_exp_create_qp()     ✗   │     │
│   │ ibv_exp_create_dct()    ✓   │      │ ibv_exp_create_dct()    ✗   │     │
│   │ ibv_exp_alloc_dm()      ✓   │      │ ibv_exp_alloc_dm()      ✗   │     │
│   │ IBV_EXP_QPT_DC_INI      ✓   │      │ IBV_EXP_QPT_DC_INI      ✗   │     │
│   └─────────────────────────────┘      └─────────────────────────────┘     │
│                                                                             │
│   CHIME needs ibv_exp_* functions which don't exist in rdma-core!          │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Build Errors Without My Wrapper

Without the compatibility layer, CHIME fails to compile:

```
error: 'ibv_exp_dct' was not declared in this scope
error: 'IBV_EXP_QPT_DC_INI' was not declared in this scope
error: 'ibv_exp_send_wr' was not declared in this scope
error: 'ibv_exp_create_dct' was not declared in this scope
error: 'ibv_exp_alloc_dm' was not declared in this scope
```

---

## 🔧 My RDMA Wrapper Solution

### What the Wrapper Does

I created `CHIME/include/RdmaCompat.h` (570+ lines) that provides:

1. **Stub structures** for experimental types (`ibv_exp_dct`, `ibv_exp_send_wr`, etc.)
2. **Wrapper functions** that convert experimental API calls to standard ones
3. **Automatic fallback** from DC transport to RC transport
4. **Device memory emulation** using regular host memory

### How the Wrapper Works

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    RDMACOMPAT.H WRAPPER ARCHITECTURE                        │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   Compile-Time Detection:                                                   │
│   ┌─────────────────────────────────────────────────────────────────┐      │
│   │  #if __has_include(<infiniband/verbs_exp.h>)                    │      │
│   │    #define HAS_EXP_VERBS 1    // MLNX_OFED present              │      │
│   │    #include <infiniband/verbs_exp.h>                            │      │
│   │    #define USE_DC_TRANSPORT 1                                   │      │
│   │    #define USE_DEVICE_MEMORY 1                                  │      │
│   │  #else                                                          │      │
│   │    #define HAS_EXP_VERBS 0    // rdma-core only                 │      │
│   │    #define USE_DC_TRANSPORT 0                                   │      │
│   │    #define USE_DEVICE_MEMORY 0                                  │      │
│   │    // ... provide stub definitions ...                          │      │
│   │  #endif                                                         │      │
│   └─────────────────────────────────────────────────────────────────┘      │
│                                                                             │
│   When HAS_EXP_VERBS = 0, we provide:                                      │
│                                                                             │
│   1. STUB STRUCTURES:                                                       │
│   ┌─────────────────────────────────────────────────────────────────┐      │
│   │  struct ibv_exp_dct {           // DC Target stub               │      │
│   │    uint32_t dct_num;            // Fake DCT number              │      │
│   │    void* context;                                               │      │
│   │  };                                                             │      │
│   │                                                                  │      │
│   │  struct ibv_exp_send_wr {       // Extended send WR stub        │      │
│   │    uint64_t wr_id;                                              │      │
│   │    struct ibv_exp_send_wr* next;                                │      │
│   │    struct ibv_sge* sg_list;                                     │      │
│   │    int num_sge;                                                 │      │
│   │    enum ibv_wr_opcode exp_opcode;                               │      │
│   │    int exp_send_flags;                                          │      │
│   │    // ... extended fields for masked atomics, DC ...            │      │
│   │  };                                                             │      │
│   │                                                                  │      │
│   │  struct ibv_exp_dm {            // Device memory stub           │      │
│   │    void* dm_ptr;                // Points to regular malloc     │      │
│   │    size_t length;                                               │      │
│   │  };                                                             │      │
│   └─────────────────────────────────────────────────────────────────┘      │
│                                                                             │
│   2. WRAPPER FUNCTIONS:                                                     │
│   ┌─────────────────────────────────────────────────────────────────┐      │
│   │  // DC QP creation → falls back to RC QP                        │      │
│   │  struct ibv_qp* compat_create_qp(ctx, exp_attr) {               │      │
│   │    struct ibv_qp_init_attr attr;                                │      │
│   │    // Convert exp_attr to standard attr                         │      │
│   │    if (attr.qp_type == IBV_EXP_QPT_DC_INI)                      │      │
│   │      attr.qp_type = IBV_QPT_RC;  // Use RC instead of DC        │      │
│   │    return ibv_create_qp(exp_attr->pd, &attr);                   │      │
│   │  }                                                              │      │
│   │                                                                  │      │
│   │  // Device memory → falls back to malloc                        │      │
│   │  struct ibv_exp_dm* compat_alloc_dm(ctx, attr) {                │      │
│   │    dm->dm_ptr = aligned_alloc(64, attr->length);                │      │
│   │    return dm;                                                   │      │
│   │  }                                                              │      │
│   │                                                                  │      │
│   │  // Masked atomics → falls back to standard CAS/FAA             │      │
│   │  int compat_exp_post_send(qp, wr, bad_wr) {                     │      │
│   │    switch (wr->exp_opcode) {                                    │      │
│   │      case IBV_EXP_WR_EXT_MASKED_ATOMIC_CMP_AND_SWP:             │      │
│   │        std_wr.opcode = IBV_WR_ATOMIC_CMP_AND_SWP;               │      │
│   │        // (ignores mask, uses full 64-bit CAS)                  │      │
│   │        break;                                                   │      │
│   │    }                                                            │      │
│   │    return ibv_post_send(qp, &std_wr, &std_bad_wr);              │      │
│   │  }                                                              │      │
│   └─────────────────────────────────────────────────────────────────┘      │
│                                                                             │
│   3. MACRO MAPPINGS:                                                        │
│   ┌─────────────────────────────────────────────────────────────────┐      │
│   │  #define ibv_exp_create_qp(ctx, attr)  compat_create_qp(ctx, attr)    │
│   │  #define ibv_exp_create_dct(ctx, attr) compat_create_dct(ctx, attr)   │
│   │  #define ibv_exp_alloc_dm(ctx, attr)   compat_alloc_dm(ctx, attr)     │
│   │  #define ibv_exp_post_send(qp, wr, b)  compat_exp_post_send(qp, wr, b)│
│   │  #define IBV_EXP_QPT_DC_INI            IBV_QPT_RC  // DC→RC           │
│   └─────────────────────────────────────────────────────────────────┘      │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Transport Fallback: DC → RC

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    DC vs RC TRANSPORT                                       │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   DC (Dynamically Connected) - Original CHIME:                             │
│   ┌─────────────────┐                                                       │
│   │  Compute Node   │                                                       │
│   │  ┌───────────┐  │       One QP connects to ANY remote DCT              │
│   │  │   QP      │──┼──────► Memory Node 1 (DCT)                           │
│   │  │ (DC_INI)  │──┼──────► Memory Node 2 (DCT)                           │
│   │  │           │──┼──────► Memory Node 3 (DCT)                           │
│   │  └───────────┘  │       (fewer resources, more scalable)               │
│   └─────────────────┘                                                       │
│                                                                             │
│   RC (Reliably Connected) - Fallback:                                      │
│   ┌─────────────────┐                                                       │
│   │  Compute Node   │                                                       │
│   │  ┌───────────┐  │       Each QP connects to ONE remote QP              │
│   │  │   QP 1    │──┼──────► Memory Node 1 (QP)                            │
│   │  │   QP 2    │──┼──────► Memory Node 2 (QP)                            │
│   │  │   QP 3    │──┼──────► Memory Node 3 (QP)                            │
│   │  └───────────┘  │       (more resources, but works everywhere)         │
│   └─────────────────┘                                                       │
│                                                                             │
│   My wrapper: When CHIME creates IBV_EXP_QPT_DC_INI, I substitute          │
│   IBV_QPT_RC so it still works without MLNX_OFED                           │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 📝 All Implementation Changes

### Changes to CHIME

#### 1. New File: `CHIME/include/RdmaCompat.h` (570 lines)

**Purpose**: RDMA compatibility layer enabling CHIME to run without MLNX_OFED

**Key Components**:

```cpp
// Feature Detection
#if __has_include(<infiniband/verbs_exp.h>)
  #define HAS_EXP_VERBS 1
  #include <infiniband/verbs_exp.h>
#else
  #define HAS_EXP_VERBS 0
  // Provide stub definitions...
#endif

// Stub Structures (when HAS_EXP_VERBS=0)
struct ibv_exp_dct { ... };           // DC Target
struct ibv_exp_qp_init_attr { ... };  // Extended QP attributes
struct ibv_exp_send_wr { ... };       // Extended send work request
struct ibv_exp_dm { ... };            // Device memory

// Compatibility Functions
compat_create_qp()     // ibv_exp_create_qp → ibv_create_qp (DC→RC)
compat_create_dct()    // ibv_exp_create_dct → stub (returns fake DCT)
compat_alloc_dm()      // ibv_exp_alloc_dm → malloc
compat_exp_post_send() // ibv_exp_post_send → ibv_post_send (masked→std atomics)
compat_exp_modify_qp() // ibv_exp_modify_qp → ibv_modify_qp

// API Mappings
#define ibv_exp_create_qp(ctx, attr) compat_create_qp(ctx, attr)
#define ibv_exp_create_dct(ctx, attr) compat_create_dct(ctx, attr)
// ... etc
```

#### 2. Modified: `CHIME/include/Rdma.h`

**Change**: Added include for compatibility layer

```cpp
#include "RdmaCompat.h"  // RDMA compatibility layer for non-MLNX_OFED systems
```

#### 3. Modified: `CHIME/src/rdma/StateTrans.cpp`

**Change**: Wrapped DC-specific code with preprocessor guards

```cpp
// Before (would fail without MLNX_OFED):
case IBV_EXP_QPT_DC_INI:
  // DC-specific state transition code

// After (only compiles DC code when available):
#if USE_DC_TRANSPORT
case IBV_EXP_QPT_DC_INI:
  // DC-specific state transition code
#endif
```

#### 4. Modified: `CHIME/include/Common.h`

**Changes**: Reduced memory configuration for limited server resources

```cpp
// Before:
constexpr uint64_t dsmSize = 64;        // 64 GB
constexpr uint64_t rdmaBufferSize = 4;  // 4 GB
#define MAX_APP_THREAD 65

// After:
constexpr uint64_t dsmSize = 8;         // 8 GB
constexpr uint64_t rdmaBufferSize = 1;  // 1 GB
#define MAX_APP_THREAD 17               // Limited by kernel keys
```

#### 5. Modified: `CHIME/include/HugePageAlloc.h`

**Change**: Added fallback when huge pages unavailable

```cpp
inline void *hugePageAlloc(size_t size) {
  // Try huge pages first
  void *res = mmap(NULL, size, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB, -1, 0);
  
  if (res == MAP_FAILED) {
    // Fallback to regular pages
    printf("Huge pages failed, falling back to regular pages\n");
    res = mmap(NULL, size, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  }
  return res;
}
```

#### 6. Modified: `CHIME/src/rdma/Resource.cpp`

**Change**: Added detailed error reporting

```cpp
ibv_mr *createMemoryRegion(uint64_t mm, uint64_t mmSize, RdmaContext *ctx) {
  ibv_mr *mr = ibv_reg_mr(ctx->pd, (void *)mm, mmSize, access_flags);
  
  if (!mr) {
    // NEW: Detailed error message
    Debug::notifyError("Memory registration failed: addr=%p, size=%lu MB, errno=%d (%s)",
                       (void*)mm, mmSize/(1024*1024), errno, strerror(errno));
  }
  return mr;
}
```

### Changes to DEX

#### 1. Modified: `dex/include/Common.h`

**Changes**: Reduced memory configuration

```cpp
// Before:
constexpr uint64_t dsmSize = 64;        // 64 GB
constexpr uint64_t rdmaBufferSize = 2;  // 2 GB

// After:
constexpr uint64_t dsmSize = 8;         // 8 GB
constexpr uint64_t rdmaBufferSize = 1;  // 1 GB
```

#### 2. Modified: `dex/include/HugePageAlloc.h`

**Change**: Added fallback when huge pages unavailable (same as CHIME)

---

## 📊 Summary Diagram

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    COMPLETE SYSTEM OVERVIEW                                 │
├─────────────────────────────────────────────────────────────────────────────┤
│                                                                             │
│   ┌─────────────────────────────┐    ┌─────────────────────────────┐       │
│   │           DEX               │    │          CHIME              │       │
│   │                             │    │                             │       │
│   │  ┌───────────────────────┐  │    │  ┌───────────────────────┐  │       │
│   │  │      Tree.cpp         │  │    │  │      Tree.cpp         │  │       │
│   │  │  (B+-tree operations) │  │    │  │  (B+-tree operations) │  │       │
│   │  └───────────┬───────────┘  │    │  └───────────┬───────────┘  │       │
│   │              │              │    │              │              │       │
│   │  ┌───────────▼───────────┐  │    │  ┌───────────▼───────────┐  │       │
│   │  │    IndexCache.h       │  │    │  │    TreeCache.h        │  │       │
│   │  │  (SkipList cache)     │  │    │  │  (SkipList + IdxCache)│  │       │
│   │  └───────────┬───────────┘  │    │  └───────────┬───────────┘  │       │
│   │              │              │    │              │              │       │
│   │  ┌───────────▼───────────┐  │    │  ┌───────────▼───────────┐  │       │
│   │  │     Rdma.h            │  │    │  │     Rdma.h            │  │       │
│   │  │  (standard verbs)     │  │    │  │  + RdmaCompat.h ★     │  │       │
│   │  └───────────┬───────────┘  │    │  │  (exp→std wrapper)    │  │       │
│   │              │              │    │  └───────────┬───────────┘  │       │
│   │              │              │    │              │              │       │
│   │  ┌───────────▼───────────┐  │    │  ┌───────────▼───────────┐  │       │
│   │  │   libibverbs          │  │    │  │   libibverbs          │  │       │
│   │  │   (ibv_*)             │  │    │  │   (ibv_* via wrapper) │  │       │
│   │  └───────────┬───────────┘  │    │  └───────────┬───────────┘  │       │
│   │              │              │    │              │              │       │
│   └──────────────┼──────────────┘    └──────────────┼──────────────┘       │
│                  │                                  │                       │
│                  └──────────────┬───────────────────┘                       │
│                                 │                                           │
│                  ┌──────────────▼───────────────┐                           │
│                  │         RDMA NIC             │                           │
│                  │  (InfiniBand/RoCE)           │                           │
│                  └──────────────┬───────────────┘                           │
│                                 │                                           │
│                  ┌──────────────▼───────────────┐                           │
│                  │      Remote Memory Node      │                           │
│                  │      (B+-tree data)          │                           │
│                  └──────────────────────────────┘                           │
│                                                                             │
│   ★ = New file I created                                                   │
│   Configuration changes: Both systems now use 8GB + 1GB memory             │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## 🔨 Build and Run Instructions

### Build Both Systems

```bash
# Clone repository
git clone https://github.com/sfu-arch/dex-partial.git
cd dex-partial

# Build CHIME
cd CHIME
mkdir -p build && cd build
cmake -DENABLE_CACHE=on ..
make -j8
cd ../..

# Build DEX
cd dex
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j8
```

### Start memcached (required for coordination)

```bash
memcached -u $USER -l 127.0.0.1 -p 11211 -c 10000 -d
```

### Configure memcached.conf

```bash
# In both CHIME/ and dex/ directories:
echo -e "127.0.0.1\n11211" > memcached.conf
```

### Run Single-Node Tests

```bash
# CHIME
cd ~/dex-partial/CHIME/build
./ycsb_test 1 8 8 randint a

# DEX
cd ~/dex-partial/dex/build
./newbench 1 100 0 0 0 0 8 4 256 0 0.99 50 10 50 0 1 1 0 1 0.1 0 8
```

---

## 🔧 Troubleshooting

| Error | Cause | Solution |
|-------|-------|----------|
| `Memory registration failed: errno=12` | memlock limit too low | `ulimit -l` check; need admin to increase in `/etc/security/limits.conf` |
| `mmap failed` | Huge pages unavailable | OK - will fallback to regular pages automatically |
| `ibv_exp_* not declared` | MLNX_OFED not installed | Should not happen - RdmaCompat.h handles this |
| `SERVER HAS FAILED` | memcached not running | Start memcached first |

---

## 📚 References

1. **DEX Paper**: "DEX: Scalable Range Indexing on Disaggregated Memory" (VLDB 2024)
2. **CHIME Paper**: "CHIME: A Cache-Efficient and High-Performance Hybrid Index on Disaggregated Memory"
3. **RDMA Programming**: [rdma-core documentation](https://github.com/linux-rdma/rdma-core)

---

*Modified by: apa222 | Repository: https://github.com/sfu-arch/dex-partial | Last updated: January 22, 2026*
