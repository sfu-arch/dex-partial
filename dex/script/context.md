# DEX Codebase — Build & Experiment Context

Everything built and understood across this session.

---

## 1. What DEX Is

DEX is a B+ tree designed for **disaggregated memory** (compute and memory in separate server pools connected via RDMA/InfiniBand). The paper: *"DEX: Scalable Range Indexing on Disaggregated Memory"* (VLDB 2024).

Three key mechanisms vs prior work (Sherman, SMART):

| Mechanism | DEX approach | Prior work gap |
|---|---|---|
| **Caching** | Path-aware, caches both inner + leaf nodes; random-sample eviction via cooling map | Sherman caches inner only; SMART uses centralized FIFO (contention bottleneck) |
| **Offloading** | Cost-aware: offloads to memory-side CPU only when `latency_offload < latency_rdma` | Sherman/SMART: no offloading |
| **Consistency** | Logical partitioning (each compute server owns a key range) → RDMA locks only at partition boundaries | All prior: distributed RDMA locks everywhere |

---

## 2. Cluster Layout

```
cs-dis-srv09s  (compute node, node 0)
    runs: newbench_latency (client side), run_bp_sweep.sh, memcached
    IP:   10.30.1.8 (approx)

10.30.1.9      (memory node, node 1)
    runs: newbench_latency (server side), run_memnode_sweep.sh
    memcached hosted here (port 11211)
    DSM: 8 GB RDMA-exposed memory pool

Interconnect: InfiniBand RDMA
    one-sided READ: ~2 µs latency
    two-sided RPC:  higher latency, used for offloading
```

Memcached is used only for **rendezvous** (nodes register `serverNum`/`clientNum` counters before the benchmark starts). It is NOT involved in the hot path.

---

## 3. Codebase Map

```
dex/
├── include/
│   ├── cache/
│   │   ├── btree_node.h        ← B+ tree node structs + PAGE SIZE CONFIG
│   │   ├── leanstore_cache.h   ← CacheManager: eviction, admission, RPC decision
│   │   └── latency_collector.h
│   ├── tree/
│   │   └── leanstore_tree.h    ← BTree class (lookup/insert/range/offload logic)
│   ├── tree_api.h              ← Abstract interface (all indexes implement this)
│   ├── sherman_wrapper.h       ← Sherman adapter
│   └── smart/
│       └── smart_wrapper.h     ← SMART adapter
├── src/
│   ├── DSM.cpp / DSM.h         ← Disaggregated Shared Memory layer (RDMA)
│   └── Tree.cpp                ← Sherman tree implementation
├── test/
│   ├── newbench.cpp            ← Throughput benchmark (used for quick iteration)
│   ├── newbench_latency.cpp    ← Latency + throughput benchmark (used for sweep)
│   ├── zipf.h                  ← Zipf distribution generator (mehcached)
│   └── uniform_generator.h     ← Uniform distribution generator
└── script/
    ├── run_bp_sweep.sh         ← Compute node sweep script (MAIN)
    ├── run_memnode_sweep.sh    ← Memory node sweep script (mirrors compute)
    ├── RUN_COMMANDS.md         ← Step-by-step execution guide
    ├── context.md              ← This file
    ├── restartMemc.sh          ← Resets memcached counters (SSH-based, partially broken)
    └── run.sh / run_other.sh   ← Original paper scripts (reference only)
```

---

## 4. Node Size Configuration

**Location:** `include/cache/btree_node.h` lines 42–45

```cpp
static const uint64_t innerNodeSize = 256;   // controls inner-node fanout
static const uint64_t leafNodeSize  = 512;   // controls leaf capacity
static const uint64_t pageSize =             // RDMA unit = max of the two
    (innerNodeSize >= leafNodeSize) ? innerNodeSize : leafNodeSize;
```

**How maxEntries is computed:**

```
BTreeInner::maxEntries = (innerNodeSize - 72) / 16
    = (256 - 72) / 16 = 11 entries (fanout 11)
    struct size = 64(NodeBase) + 8×11(children) + 8×11(keys) + 8 + 8 = 256 B ✓

BTreeLeaf::maxEntries = (leafNodeSize - 96) / 16
    = (512 - 96) / 16 = 26 entries
    struct size = 64(NodeBase) + 16×26(data) + 8 + 8 + 16 = 512 B ✓
```

**Why inner=256, leaf=512 gives depth=10 with 50M keys:**

