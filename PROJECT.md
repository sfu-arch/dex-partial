# DEX-CHIME: RDMA-Based Distributed Index Structures

**ATLAS** — An Interactive Benchmarking Framework for RDMA Database Systems
*Simon Fraser University · SOSP'24 / VLDB paper*

---

## Overview

This repository contains three RDMA-optimized distributed index structures and an evaluation framework (ATLAS) that benchmarks them across diverse workloads, cache sizes, and tree configurations. The core research question is: **how do RDMA database systems behave across diverse query distributions and configuration parameters?**

The three systems represent three distinct design philosophies on the spectrum of local caching vs. remote memory access:

| System | Index Type | Cache Strategy | Best For |
|--------|-----------|----------------|----------|
| **DEX** | B+-tree | Full page cache (up to 512 MB) | Range scans + large cache + skewed workloads |
| **CHIME** | B+-tree + hopscotch hashing | Metadata only (~100 MB) | Mixed workloads + cache-constrained environments |
| **DART** | Adaptive Radix Tree (ART) | Directory only (~1 MB) | Minimal local state + predictable latency |

---

## 1. DEX

### What It Is
DEX is a distributed B+-tree built on **Sherman** (a state-of-the-art RDMA B+-tree) with an aggressive **page cache** that holds full data pages locally. It is optimized for range scans and point lookups when sufficient memory is available for caching hot pages.

### Architecture

```
Client (Compute Node)
│
├── IndexCache (skip-list based, up to 512 MB)
│   └── Caches full 1024-byte internal AND leaf pages
│
└── RDMA Layer
    └── Reads 1024-byte pages from remote memory on cache miss
```

### How It Works

**Point Lookup:**
1. Traverse B+-tree from root. Check IndexCache for each node.
2. On **cache hit**: serve key-value from local memory (~0.5–1 µs).
3. On **cache miss**: issue RDMA READ for the 1024-byte page (~4–5 RTTs cold start).
4. Insert fetched page into IndexCache, evicting LRU entries if full.

**Range Scan:**
1. Traverse to the first leaf in range.
2. Scan **sequentially** through the leaf-linked list — leaf pages are laid out for sequential access.
3. Benefit: prefetching and spatial locality make range scans very fast (~2 µs P50).

### Key Parameters
| Parameter | Value | Notes |
|-----------|-------|-------|
| Leaf page size | 1024 bytes | Compile-time |
| Internal page size | 1024 bytes | Compile-time |
| Cache size | 32–512 MB | **Runtime** configurable (`argv[9]`) |
| DSM total size | 8 GB | Remote memory |
| RDMA RTTs (cold) | 4–5 | Depends on tree height |
| RDMA RTTs (warm) | 1 | Cache hit |

### Performance Characteristics
- **Throughput (uniform, 256 MB cache)**: ~7.22 Mops/s
- **Throughput (Zipf θ=0.99, 256 MB cache)**: ~11.33 Mops/s (+57%)
- **Point read P50 (hot)**: ~0.5 µs
- **Range scan P50**: ~2 µs
- **Sharp cache threshold**: Below ~64 MB the system becomes non-functional — the cache is too small to hold the internal nodes needed for navigation.

### Trade-offs
- Excellent throughput when cache is large enough to hold hot pages.
- Strong skew exploitation: Zipfian workloads concentrate access on few pages → near-100% cache hit rate.
- **Weakness**: Requires a large cache. Falls off sharply below a threshold. Not suitable for memory-constrained deployments.

### Code Location
```
dex/
├── include/
│   ├── Common.h          # kLeafPageSize=1024, kInternalPageSize=1024
│   ├── IndexCache.h      # Skip-list based page cache
│   ├── Tree.h            # B+-tree traversal & operations
│   └── cache/            # LRU/frequency eviction policy
├── src/                  # Implementation
└── test/newbench_latency.cpp  # Benchmark entry point
```

---

## 2. CHIME

