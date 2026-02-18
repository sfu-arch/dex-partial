# TBC — Trie + Bitmap Cache for Disaggregated Memory B+-Trees

## Motivation

Comparing **DEX** and **CHIME**, CHIME's architecture has a fundamental
bottleneck: it stores the **vacancy bitmap inside VALOCK on the memory node**,
forcing an RDMA round-trip just to check which leaf slots are occupied.

TBC fixes this by **pulling the bitmap to the compute node** and replacing
CHIME's skip-list inner-node cache with a **radix trie** for O(key_bytes)
routing.

## Architecture

```
     DEX                        CHIME                       TBC (ours)
  ┌─────────┐              ┌─────────┐                 ┌─────────────┐
  │ Compute  │              │ Compute  │                 │   Compute    │
  │          │              │          │                 │              │
  │ LeanStore│              │TreeCache │                 │  TrieCache   │
  │ Buffer   │              │(skiplist)│                 │  (radix trie)│
  │ Pool     │              │          │                 │      ↓       │
  │ (swizzle │              │ IdxCache │                 │  Bitmap Leaf │
  │  tags)   │              │ (hash)   │                 │  Directory   │
  └────┬─────┘              └────┬─────┘                 └──────┬──────┘
       │                         │                              │
    RDMA per                  RDMA for                    ZERO RDMA on
    inner level               VALOCK bitmap               hot path!
       │                    + RDMA for leaf                    │
       ↓                         ↓                        1 RDMA on miss
  ┌─────────┐              ┌─────────┐                 ┌─────────────┐
  │ Memory   │              │ Memory   │                 │   Memory     │
  │ Node     │              │ Node     │                 │   Node       │
  │          │              │ VALOCK   │                 │              │
  │ B+-tree  │              │ bitmap ← │ PROBLEM        │   B+-tree    │
  │ pages    │              │ B+-tree  │                 │   pages      │
  └──────────┘              └──────────┘                 └──────────────┘
```

### Lookup Flow Comparison

| Step | DEX | CHIME | TBC |
|------|-----|-------|-----|
| 1 | Check swizzle tag | Skip-list lookup | **Trie lookup** (local) |
| 2 | If miss: RDMA read inner node | RDMA read VALOCK bitmap | **Bitmap check** (local) |
| 3 | Repeat for each B-tree level | RDMA read leaf entries | If both hit: **local search** |
| 4 | At leaf: cache-get | Search entries | If miss: 1 RDMA read |
| Hot-path RDMA | 0 (if all cached) | 1+ (always for bitmap) | **0** |
| Cold-path RDMA | 1 per level | 2+ (bitmap + entries) | height + 1 |

## Components

| File | Description | Reuses from |
|------|-------------|-------------|
| `TrieNode.h` | 256-way radix trie for key→leaf routing | CHIME eviction pattern |
| `BitmapLeafDirectory.h` | Set-associative leaf cache with bitmap | CHIME Bitmap.h pattern |
| `TrieBitmapCache.h` | Integrated cache manager | — |
| `TrieBitmapTree.h` | `tree_api<Key,Value>` implementation | DEX node format, DSM |
| `tbc_bench.cpp` | Benchmark driver | DEX newbench.cpp |

## What We Reuse (NOT Reinvented)

From **DEX** (via include paths + linking against libDEX):
- `DSM` class — all RDMA operations (read_sync, write_sync, alloc, CAS)
- `BTreeInner<Key>` / `BTreeLeaf<Key, Value>` — remote B-tree page format
- `tree_api<Key, Value>` — benchmark interface
- `GlobalAddress` — 64-bit packed {nodeID:16, offset:48}
- `Common.h` — Key, Value types + constants
- Workload generators (zipf.h, uniform.h)

From **CHIME** (concepts, not code):
- Bitmap free-slot finding via `__builtin_ctz` (Bitmap.h pattern)
- Two-random-choice LFU eviction (TreeCache/IdxCache pattern)
- Vacancy bitmap concept (pulled from memory node to compute side)

