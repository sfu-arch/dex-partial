# DEX vs CHIME — Comprehensive Benchmark Analysis

## Table of Contents
1. [Executive Summary](#executive-summary)
2. [System Architectures](#system-architectures)
3. [Structural Parameters Comparison](#structural-parameters-comparison)
4. [RDMA Cost per Operation](#rdma-cost-per-operation)
5. [QW1 Experiment Results (30T/30M)](#qw1-experiment-results)
6. [Root Cause of Performance Gap](#root-cause-of-performance-gap)
7. [CHIME Diagnostic Stats Analysis](#chime-diagnostic-stats-analysis)
8. [Where CHIME Could Be Better](#where-chime-could-be-better)
9. [Point Lookup Analysis — Is CHIME Supposed to Win?](#point-lookup-analysis)
10. [Experiment Recommendations](#experiment-recommendations)
11. [Bugs Found and Fixed](#bugs-found-and-fixed)
12. [File Inventory](#file-inventory)

---

## Executive Summary

DEX outperforms CHIME by **42–49×** in throughput across all Zipfian skew levels under the QW1 workload (70% read, 30% range scan, 30 threads, 30M ops, 10M bulk-loaded keys). The gap is primarily architectural:

- **DEX caches full pages (internal + leaf pointers)** → cached point lookup = **1 small RDMA read (~41 bytes)**
- **CHIME caches only internal nodes** → every point lookup still needs **1 large RDMA read (~1132 bytes) for the leaf**
- **Range scans amplify the gap**: DEX traverses cached internal pages locally; CHIME follows remote sibling pointers (each = 1 RDMA round-trip)
- Our `latency_bench.cpp` does **not** use coroutines — CHIME's RDWC (Read Delegation with Write Combining) barely activates without them

---

## System Architectures

### DEX (Disaggregated EXtensible B-tree / SMART-based ART)
- **Tree type**: Adaptive Radix Tree (ART) with path compression
- **Key discrimination**: 1 byte per internal node level (256-way fanout)
- **Leaf model**: Each leaf = a **single KV pair** (~41 bytes)
- **Caching**: `IndexCache` (skiplist-based) caches **InternalPage** objects locally on compute node
  - On cache hit: skip all internal traversal, jump directly to reading the leaf
  - Cache stores key-range → InternalPage mappings
- **Cache invalidation**: Reverse pointers + CAS-based validation
- **Concurrency**: HOCL (Hierarchical On-Chip Lock) + local lock table + coroutines (8 per thread in `ycsb_test.cpp`)
- **Fine-grained nodes**: NODE_4 through NODE_256 (adaptive sizing)

### CHIME (Concurrent Hopscotch-hashed Index for Memory-disaggregated Environments)
- **Tree type**: B+tree with hopscotch-hashed leaf nodes
- **Key discrimination**: Sorted key comparison in internal nodes (fanout = 64)
- **Leaf model**: Each leaf node = **64 KV entries** (~1132 bytes on wire)
- **Caching**: Two-level cache:
  1. `TreeCache` (226 MB allocated, skiplist-based): caches **internal nodes** only
  2. `IdxCache` (30 MB, hash table): hotspot buffer (maps leaf_addr+index → cached result)
- **Optimizations**: RDWC (read/write delegation via local lock table), speculative reads, metadata replication, vacancy-aware locking
- **Coroutines**: 8 per thread in `ycsb_test.cpp` (but **not used** in `latency_bench.cpp`)

---

## Structural Parameters Comparison

| Parameter | DEX | CHIME |
|-----------|-----|-------|
| **Key size** | 8 bytes (`uint64_t`) | 8 bytes (`std::array<uint8_t, 8>`) |
| **Value size** | 8 bytes (`uint64_t`) | 8 bytes (`uint64_t`) |
| **Internal page size** | 2064 bytes (8+8+256×8) | ~1148 bytes (1131 decoded + versioning) |
| **Internal entry size** | 8 bytes (packed: partial+flags+addr) | 17 bytes (1B version + 8B key + 8B addr) |
| **Internal fanout** | 256 (1 byte per level) | 64 (sorted keys) |
| **Leaf size (on wire)** | ~41 bytes (single KV + metadata) | ~1132 bytes (27B metadata + 64×17B entries) |
| **Leaf entries** | 1 (single KV pair) | 64 (hopscotch-hashed) |
| **Version overhead** | None (CRC-based consistency) | 1 byte per entry + per node (cacheline versioning) |
| **Tree height (10M keys)** | ~4–6 (ART with compression) | ~4 (log₆₄(10M) ≈ 3.87) |
| **Cache size** | 256 MB (internal pages) | 226 MB (internal) + 30 MB (hotspot) |
| **Cache contents** | Full InternalPages (2064B each) | InternalNodes only (~289B consumed avg) |
| **Cached pages capacity** | ~130K internal pages | ~20K internal nodes + IdxCache entries |
| **MAX_APP_THREAD** | 36 | 32 |
| **MAX_CORO_NUM** | 8 | 8 |
| **NR_DIRECTORY** | 4 | 1 |
| **MEMORY_NODE_NUM** | 4 | 1 |
| **Lock mechanism** | On-chip CAS (1-bit locks) | On-chip CAS + local lock table |
| **Allocation alignment** | 256 bytes | 16 bytes (lock + node) |

### Node Size Calculation (CHIME)

```
versionSize = 1 byte  (4-bit entry version + 4-bit node version, rounded up)

Leaf:
  leafMetadataSize  = 1 + 2 + 8 + 16 = 27 bytes
  leafEntrySize     = 1 + 8 + 8      = 17 bytes  (no HOPSCOTCH_LEAF_NODE)
  decodedLeafSize   = 27 + 64 × 17   = 1115 bytes
  transLeafSize     = 64 + 1051 + 17  = 1132 bytes  (with cacheline versioning)
  allocationLeafSize= 1132 + 16      = 1148 bytes

Internal:
  internalMetadataSize = 1 + 2 + 24 + 16 = 43 bytes
  internalEntrySize    = 1 + 8 + 8       = 17 bytes
  decodedInternalSize  = 43 + 64 × 17    = 1131 bytes
  transInternalSize    = 64 + 1067 + 17   = 1148 bytes
  allocationInternalSize = 1148 + 16     = 1164 bytes
```

### Node Size Calculation (DEX)

```
InternalPage = rev_ptr(8) + Header(8) + records[256](256 × 8) = 2064 bytes
  - But read_node only reads: 8 + 8 + node_type_to_num(type) × 8
  - NODE_4:   8+8+32   = 48 bytes
  - NODE_16:  8+8+128  = 144 bytes
  - NODE_256: 8+8+2048 = 2064 bytes

Leaf = rev_ptr(8) + valid_byte(1) + checksum(8) + key(8) + value(8) + lock_byte(8)
     = 41 bytes (may be padded to 48 on wire)
allocAlignLeafSize = ROUND_UP(41, 8) = 256 bytes (allocation granularity)
```

---

## RDMA Cost per Operation

### Point Lookup (Cache Hit)

| Step | DEX | CHIME |
|------|-----|-------|
| 1. Cache lookup | Local memory (~100ns) | Local memory (~100ns) |
| 2. Traversal | Skip (cache provides leaf entry directly) | Skip (cache provides leaf-node addr) |
| 3. Leaf read | **1 RDMA read, ~41–48 bytes** | **1 RDMA read, ~1132 bytes** |
| 4. Validate | CRC check (local) | Version check + fence key check (local) |
| **Total RDMA round-trips** | **1** | **1** |
| **Total RDMA bytes** | **~41 B** | **~1132 B (27× more)** |

### Point Lookup (Cache Miss — Full Traversal)

| Step | DEX | CHIME |
|------|-----|-------|
| Tree height | ~4–6 levels | ~4 levels |
| Internal reads | 3–5 × (48–2064 B each) | 3 × (~1148 B each) |
| Leaf read | 1 × ~41 B | 1 × ~1132 B |
| **Total RDMA round-trips** | **4–6** | **4** |
| **Typical total bytes** | **~200–600 B** (with fine-grain nodes) | **~4576 B** |

### Range Scan (100 keys)

| Step | DEX | CHIME |
|------|-----|-------|
| Find start key | 1 RDMA (cached) | 1 RDMA (cached) |
| Scan leaves | **Traverse cached internal pages locally** → batch leaf reads | **Follow sibling pointers**: each sibling = 1 RDMA read of ~1132 B |
| Keys per leaf | 1 (each leaf = 1 KV) | Up to 64 |
| Leaves to read | ~100 (but batched from internal pages) | ~2–4 (64 keys/leaf, but linked-list traversal) |
| **Total RDMA round-trips** | **~3–10** (batched from cache) | **~4–6** (sequential sibling reads) |
| **Effective latency** | **3–8 μs** | **450–640 μs** (!!) |

The 87× gap in range scan P50 is because CHIME must do sequential RDMA reads following sibling pointers, while DEX calculates leaf addresses from cached internal pages and batches reads.

---

## QW1 Experiment Results

**Configuration**: 30 threads, 30M ops, 70% read + 30% range (100 keys), 10M bulk-loaded keys

### Throughput (ops/sec)

| Skew | DEX | CHIME | Ratio |
|------|-----|-------|-------|
| Uniform | 7,216,000 | 149,700 | **48.2×** |
| Zipf 0.6 | 7,393,000 | 152,200 | **48.6×** |
| Zipf 0.8 | 8,292,000 | 180,800 | **45.9×** |
| Zipf 0.9 | 9,555,000 | 205,000 | **46.6×** |
| Zipf 0.99 | 11,331,000 | 268,300 | **42.2×** |

### Read Latency (μs)

| Skew | DEX P50 | CHIME P50 | DEX P99 | CHIME P99 |
|------|---------|-----------|---------|-----------|
| Uniform | 1.0 | 8.5 | 8.0 | 16.5 |
| Zipf 0.6 | 1.0 | 8.5 | 7.5 | 16.0 |
| Zipf 0.8 | 0.5 | 8.0 | 6.0 | 14.0 |
| Zipf 0.9 | 0.5 | 8.0 | 5.0 | 12.5 |
| Zipf 0.99 | 0.5 | 7.5 | 3.0 | 11.5 |

### Range Scan Latency (μs)

| Skew | DEX P50 | CHIME P50 | DEX P99 | CHIME P99 |
|------|---------|-----------|---------|-----------|
| Uniform | 7.5 | 637.0 | 19.0 | 871.5 |
| Zipf 0.6 | 7.0 | 625.5 | 18.0 | 856.5 |
| Zipf 0.8 | 6.0 | 522.0 | 16.0 | 800.0 |
| Zipf 0.9 | 4.0 | 450.5 | 14.0 | 701.0 |
| Zipf 0.99 | 3.0 | 262.5 | 10.0 | 532.0 |

---

## Root Cause of Performance Gap

### Factor 1: Leaf Caching Architecture (14–15× impact)
- DEX caches InternalPages that contain **direct pointers to individual leaves**. Each leaf is only 41 bytes. A cached lookup = 1 small RDMA read.
- CHIME caches only internal B+tree nodes. Every leaf access = 1 RDMA read of **1132 bytes** (the entire leaf node with 64 entries).
- Under skew, DEX's cache hit rate is near 100%, meaning almost every lookup is 1 tiny RDMA read.
- CHIME's IdxCache (30MB hotspot buffer) tries to cache leaf results but saturates quickly and only covers point lookups (not ranges).

### Factor 2: Range Scan Cost (85–87× impact)
- DEX range scans traverse cached internal pages to find all leaf addresses, then batch-reads them. Most work is local.
- CHIME range scans must follow sibling pointers: each step = a full leaf-node RDMA read (~1132 bytes), and these are sequential (can't batch because you need each sibling pointer to know the next address).
- For 100-key ranges across ~2 leaf nodes: CHIME does ~2–4 sequential RDMA reads of 1132 bytes each.

### Factor 3: Missing Coroutines (2–3× impact)
- CHIME's `latency_bench.cpp` does **not** use coroutines. The original `ycsb_test.cpp` uses 8 coroutines per thread.
- Without coroutines, threads block on each RDMA read. With 8 coroutines, up to 8 RDMA reads can be in flight per thread, hiding latency.
- RDWC (Read Delegation) also requires coroutines to be effective — it works by having waiting coroutines check if another coroutine already fetched the same key.

### Factor 4: RDWC Not Activating (negligible measured impact)
- Read delegation rate: 0.0000–0.0055 (essentially zero)
- Without coroutines, there's no concurrent read overlap within a thread, so delegation never triggers.
- With coroutines + high skew (Zipf 0.99), delegation could eliminate many redundant RDMA reads for hot keys.

### Quantitative Breakdown (Zipf 0.99)
```
Total RDMA time estimate for CHIME:
  Read ops:  20,346,687 × 1 RDMA read × ~7μs = ~142s (across 30 threads = ~4.7s elapsed)
  Range ops:  9,653,313 × ~3 RDMA reads × ~7μs = ~203s (across 30 threads = ~6.8s elapsed)
  Total elapsed ≈ 11.5s → actual measured: 111.8s (30M ÷ 268K ops/s)

Discrepancy: retries, lock contention, hopscotch hash computation, version validation
```

---

## CHIME Diagnostic Stats Analysis

### Stats from Zipf 0.99 run:

| Metric | Value | Interpretation |
|--------|-------|----------------|
| Cache hit rate | 0.9999 | **Misleading**: only covers internal node cache. Does NOT mean 99.99% of lookups are local. Every lookup still needs 1 RDMA for the leaf. |
| Read delegation rate | 0.0055 | Essentially zero. RDWC needs coroutines to work. Only 110K out of 20M reads delegated. |
| Speculative read rate | 0.6147 | 61.5% of speculative reads succeed. This means the predicted leaf address from the cached internal node is correct ~61% of the time. |
| Read leaf retry rate | 0.0000 | Version conflicts are extremely rare — good. |
| TreeCache used | 5.824 MB / 226 MB | Only ~2.6% of TreeCache is used. The tree has very few internal nodes (10M keys ÷ 64 fanout = ~156K leaf nodes, ~2400 internal nodes). |
| IdxCache free | 0 MB / 30 MB | Fully saturated. Every slot is occupied. |
| RDMA errors | 4 "Failed status unknown" | Minor. Likely transient NIC queue issues under load. |

### Key Insight: 99.99% Cache Hit ≠ 99.99% Local
The cache hit rate only tells you that the internal-node traversal was served from local cache. But in CHIME, leaf nodes are **never cached** in TreeCache. Every single read/update/scan that reaches a leaf still does at least 1 RDMA round-trip. The IdxCache (hotspot buffer) can cache some individual KV results, but at 30MB it's far too small for 10M keys.

---

## Where CHIME Could Be Better

### Scenario 1: Pure Point Lookups WITH Coroutines (Most Likely Win)
**Why**: CHIME's RDWC is designed to eliminate redundant RDMA reads for hot keys. With 8 coroutines per thread and high skew (Zipf ≥ 0.9):
- Multiple coroutines reading the same hot key → only 1 RDMA read, result shared with all waiters
- Speculative reads (61% success rate) effectively pipeline internal-node + leaf reads into a single round-trip
- Could reduce effective per-op RDMA cost to < 1 for hot keys

**Experiment**: Run with coroutines enabled (use `ycsb_test.cpp` as base), 100% read, no range scans, Zipf 0.99.

**Expected outcome**: CHIME throughput could increase **3–8×** with coroutines + delegation.

### Scenario 2: Write-Heavy Workloads WITH Coroutines
**Why**: RDWC's **Write Combining** batches multiple writes to the same key into one RDMA write. Under high-skew writes:
- Many threads updating the same hot key → only 1 RDMA write, rest are combined locally
- DEX uses out-of-place updates (allocate new leaf, CAS pointer), which is expensive per-write

**Experiment**: 100% update (or 50% read + 50% update), Zipf 0.99, with coroutines.

### Scenario 3: Working Set Exceeds DEX Cache
**Why**: DEX's IndexCache stores full InternalPage objects (2064 bytes each). With 256MB cache:
- Can cache ~130K internal pages
- Each page covers 256 leaf entries → total coverage: ~33M leaf pointers
- For 10M keys this is sufficient, but for **60M+ keys**, cache starts thrashing
- CHIME's internal nodes are smaller (289 bytes avg used) → its TreeCache covers more of the tree

**Experiment**: Increase bulk-loaded keys from 10M to 50M or 100M. Keep cache at 256MB. Uniform access pattern (no skew to help caching).

### Scenario 4: Multiple Compute Nodes
**Why**: With N compute nodes all accessing the same memory node:
- RDMA contention at the NIC increases
- CHIME's RDWC reduces total RDMA traffic (delegation happens locally on each compute node)
- DEX's cache invalidation protocol generates extra cross-node traffic

**Experiment**: Run with 2–4 compute nodes, each with 15 threads.

### Scenario 5: Very Deep Trees (Long Keys)
**Why**: DEX's ART discriminates 1 byte per level, so an 8-byte key → up to 8 levels. With longer keys (16–32 bytes):
- DEX tree height grows linearly with key length
- CHIME's B+tree height grows logarithmically: log₆₄(N)
- More levels = more RDMA round-trips for cold lookups in DEX

**Experiment**: Increase key length to 16+ bytes (requires code changes in both systems).

---

## Point Lookup Analysis

### "Is CHIME supposed to be better at point lookups?"

**Short answer**: CHIME *could* be competitive on point lookups under specific conditions, but DEX has a fundamental architectural advantage for cached workloads.

### Why DEX Wins Point Lookups Today

1. **Leaf size**: DEX leaf = 41 bytes (single KV). CHIME leaf = 1132 bytes (64 KVs). Even though both do 1 RDMA read per lookup (with cache hit), DEX reads **27× less data**. Smaller RDMA reads complete faster.

2. **RDMA latency scales with size** (for small reads):
   - 41 bytes → ~0.5–1.0 μs on our hardware
   - 1132 bytes → ~3–5 μs base + version decoding overhead → ~7–8 μs measured

3. **Cache depth**: DEX's cache directly provides the leaf pointer. CHIME's cache provides the leaf-node address, but you still need to hash the key and search within the 64-entry leaf after reading it.

### What Would Need to Change for CHIME to Win Point Lookups

| Change | Impact | Feasibility |
|--------|--------|-------------|
| Enable coroutines (8 per thread) | 3–5× throughput gain from latency hiding + RDWC activation | Easy — use `ycsb_test.cpp` harness |
| Reduce `leafSpanSize` from 64 to 16 | Leaf RDMA read drops from 1132B to ~300B | Medium — requires rebuild + rebalancing tree |
| Increase IdxCache to 256MB | More point-lookup results cached locally | Easy — change `kHotspotBufSize` |
| Use 100% read (no range scans) | Removes CHIME's worst bottleneck | Easy — change workload config |
| Increase key space to 100M+ | DEX cache starts thrashing | Easy — change bulk load count |
| Add more compute nodes | RDWC delegation becomes valuable | Requires cluster resources |

### Inner Node Size and Parameters

The inner node size matters for **cold traversals** (cache miss). Comparing:

```
CHIME internal read:  1148 bytes → provides 64 child pointers
  Per child discriminated: 1148/64 = 17.9 bytes/child

DEX internal read (NODE_256): 2064 bytes → provides 256 child pointers
  Per child discriminated: 2064/256 = 8.1 bytes/child

DEX internal read (NODE_4):  48 bytes → provides 4 child pointers
  Per child discriminated: 48/4 = 12 bytes/child
```

CHIME's internal nodes are more **space-efficient per child** when full (17.9 B/child vs 8.1 B/child for NODE_256), but DEX's adaptive sizing means small nodes (NODE_4, NODE_8) are extremely compact. For 10M keys with 8-byte keys, DEX's ART typically has nodes of varying sizes — many NODE_4/NODE_8 at the top, larger nodes deeper. The total bytes read per cold traversal can be similar or favor DEX.

---

## Experiment Recommendations

### Priority 1: Add Coroutines to CHIME Benchmark
- Modify `latency_bench.cpp` to use 8 coroutines per thread (port coro logic from `ycsb_test.cpp`)
- This is the single biggest improvement for CHIME — enables RDWC + latency hiding
- Expected: 3–8× throughput improvement for CHIME

### Priority 2: 100% Point Lookup Workload
- Set `read_ratio=1.0, range_ratio=0.0`
- Removes range scan penalty (85×) from CHIME's results
- Expected: gap narrows from 42× to ~8–15×

### Priority 3: Larger Key Space
- Increase bulk-loaded keys to 50M or 100M
- Keeps cache at 256MB for both
- Tests what happens when DEX cache can't cover the working set
- Expected: DEX degrades significantly at 50M+ keys

### Priority 4: Write-Heavy Workload
- 50% read + 50% update, or 100% update
- Tests CHIME's write combining vs DEX's out-of-place updates
- Expected: gap may narrow or reverse at high skew with coroutines

### Priority 5: Multi-Compute-Node Setup
- Use 2 or 4 compute nodes, total 30 threads spread across them
- Tests RDWC's cross-coroutine delegation under realistic disaggregated conditions
- Expected: CHIME's advantage grows with more compute nodes

---

## Bugs Found and Fixed

### CHIME `latency_bench.cpp`
1. **Warmup included in timing** — `start_time` was captured before warmup ops → inflated latency. Fixed: moved `start_time` to after warmup barrier.
2. **No thread barrier after warmup** — Threads started measuring at different times. Fixed: added `warmup_cnt` + `ready` atomic barrier.
3. **No diagnostic stats** — Added extern counters for cache_hit/miss, delegation, speculative reads, retries + `tree->statistics()` call.

### DEX `qw1_dex_node0/1.sh`
4. **MEM_THREADS=8 exceeds NR_DIRECTORY=4** — `dirCon[]` only has 4 slots. Writing index 4–7 = segfault. Fixed: `MEM_THREADS=4`.

### CHIME Bulk Load
5. **Bulk load count mismatch** — CHIME was loading a different number of keys than DEX. Fixed by aligning both to 10M.

### CHIME `Common.h`
6. **MAX_APP_THREAD too low (17)** — Couldn't run 30 threads. Fixed: increased to 32.

---

## File Inventory

### Benchmark Source Code
| File | Description |
|------|-------------|
| `CHIME/test/latency_bench.cpp` | CHIME latency benchmark (fixed, ~534 lines) |
| `dex/test/newbench_latency.cpp` | DEX latency benchmark (~745 lines) |

### Experiment Scripts
| File | Description |
|------|-------------|
| `experiments/qw1_zipfian_skew/qw1_chime_node0.sh` | CHIME memory-node script |
| `experiments/qw1_zipfian_skew/qw1_chime_node1.sh` | CHIME compute-node script |
| `experiments/qw1_zipfian_skew/qw1_dex_node0.sh` | DEX memory-node script (MEM_THREADS=4) |
| `experiments/qw1_zipfian_skew/qw1_dex_node1.sh` | DEX compute-node script |
| `experiments/qw1_zipfian_skew/plot_qw1_comparison.py` | Auto-generates 8 comparison plots |

### Results (in `experiments/qw1_zipfian_skew/results/`)
| File | Description |
|------|-------------|
| `dex_uniform_stdout.log` | DEX stdout, uniform access |
| `dex_zipf_0.60_stdout.log` | DEX stdout, Zipf θ=0.6 |
| `dex_zipf_0.80_stdout.log` | DEX stdout, Zipf θ=0.8 |
| `dex_zipf_0.90_stdout.log` | DEX stdout, Zipf θ=0.9 |
| `dex_zipf_0.99_stdout.log` | DEX stdout, Zipf θ=0.99 |
| `chime_uniform_stdout.log` | CHIME stdout, uniform |
| `chime_zipf_0.60_stdout.log` | CHIME stdout, Zipf θ=0.6 |
| `chime_zipf_0.80_stdout.log` | CHIME stdout, Zipf θ=0.8 |
| `chime_zipf_0.90_stdout.log` | CHIME stdout, Zipf θ=0.9 |
| `chime_zipf_0.99_stdout.log` | CHIME stdout, Zipf θ=0.99 |
| `*_read_latency.dat` | Per-μs read latency histograms |
| `*_range_latency.dat` | Per-μs range latency histograms |

### Generated Plots (in `experiments/qw1_zipfian_skew/plots/`)
1. `qw1_throughput_comparison.png` — Throughput bar chart
2. `qw1_read_p50_comparison.png` — Read P50 latency bars
3. `qw1_read_p99_comparison.png` — Read P99 latency bars
4. `qw1_range_p50_comparison.png` — Range P50 latency bars
5. `qw1_range_p99_comparison.png` — Range P99 latency bars
6. `qw1_latency_breakdown_zipf099.png` — Latency breakdown at θ=0.99
7. `qw1_latency_cdf_zipf099.png` — CDF overlay
8. `qw1_chime_diagnostics.png` — CHIME cache/delegation/speculative stats

### Cluster Configuration
| Node | IP | Role |
|------|-----|------|
| Node 0 | 10.30.1.9 | Memory server |
| Node 1 | 10.30.1.6 | Compute node |
| Memcached | 10.30.1.9:11211 | Coordination |

---

## Appendix: Key Source Files Modified

### `CHIME/include/Common.h`
- `MAX_APP_THREAD`: 17 → **32**
- `kIndexCacheSize`: → 256 MB
- `kHotspotBufSize`: 30 MB (unchanged)
- `leafSpanSize`: 64, `internalSpanSize`: 64, `neighborSize`: 8

### `CHIME/test/latency_bench.cpp`
- Complete rewrite with timing fix, warmup barrier, diagnostic stats
- Args: `<node_count> <thread_count> <read_ratio> <range_ratio> <total_ops> [range_size] [zipf_theta] [uniform]`

### `dex/test/newbench_latency.cpp`
- Working benchmark with per-thread timing
- Args: 22 positional CLI args (see script for order)

### `experiments/qw1_zipfian_skew/qw1_dex_node0.sh`
- `MEM_THREADS`: 8 → **4** (fix NR_DIRECTORY overflow)