Sequential eager-split bulk load gives ~50% fill on average:
- Effective leaf fill ≈ 13 entries → leaf nodes ≈ 50M/13 = **3.85M**
- Effective inner fanout ≈ 5.5 → height ≈ log₅.₅(3.85M) + 1 ≈ **10** ✓

The original `pageSize=1024` (fanout≈59, leaf cap≈58) gave depth=5 with 50M keys.
`pageSize=192` (fanout=7, leaf cap=6) was tried first but gave depth=16 because
sequential inserts create very unbalanced trees with small nodes (≈50% fill at each level).

**Rule of thumb:** `height ≈ log_(fanout/2)(N / (leaf_cap/2)) + 1`

---

## 5. NodeBase Layout (64 bytes = 1 cache line)

```cpp
struct NodeBase : OptLock {        // OptLock = atomic<uint64_t> version lock
    uint64_t front_version;        // consistency check (read before data)
    GlobalAddress remote_address;  // [swizzle(1b) | server_id(15b) | addr(48b)]
    uint64_t bitmap;               // tracks swizzled child pointers
    NodeBase *parent_ptr;          // pointer to parent in local DRAM cache
    PageType type;                 // BTreeInner=1, BTreeLeaf=2
    uint8_t count;                 // number of keys currently stored
    uint8_t level;                 // 0=leaf, 1=lowest inner, etc.
    uint8_t pos_state;             // 0=remote, 1=cooling, 2=hot, 3=working, 4=pinned
    bool obsolete, shared, is_smo, dirty;
    Key min_limit_, max_limit_;    // fence keys for staleness detection
};                                 // total = 64 bytes exactly
```

---

## 6. Cache Architecture

**CacheManager** (`leanstore_cache.h`) manages a flat pool of `pageSize`-byte frames in local DRAM.

Key mechanisms:

| Component | Detail |
|---|---|
| **Mapping table** | Concurrent hash table: `GlobalAddress → local frame pointer` |
| **Pointer swizzling** | Once a node is cached, its parent stores its LOCAL address (bit 63 set). Hot-path traversal is pure pointer chasing — no hash table lookup. |
| **Cooling map** | Hash table of per-bucket FIFO arrays. Random sample → cooling → eviction. Replaces centralized FIFO queue (which caused cache-line ping-pong at high replacement frequency). |
| **Path-aware eviction** | When an inner node is sampled for cooling, the cooling command is delegated DOWN to its deepest swizzled child. This keeps complete root→leaf paths cached together. |
| **Lazy admission** | `admission_rate=0.1` for leaf nodes: only 10% of fetched leaf nodes are admitted to cache. Inner nodes are always admitted (`PA=1`). Under uniform workload, most leaf fetches are one-shot RDMA reads that bypass the cache. |
| **I/O flag** | When a thread begins fetching a missing node, it sets an I/O flag in the mapping table. Concurrent threads seeing I/O re-traverse from cached root instead of issuing duplicate RDMA reads. |

**Stats tracked in CacheManager (now instrumented):**

```cpp
uint64_t inner_miss_ = 0;      // RDMA reads for inner nodes
uint64_t leaf_miss_  = 0;      // RDMA reads for leaf nodes
uint64_t full_page_miss_ = 0;  // full subtree misses
uint64_t rdma_write  = 0;      // dirty page writebacks
```

These are reset in `BTree::clear_statistic()` after warmup and printed every 2s
via `BTree::get_statistic()` → `[DEX] inner_miss=N leaf_miss=N dirty_wb=N total_remote=N`.

---

## 7. Benchmark Binaries

### `newbench_latency` (used for all sweep runs)

Adds per-operation latency histograms on top of the throughput benchmark:

```cpp
#define LATENCY_BUCKETS       100000   // max tracked latency
#define LATENCY_NS_GRANULARITY 500     // 500 ns per bucket → max 50 ms
```

Per-thread histograms avoid atomic contention in hot path.
Output saved to `dex_read_latency.dat` / `dex_range_latency.dat` (moved by script).

**What was added to `newbench_latency` in this session:**
1. Periodic `tree->get_statistic()` call in the main wait loop (every 2s)
2. Full RDMA stats block at end (`Avg. rdma read / op`, `Avg. rdma read size / op`, etc.)
3. Final `tree->get_statistic()` snapshot before barrier

### `newbench` (kept for reference / quick tests)

Same logic without latency histogram tracking. Originally used for throughput-only sweep.