### What It Is
CHIME (Cache-efficient and High-performance Hybrid Index on disaggregated MEory) is a B+-tree with **hopscotch hashing** in leaf nodes and a **metadata-only cache**. It caches internal navigation nodes (not leaf data), so every key-value retrieval requires an RDMA read to remote memory. Published at **SOSP'24**.

> Paper: https://doi.org/10.1145/3694715.3695959

### Architecture

```
Client (Compute Node)
│
├── TreeCache (~70 MB)
│   └── Caches internal B+-tree nodes only
│
├── IdxCache (~30 MB) — "hotspot fingerprint buffer"
│   └── Tracks (leaf_addr, slot_index) → fingerprint for hot keys
│
└── RDMA Layer
    └── ALWAYS reads leaf data from remote (mandatory RDMA per lookup)
```

### How It Works

**Point Lookup:**
1. Traverse internal nodes (may be served from TreeCache).
2. **Must** issue RDMA READ to fetch the leaf node from remote memory.
3. Within the leaf, use **hopscotch hashing** to locate the key in O(1):
   - Leaf has 64 entries split into groups of 8 (`neighborSize=8`).
   - Each group has a `hop_bitmap` tracking which slots in its neighborhood are occupied by keys that hash to it.
4. Optionally consult IdxCache to short-circuit to the exact slot.

**Range Scan:**
1. Traverse to first leaf via B+-tree.
2. Issue RDMA batch reads across linked leaf segments.
3. Read delegation: optionally offload reads to memory node for better throughput.

### Key Compile-Time Toggles
| Flag | Effect |
|------|--------|
| `HOPSCOTCH_LEAF_NODE` | Hopscotch hashing in leaves (O(1) slot lookup) |
| `VACANCY_AWARE_LOCK` | Vacancy bitmap piggybacked on pointers |
| `METADATA_REPLICATION` | Replicate leaf metadata for validation |
| `SIBLING_BASED_VALIDATION` | Validate via sibling pointers instead of fence keys |
| `SPECULATIVE_READ` | Prefetch adjacent nodes before they are needed |
| `READ_DELEGATION` | Offload reads to memory node |
| `WRITE_COMBINING` | Batch multiple writes together |

### Key Parameters
| Parameter | Value | Notes |
|-----------|-------|-------|
| `kIndexCacheSize` | 100 MB | Total cache (TreeCache + IdxCache), compile-time |
| `leafSpanSize` | 64 entries | Entries per leaf node |
| `internalSpanSize` | 64 entries | Entries per internal node |
| `kHotspotBufSize` | 30 MB | IdxCache size |
| RDMA RTTs per lookup | 2–4 | Always includes one remote leaf fetch |

### Performance Characteristics
- **Throughput (uniform)**: ~3.70 Mops/s
- **Throughput (Zipf θ=0.99)**: ~4.73 Mops/s (+28%)
- **Point read P50**: ~7 µs (dominated by mandatory RDMA leaf fetch)
- **Range scan P50**: ~17 µs
- **Cache resilience**: Works at all cache sizes — metadata cache is much smaller than DEX's page cache requirement.

### Trade-offs
- Graceful degradation under cache pressure (metadata is small).
- Handles mixed workloads (point + range) reasonably well.
- **Weakness**: Mandatory RDMA per lookup is a hard floor on latency. Cannot exploit skew as strongly as DEX because hot leaf data is never cached locally.

### Code Location
```
CHIME/
├── include/
│   ├── Common.h          # kIndexCacheSize=100MB, compile-time config
│   ├── LeafNode.h        # Leaf with hopscotch hashing
│   ├── InternalNode.h    # Internal node structure
│   ├── IdxCache.h        # Hotspot fingerprint buffer
│   ├── DSM.h             # Distributed Shared Memory abstraction
│   └── Rdma.h            # RDMA primitives
├── src/
│   └── rdma/             # Resource management, state transitions
└── test/latency_bench.cpp  # Benchmark entry point
```

---

## 3. DART

