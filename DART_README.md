# DART: Lock-free Two-layer Hashed ART for Disaggregated Memory

## How DART Works

DART is an Adaptive Radix Tree (ART) optimized for **disaggregated memory**, where the index lives in remote memory nodes and every tree traversal step requires an expensive RDMA network round-trip (~2 µs each).

### The Problem: ART Over RDMA Is Slow

In a standard ART, searching a key like `"ANIMAL"` in this tree:

```
Root
 ├ A
 │  └ N
 │      ├ D
 │      ├ T
 │      └ IMAL → Leaf("Lion")
 └ C
    └ U
       ├ P
       └ T
```

requires traversing **Root → A → AN → ANIM → Leaf** = **5 RDMA reads ≈ 10 µs**.

DART applies three optimizations to reduce this cost:

---

### Optimization 1: Express Skip Table

A hash table that maps key **prefixes** to internal ART node addresses, allowing queries to skip upper tree levels.

```
Skip Table:
  hash("ANIM") → node_ANIM
  hash("CU")   → node_CU
```

Searching `"ANIMAL"`: compute prefix `"ANIM"` → lookup skip table → jump directly to `node_ANIM` → read leaf.

**Result: 2 RDMA reads instead of 5.**

---

### Optimization 2: Adaptive Hashed Node Layout

Traditional ART reads the **entire node** (up to 1 KB) to find a child pointer. DART splits nodes into **hash buckets** (128 bytes each):

```
Node_ANIM:
  Bucket 0: [child 'L' → leaf_ANIMAL]
  Bucket 1: [empty]
  Bucket 2: [child 'S' → ...]
  Bucket 3: [empty]
```

To find child `'L'`: compute `hash('L')` → RDMA read only **bucket 0 (128 bytes)** instead of the full 1 KB node.

**Result: 8× less bandwidth per node access.**

---

### Optimization 3: Decoupled Metadata in Pointers

Each child pointer is packed into **8 bytes** containing:

```
| partial_key | prefix_len | node_type | address |
```

This means reading a pointer immediately reveals the child's type, prefix length, and address — **no extra metadata fetch needed**.

This also enables **lock-free updates** via RDMA Compare-and-Swap (CAS):
- To insert a new node: `CAS(parent_pointer, old_value, new_value)`
- If another thread modified the pointer: CAS fails → retry
- No distributed locks required

---

### End-to-End Search: `"ANIMAL"` → `"Lion"`

| Step | Operation | Bytes Read |
|------|-----------|------------|
| 1 | Skip table lookup: `hash("ANIM")` → `node_ANIM` | Local (~1 MB cached directory) |
| 2 | RDMA read `node_ANIM` hashed bucket for child `'L'` | 128 bytes |
| 3 | RDMA read leaf node | 1095 bytes |

**Total: ~3 RDMA round-trips vs 5 without DART.**

---

## What Is Stored Where

### Memory Node (Remote — 10.30.1.9)

All persistent index data lives here, accessed via one-sided RDMA:

| Data | Description |
|------|-------------|
| ART inner nodes | Node4, Node16, Node48, Node256 — the tree structure |
| ART leaf nodes | Key-value pairs (1095 bytes each: key + value + metadata) |
| Express Skip Table segments | Hash buckets containing shortcut entries (prefix → ART node address) |
| RACE directory (primary copy) | Top-level hash table pointing to skip table segments |

### Compute Node (Local — 10.30.1.6)

Minimal local state — almost everything is fetched from remote memory on each query:

| Data | Size | Persistent? | Description |
|------|------|-------------|-------------|
| RACE directory (local copy) | ~1 MB | Yes — synced once at startup via `sync_dir()`, reused every query | The only true "cache" — maps hash buckets to skip table segment addresses in remote memory |
| Per-thread ART RDMA buffer | 10 MB × 30 = 300 MB | No — overwritten each RDMA read | Scratch space for reading remote ART nodes; data is discarded after each operation |
| Per-thread RACE RDMA buffer | 32 MB × 30 = 960 MB | No — overwritten each operation | Scratch space for skip table RDMA operations |
| YCSB workload data | ~2.1 GB (load) + ~105 MB (run) | Yes — loaded from disk at startup | The key-value pairs and query keys to execute |

**Key distinction:** The skip table *entries* (the actual shortcuts) live in **remote memory**. The compute node only caches the *directory* (~1 MB) that tells it where to find those entries. Every skip table lookup still requires an RDMA read to fetch the actual shortcut from remote memory.

---

## Benchmark Results Analysis

### Experiment Configuration

| Parameter | Value |
|-----------|-------|
| Dataset | 2M key-value pairs (int64 keys) |
| Workload | 2M read-only queries, Zipfian θ=0.5 |
| Threads | 30 |
| Nodes | 1 memory node (10.30.1.9) + 1 compute node (10.30.1.6) |
| RDMA | InfiniBand, NIC index 0, gid_idx=-1 (LID-based) |
| Local cache | ~1 MB (skip table hash directory only) |

### Execution Phases

#### Phase 1: Load (Insert 2M keys into remote ART)