## Building

```bash
# Prerequisites: same as DEX (ibverbs, memcached, cityhash, boost, tbb, numa)
cd trie_bitmap_cache
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc) tbc_bench tbc_unit_test
```

## Quick Start (2-Node Cluster)

### Step 1: Reset Memcached (on memory node)
```bash
./reset_memcached.sh 10.30.1.9 11211
```

### Step 2: Start Memory Node (Node 0)
```bash
./run_node0_memory.sh [cache_mb] [threads]
# Example: ./run_node0_memory.sh 256 16
```

### Step 3: Start Compute Node (Node 1) - within 30 seconds
```bash
./run_node1_compute.sh [cache_mb] [threads] [zipfian]
# Example: ./run_node1_compute.sh 256 16 0.99
```

### Alternative: Combined Script
```bash
# On memory node:
./run_tbc.sh node0

# On compute node:
./run_tbc.sh node1

# Reset only:
./run_tbc.sh reset
```

## Scripts Reference

| Script | Description |
|--------|-------------|
| `reset_memcached.sh` | Kill all, flush memcached, initialize keys |
| `run_node0_memory.sh` | Full reset + start memory server |
| `run_node1_compute.sh` | Start benchmark (no memcached reset) |
| `run_tbc.sh` | Combined script with `node0/node1/reset` commands |
| `build_and_run.sh` | Build only or build+run |

## Directory Structure

```
trie_bitmap_cache/
├── CMakeLists.txt           # Builds against DEX's source
├── run_tbc.sh               # Combined launcher script
├── run_node0_memory.sh      # Memory node script
├── run_node1_compute.sh     # Compute node script
├── reset_memcached.sh       # Memcached reset utility
├── build_and_run.sh         # Build + optional run
├── README.md                # This file
├── include/
│   ├── TrieNode.h           # 256-way radix trie for routing
│   ├── ARTCache.h           # Adaptive Radix Tree (NODE_4/16/48/256)
│   ├── BitmapLeafDirectory.h# Compute-side bitmap leaf cache
│   ├── TrieBitmapCache.h    # Integrated cache manager
│   ├── TrieBitmapTree.h     # tree_api implementation
│   └── CHIMEAdapter.h       # Drop-in CHIME compatibility layer
└── test/
    ├── tbc_bench.cpp        # Benchmark (DEX-compatible format)
    └── tbc_unit_test.cpp    # Unit tests (no RDMA required)
```

## New Components

| File | Description | Origin |
|------|-------------|--------|
| `ARTCache.h` | Adaptive Radix Tree with NODE_4→16→48→256 growth | SMART RadixCache pattern |
| `CHIMEAdapter.h` | Drop-in replacement for CHIME's TreeCache/IdxCache | Integration layer |

## Key Architectural Differences

### CHIME's Problem: Memory-Side Bitmap

```
CHIME Lookup Flow:
  1. TreeCache skip-list lookup → find inner node (local)
  2. RDMA read VALOCK bitmap    → 1 RTT ❌
  3. RDMA read leaf entries     → 1 RTT
  Total: 2+ RDMA round trips minimum
```

### TBC's Solution: Compute-Side Bitmap

```
TBC Lookup Flow:
  1. TrieCache lookup      → find leaf address (local)
  2. BitmapLeafDirectory   → check cached page (local)
     HIT:  local search    → 0 RTT ✅
     MISS: RDMA read leaf  → 1 RTT
  Total: 0-1 RDMA round trips
```

### Why This Matters

| Metric | CHIME | TBC |
|--------|-------|-----|
| Hot-path RDMA | 2+ | **0** |
| Cold-path RDMA | 2+ | height+1 |
| Bitmap location | Memory node | **Compute node** |
| Cache structure | Skip-list | **Radix trie** |