### What It Is
DART (lock-free Two-layer Hashed ART for Disaggregated memory) is an **Adaptive Radix Tree** accelerated by an **Express Skip Table (RACE directory)** that allows skipping upper tree levels. It stores only ~1 MB of local state (the directory), issuing RDMA reads for every tree level traversal. Its distinguishing feature is **predictable latency regardless of workload**.

### Architecture

```
Client (Compute Node)
│
├── RACE Directory (~1 MB local)  ← synced once at startup
│   └── Hash table: hash(key_prefix) → remote ART node address
│       Allows skipping 2–3 levels of ART traversal
│
└── RDMA Layer
    ├── Reads 128-byte Buckets (selective, not full nodes)
    └── Lock-free updates via RDMA CAS (Compare-and-Swap)
```

### How It Works

**ART Node Types** (standard ART): Node4, Node16, Node48, Node256 — each stores a compressed mapping from byte → child pointer. Node type is chosen based on number of children to save space.

**Express Skip Table (RACE):**
- A local hash table of size ~1 MB maps `hash(key_prefix)` to the address of a deeper ART node.
- Instead of reading 5+ levels from the root, the lookup starts 2–3 levels deeper.
- The directory is loaded once at startup and reused for every query.

**Example Lookup ("ANIMAL" → value):**
1. Hash key prefix → look up RACE directory locally (no RDMA).
2. RDMA READ: fetch 128-byte bucket from the shortcut ART node.
3. RDMA READ: fetch leaf node (1095 bytes) containing the value.
4. Return value. **Total: ~3 RDMA round-trips.**

**Lock-free Updates:**
- Node metadata (type, prefix length) is packed into 8-byte pointers.
- Updates use RDMA CAS on these pointers — no distributed lock manager needed.
- 99.94% CAS success rate observed (very low contention).

**Bucket Layout (128 bytes):**
```
struct Bucket {
    uint32_t local_depth;
    uint32_t suffix;
    Slot slots[8];   // each: fingerprint + position + type + offset
};
```
Reading only 128-byte buckets instead of full 1 KB nodes reduces RDMA bandwidth by 8×.

### Key Parameters
| Parameter | Value | Notes |
|-----------|-------|-------|
| Local directory size | ~1 MB | Synced at startup |
| Bucket size | 128 bytes | Selective RDMA fetch unit |
| RDMA RTTs per lookup | ~4.5 | Fixed, workload-independent |
| Bytes per operation | ~1,820 bytes | Fixed, workload-independent |
| Node types | 4 (Node4/16/48/256) | Standard ART |

### Performance Characteristics
- **Throughput**: ~1.67–1.71 Mops/s — **flat across all workloads**
- **Skew sensitivity**: Zero — no data cache to exploit hot keys
- **Point read P50**: ~18 µs (fixed)
- **P99 latency**: Very stable (no cache eviction jitter)
- **NUMA effect**: Threads with NUMA-local memory show ~15% lower latency (16.2 µs vs 18.7 µs)

### Trade-offs
- Completely predictable performance — ideal for SLA-constrained deployments.
- Minimal local memory footprint (~1 MB).
- **Weakness**: Hard ceiling at ~1.67 Mops/s. Cannot exploit skew. Not optimized for range scans.

### Code Location
```
DART-main/
├── include/race/
│   ├── race.h            # Express skip-table + ART interface
│   └── aiordma.h         # Async I/O RDMA
├── src/
│   ├── main/
│   │   ├── compute.cc    # Client process (issues queries)
│   │   ├── memory.cc     # Memory server process (hosts DSM)
│   │   └── monitor.cc    # Coordinator process
│   ├── prheart/          # ART node implementations (art-node.cc, etc.)
│   └── race/             # RACE hashing implementation
└── CMakeLists.txt
```

**DART uses a 3-process model**: monitor (coordinator), memory server (hosts the ART in DSM), and compute client (issues RDMA queries). All three must be started in order.

---

## 4. System Comparison