All 2M keys are inserted into the remote ART tree via RDMA writes and CAS operations.

**RDMA operation breakdown during load:**

| Operation | Size (bytes) | Count | What It Does |
|-----------|-------------|-------|--------------|
| READ 256 | 256 | 184,469 | Reading ART inner nodes (Node256 type) |
| READ 136 | 136 | 7,165 | Reading smaller node types (Node4/leaf headers) |
| READ 64 | 64 | 9,323 | Reading compact nodes (Node4) |
| READ 8 | 8 | 3,785 | Reading single pointers/values |
| READ 128 | 128 | 11,091 | Reading Node16 type |
| READ 512 | 512 | 438 | Reading Node48 type |
| READ 1024 | 1024 | 6 | Reading Node256 (full) |
| WRITE 1095 | 1095 | 66,707 | Writing leaf nodes (key + value + metadata) |
| WRITE 64–2048 | various | 11,949 | Writing new inner nodes of various ART types |
| CAS 8 | 8 | 70,447 | Lock-free pointer installs (atomic pointer swings) |
| CAS fail 8 | 8 | 44 | CAS retries due to contention (only 0.06% failure rate) |

**Key observation:** Only 44 CAS failures out of 70,447 attempts — very low contention with 30 threads, validating DART's lock-free design.

#### Phase 2: Prepare (Build Skip Table)

After loading, DART traverses the ART tree and creates shortcut entries:

| Metric | Value |
|--------|-------|
| Total shortcuts | 251,249 |
| Level 1 shortcuts | 128 (top-level, broad prefixes) |
| Level 3 shortcuts | 251,121 (where most keys diverge) |
| All other levels | 0 |

**Interpretation:** With 2M int64 keys, the ART tree is shallow. Most keys share prefixes up to level 3, so the skip table sends queries directly to level-3 nodes, skipping the root and first two levels.

#### Phase 3: Run (2M Read-Only Queries, Zipfian θ=0.5)

**RDMA operations during run phase:**

| Operation | Size (bytes) | Count | What It Does |
|-----------|-------------|-------|--------------|
| READ 256 | 256 | 149,307 | Reading inner nodes during traversal |
| READ 136 | 136 | 66,659 | Reading node headers for verification |
| READ 1095 | 1095 | 66,659 | Reading full leaf nodes (returning values) |
| READ 64 | 64 | 16,902 | Reading compact nodes |
| WRITE | — | 0 | None (read-only workload) |
| CAS | — | 0 | None (read-only workload) |

**Key observation:** Zero writes and zero CAS operations confirms this is purely read-only. The 66,659 leaf reads of 1095 bytes each correspond to successful key lookups.

### Per-Thread Results

| Metric | Threads 1–10 | Threads 11–20 | Threads 21–30 |
|--------|-------------|--------------|--------------|
| Throughput (MOps) | 0.053–0.055 | 0.061–0.062 | 0.053–0.054 |
| Latency (µs) | 18.3–18.8 | 16.2–16.3 | 18.5–18.8 |

**NUMA effect:** Threads 11–20 show ~15% better performance (16.2 µs vs 18.7 µs). This is because these threads are pinned to CPU cores that have closer affinity to the RDMA NIC, resulting in lower PCIe/memory access latency for posting RDMA operations.

### Aggregate Results

| Metric | Value |
|--------|-------|
| **Total operations** | 2,000,000 |
| **Aggregate throughput** | **1.678 MOps** |
| **Average latency** | **17.878 µs** |
| **Avg RDMA round-trips/query** | **4.49** |
| **Avg bandwidth/query** | **1820 bytes** |

### What the Numbers Mean

- **4.49 avg RTTs/query:** Each lookup requires ~4.5 RDMA round-trips on average. The skip table saves 1–2 levels (would be ~6 without it), but the remaining traversal from level 3 → leaf still costs multiple hops.

- **1820 bytes avg bandwidth/query:** Thanks to the hashed node layout, DART reads selective buckets (~128–256 bytes) instead of full nodes. But with ~4.5 reads per query, total bandwidth adds up.

- **1.678 MOps throughput:** With 30 threads each doing independent RDMA reads, the aggregate throughput is limited by the RDMA RTT latency (~4 µs per hop × 4.5 hops ≈ 18 µs per operation → ~55K ops/thread → 1.67M ops total).

- **Zipfian 0.5 has minimal impact:** θ=0.5 is only mildly skewed. Since DART has no persistent local cache (only the ~1 MB directory), hot keys still require the same number of RDMA reads as cold keys. This is different from DEX, where a large local cache would benefit from skew.

### Comparison: Zipfian 0.5 vs 0.99

| Metric | Zipfian θ=0.5 | Zipfian θ=0.99 |
|--------|--------------|----------------|
| Throughput | 1.678 MOps | 1.674 MOps |
| Latency | 17.878 µs | 17.916 µs |
| Avg RTTs | 4.49 | 4.49 |

**Nearly identical performance.** This confirms that DART's performance is **independent of key access skew** — every query still does the same number of RDMA reads regardless of whether it's a hot or cold key. Without a persistent local data cache, skew provides no benefit.