---

## 8. Workload Generation

**Key space:** `kKeySpace = bulk_load_num + 1000 = 50,001,000`

**Key transformation:** `to_key(k) = (CityHash64(&k, 8) + 1) % kKeySpace`
→ maps sequential integers to pseudo-random keys (avoids sequential access patterns in index).

**Distribution selection:**
```cpp
if (uniform_workload == 1)
    key = uniform_generator->next_id();   // uniform random in [0, kKeySpace)
else
    key = mehcached_zipf_next(&state);    // Zipf with theta
```

`uniform_workload=1` entirely ignores theta (theta still printed in config for reference).

**Operation encoding:** top 8 bits of the pre-generated 64-bit value encode op type;
lower 56 bits encode the key. Workload array is generated once and replayed.

**Marks (printed at start, looks confusing but is correct):**
```
insertmark  = read_ratio + insert_ratio   (cumulative upper bound)
updatemark  = insertmark + update_ratio
deletemark  = updatemark + delete_ratio
rangemark   = deletemark + range_ratio    (always == 100)
```
For 100% reads: all marks = 100. `random_num < kReadRatio` covers 0–99 → always reads.

---

## 9. What Was Changed in This Session

### `include/cache/btree_node.h`
- Split single `pageSize` into `innerNodeSize` + `leafNodeSize` + `pageSize = max(...)`
- Changed from `1024` → `innerNodeSize=256, leafNodeSize=512` for depth=10
- `BTreeInner::maxEntries` now uses `innerNodeSize`
- `BTreeLeaf::maxEntries` now uses `leafNodeSize`
- Added detailed comments on valid configs and depth formula

### `include/tree/leanstore_tree.h`
- `clear_statistic() override` — resets all 4 cache stats (was empty `{}`)
- `get_statistic() override` — prints `[DEX]` line (was not implemented)

### `test/newbench_latency.cpp`
- Added periodic `tree->get_statistic()` in the main wait loop
- Added full RDMA stats block (mirrors `newbench.cpp` lines 1055–1095)
- Added final `tree->get_statistic()` snapshot before exit

### `script/run_bp_sweep.sh`
- Switched from `newbench` (throughput only) to `newbench_latency` (everything)
- Full cache sweep (128/256/512 MB) for every distribution × workload type
- Added `flush_memc()` after each run (pkill + restart + nc counter reset)
- All output tee'd to `sweep_YYYYMMDD_HHMM.log`
- `MEMC_HOST=10.30.1.9` variable for easy configuration

### `script/run_memnode_sweep.sh` (new file)
- Mirrors the compute sweep exactly (same experiment order)
- `wait_for_reset()` polls `serverNum` via nc until compute's `flush_memc` resets it to 0
- `flush_counters()` resets nc counters only (no local memcached restart)
- Tee'd to `memnode_sweep_YYYYMMDD_HHMM.log`

---

## 10. Known Issues / Gotchas

| Issue | Detail |
|---|---|
| `restartMemc.sh` SSH fails | `ssh -p 404 10.30.1.9: Permission denied (publickey)`. The nc counter reset still succeeds. Use `flush_memc()` in the script instead of relying on SSH. |
| Sequential bulk load → unbalanced tree | Eager splits + sequential keys → ~50% node fill. Tree height is determined by `height ≈ log_(f/2)(N/(cap/2))`, not the theoretical `log_f(N)`. With `innerNodeSize=192, leafNodeSize=192` this gave depth=16 not 10. Fixed by using `innerNodeSize=256, leafNodeSize=512`. |
| `inner_miss_` / `leaf_miss_` were never incremented | The counters existed but had no `++` anywhere. The increment was added to `leanstore_cache.h`'s miss path (cold_to_hot_with_admission). Verified working: output shows non-zero counts with `rdma_read/op ≈ 2.13` for uniform/128MB. |
| RDMA stats missing from `newbench_latency` | Added in this session. Now prints same `Avg. rdma read / op` block as `newbench`. |
| `inner_miss` units | Cumulative since `clear_statistic()` (post-warmup). The `[DEX]` line is printed every 2s so the delta per interval ≈ `current_value - previous_value`. |
| `pageSize` is the RDMA transfer unit | Even if `innerNodeSize < leafNodeSize`, every RDMA read transfers `pageSize` bytes. Inner nodes waste `pageSize - innerNodeSize` bytes per transfer. Acceptable for experiment purposes. |