### Throughput vs. Cache Size
```
High ┤ DEX (256MB)  ████████████ 11.33 Mops/s (Zipf 0.99)
     │ DEX (256MB)  ████████     7.22 Mops/s  (uniform)
     │ CHIME        ████▌        4.73 Mops/s  (Zipf 0.99)
     │ CHIME        ████         3.70 Mops/s  (uniform)
     │ DART         ██           1.67 Mops/s  (all workloads)
Low  ┤ DEX (<64MB)  ✗ non-functional
```

### Latency Comparison (Point Read P50)
| Workload | DEX | CHIME | DART |
|----------|-----|-------|------|
| Uniform (cold) | ~4–5 µs | ~7 µs | ~18 µs |
| Uniform (warm cache) | ~1 µs | ~7 µs | ~18 µs |
| Zipf θ=0.99 (warm) | ~0.5 µs | ~7 µs | ~18 µs |

### When to Use Which System
| Scenario | Recommended |
|----------|-------------|
| Large available cache + skewed reads | **DEX** |
| Range scan-heavy workloads | **DEX** |
| Cache-constrained + mixed point/range | **CHIME** |
| Predictable SLA required | **DART** |
| Minimal local memory footprint | **DART** |
| Uniform point lookups, moderate cache | **CHIME** |

### Key Crossover Points
1. **DEX vs. CHIME crossover**: ~64–128 MB cache. Below this, DEX falls off a cliff; CHIME's smaller metadata cache still fits.
2. **CHIME vs. DART crossover**: When cache is so constrained that CHIME's 100 MB metadata doesn't fit, DART's 1 MB directory remains functional.
3. **Skew exploitation**: DEX shows 57% throughput gain from uniform → Zipf 0.99; CHIME shows 28%; DART shows 0%.

---

## 5. RDMA Infrastructure

All three systems use **InfiniBand RDMA** for remote memory access and are built on a shared **Distributed Shared Memory (DSM)** abstraction.

### Cluster Configuration
| Role | IP |
|------|----|
| Memory Server | 10.30.1.9 |
| Compute Client | 10.30.1.6 |

### RDMA Operation Types Used
| Operation | Description | Used By |
|-----------|-------------|---------|
| `RDMA READ` | One-sided read of remote buffer | All three |
| `RDMA WRITE` | One-sided write to remote buffer | All three |
| `RDMA CAS` | Atomic compare-and-swap | DART (lock-free updates) |
| `RDMA FAA` | Fetch-and-add | Counters/versioning |

### Coordination
- **Memcached** is used to exchange RDMA Queue Pair (QP) metadata between nodes and synchronize experiment start/stop.
- Memcached must be **killed and restarted** between runs (not just flushed) — stale `serverNum`/`clientNum` values cause indefinite hangs.

### Memory Layout
```
Hugepages: 36,864 × 2 MB pages = ~72 GB allocated
DSM total: 8 GB (remote memory pool)
Per-thread RDMA buffer: 10 MB × 30 threads = 300 MB
```

---

## 6. Building the Systems

### Prerequisites
- Linux with InfiniBand RDMA support (`libibverbs`, `librdmacm`)
- CMake ≥ 3.14
- GCC with C++17 support
- Memcached
- Hugepages configured (`/proc/sys/vm/nr_hugepages`)

### Build Commands

**DEX:**
```bash
cd dex
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

**CHIME:**
```bash
cd CHIME
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

**DART:**
```bash
cd DART-main
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

---

## 7. Running Benchmarks

### DEX Benchmark Interface
```bash
./newbench_latency \
  <node_count>      # Number of cluster nodes
  <read_ratio>      # Fraction of reads (0.0–1.0)
  <insert_ratio>    # Fraction of inserts
  <update_ratio>    # Fraction of updates
  <delete_ratio>    # Fraction of deletes
  <range_ratio>     # Fraction of range scans
  <total_threads>   # Client threads
  <mem_threads>     # Memory node threads
  <cache_mb>        # Cache size in MB (runtime configurable)
  <uniform>         # 1=uniform, 0=Zipfian
  <zipf_theta>      # Zipf skew parameter (e.g. 0.99)
  <bulk_load_M>     # Millions of keys to load
  <warmup_M>        # Millions of warmup ops
  <run_M>           # Millions of measured ops
  <check>           <time_based>  <early_stop>
  <tree_index>      <rpc_rate>    <admit_rate>  <auto_tune>  <max_thread>
```

### CHIME Benchmark Interface
```bash
./latency_bench \
  <node_count>      # Number of cluster nodes
  <thread_count>    # Client threads
  <read_ratio>      # Fraction of point reads
  <range_ratio>     # Fraction of range scans
  <total_ops>       # Total operations to run
  <range_size>      # Keys per range scan
  <zipf_theta>      # Zipf skew (0 = uniform)
  <uniform>         # 1=uniform override
```

### Output Format
Both systems output latency histograms at 500 ns granularity (0–50 ms, 100,000 buckets):
- `*_read_latency.dat` — point read latency distribution
- `*_range_latency.dat` — range scan latency distribution

---

## 8. Experiment Suite

### QW1 — Zipfian Skew Sensitivity
Varies Zipf θ ∈ {uniform, 0.6, 0.8, 0.9, 0.99} for both DEX and CHIME.
**Key finding**: DEX exploits skew 2× more effectively than CHIME.

### QW2 — Coherent Trie (CHIME Ablation)
Compares original CHIME vs. CHIME with coherent trie optimization enabled.
**Key finding**: Coherent trie reduces range scan P99 significantly at high skew.

### QW3 — Tree Height Crossover
Varies key-space size (10M → 100M keys) to change tree height for DEX and DART.
**Key finding**: DEX throughput degrades with tree height; DART is height-invariant due to express skip table.

### Op Crossover
Sweeps read/range ratio to find the point where CHIME beats DEX or vice versa.
**Key finding**: DEX dominates for >70% range workloads; CHIME is better for point-heavy workloads with small caches.

---

## 9. Data Structures Summary

### DEX: B+-tree Page
```
┌─────────────────────────────────┐
│ Internal Page (1024 bytes)      │
│ [key₀|ptr₀] [key₁|ptr₁] ...    │
└─────────────────────────────────┘
┌─────────────────────────────────┐
│ Leaf Page (1024 bytes)          │
│ [key₀|val₀] [key₁|val₁] ...    │ ──→ next leaf (scan)
└─────────────────────────────────┘
```

### CHIME: Leaf Node with Hopscotch Hashing
```
┌─────────────────────────────────────────────────┐
│ LeafNode                                         │
│  metadata: version | sibling_ptr | fence_keys    │
│  entries[64]:                                    │
│    group[0..7] (8 entries each):                 │
│      hop_bitmap: which of the 8 slots are mine   │
│      [fingerprint|key|value] × 8                 │
└─────────────────────────────────────────────────┘
```

### DART: ART Node (Bucket Layout)
```
┌──────────────────────────────────────┐
│ Node (remote, full size 512–1024 B)  │
│   Divided into 128-byte Buckets      │
│   ┌────────────────────┐             │
│   │ Bucket (128 bytes) │             │
│   │  local_depth       │             │
│   │  suffix            │             │
│   │  slots[8]:         │             │
│   │    fingerprint     │             │
│   │    child_ptr       │             │
│   └────────────────────┘             │
└──────────────────────────────────────┘
RACE Directory (local, ~1 MB):
  hash(prefix) → remote Bucket address
```

---

## 10. Academic Context

This work is part of a VLDB/SOSP paper draft (*ATLAS: Interactive Benchmarking Framework for RDMA Database Systems*) by Abhiram Prasanna at Simon Fraser University.

**Core thesis**: Single-point benchmarks of RDMA index structures are misleading. Systems exhibit stable *operating regions* and sharp *transition points* that only appear when sweeping workload, cache, and structural parameters together. ATLAS provides a principled multi-axis evaluation framework to expose these behaviors.

**References:**
- CHIME (SOSP'24): https://doi.org/10.1145/3694715.3695959
- Sherman (B+-tree baseline for DEX): SIGMOD'22
- RDMA ART / ROLEX (related work)
- RACE hashing (basis for DART's express skip table)
