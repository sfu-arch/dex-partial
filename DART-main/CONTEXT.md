# DART — Project Context

**Paper:** DART: A Lock-free Two-layer Hashed ART Index for Disaggregated Memory (SIGMOD '26)

---

## 1. What DART is

DART is a **B-tree / radix-tree index** designed to run on **disaggregated memory** — a data-centre architecture where compute nodes (CPUs with small DRAM) are separated from memory nodes (large DRAM pools) connected over a high-speed RDMA fabric (InfiniBand).

The key design goals:
- **Lock-free** — concurrent insert/update/delete/search without coarse-grained locks
- **Two-layer** — an Adaptive Radix Tree (ART) as the primary index plus a RACE hash table as a fast skip layer
- **RDMA-native** — tree nodes live on remote memory nodes; compute nodes issue RDMA reads/writes/CAS over InfiniBand

---

## 2. System architecture

```
┌──────────────────────────────────────┐      RDMA / InfiniBand
│  Compute Node (10.30.1.6)            │◄────────────────────────►┐
│                                      │                           │
│  bin/compute                         │                           │
│  ├── N worker threads                │                           │
│  │   └── PrheartTree (ART client)    │                           │
│  │       └── DisaggregatedMemory-    │                           │
│  │           Controller (DMC)        │                           │
│  └── RACE::Client (skip-table cache) │                           │
│                                      │  ┌────────────────────────┴──┐
└──────────────────────────────────────┘  │  Memory Node (10.30.1.9)  │
                                          │                            │
                TCP (socket)              │  bin/memory               │
┌─────────────────────────────────────┐   │  └── remote DRAM pool     │
│  Monitor (10.30.1.9)                │   │      (MR exposed via IB)  │
│                                     │   └───────────────────────────┘
│  bin/monitor                        │
│  └── orchestrates experiment:       │
│      • sends config to compute      │
│      • coordinates load/run phases  │
│      • collects throughput results  │
└─────────────────────────────────────┘
```

### Three-process model

| Binary | Typical node | Role |
|--------|-------------|------|
| `bin/monitor` | 10.30.1.9 | Experiment controller. Accepts TCP connections from memory and compute nodes; sends configuration; signals phase transitions (load→prepare→run); collects and prints throughput/latency. |
| `bin/memory` | 10.30.1.9 | Memory server. Registers a large DRAM region as an RDMA Memory Region (MR); serves all RDMA reads, writes, and atomic CAS operations issued by compute nodes. Has no knowledge of the ART structure. |
| `bin/compute` | 10.30.1.6 | Workload executor. Spawns N worker threads; each thread builds a local `PrheartTree` client over its RDMA connection and executes inserts, lookups, updates, deletes, and scans. |

**Startup order:** monitor → memory → compute. The monitor must be listening before the other two connect. Each compute thread's RDMA QP is connected to each memory node's MR via the monitor acting as a rendezvous point.

---

## 3. The ART index — `prheart`

### 3a. Node types

DART uses an **Adaptive Radix Tree** (ART). Keys are variable-length byte strings; each level of the trie consumes one byte of the key.

```
PrheartNodeType:
  Leaf    — stores key + value pointer
  Node8   — up to   8 children
  Node16  — up to  16 children
  Node32  — up to  32 children
  Node64  — up to  64 children
  Node128 — up to 128 children
  Node256 — up to 256 children (root)
```

Nodes **grow** as children are added (Node8 → Node16 → … → Node256). The root is always a Node256.

### 3b. Slot encoding

Every child pointer in an ART node is a 64-bit **slot** with a packed bit layout:

```
 63        61 60     56 55    48 47              6 5       0
 ┌──────────┬─────────┬────────┬─────────────────┬─────────┐
 │ node_type│ version │key_byte│      fptr (42b) │  length │
 │  (3 bit) │  (5 bit)│ (8 bit)│  (remote addr)  │  (6 bit)│
 └──────────┴─────────┴────────┴─────────────────┴─────────┘
```

- **fptr** — 42-bit remote far-pointer (address in the memory node's MR, 64-byte aligned)
- **version** — optimistic concurrency control; incremented on every write
- **key_byte** — the radix byte this edge represents
- **node_type / length** — type and compressed-prefix length of the child

### 3c. Key encoding for the microbenchmark

`uint64_t` keys are stored as **8-byte big-endian** spans so numerical ordering is preserved in the trie (byte `[0]` = MSB). Helper in `compute.cc`:

```cpp
inline span mb_make_span_be(uint64_t k, uint8_t buf[8]) {
    const uint8_t *p = reinterpret_cast<const uint8_t *>(&k);
    for (int i = 0; i < 8; i++) buf[i] = p[7 - i];
    return span(buf, 8);   // span<uint8_t>
}
```

Keys from the distribution are scrambled with `FNVHash64(raw) % key_space` before encoding to avoid sequential-access hot-spots in the trie.

### 3d. Operations

| Method | Signature | Notes |
|--------|-----------|-------|
| `search` | `bool search(span key)` | Traverses from root to leaf over RDMA; optimistic read |
| `insert` | `bool insert(span key, span value)` | Lock-free CAS on the target slot |
| `update` | `bool update(span key, span value)` | Overwrites leaf value |
| `remove` | `bool remove(span key)` | Marks slot as empty |
| `scan`   | `bool scan(span start, span end, vec<str>& out)` | In-order traversal between keys |

All operations issue **RDMA reads** to traverse the trie and **RDMA CAS / writes** only at the point of mutation. The tree never needs to contact the monitor.

### 3e. Path compression

DART compresses runs of single-child nodes into **prefix strings** stored inline in the slot's `length` field. The scan implementation tracks `scan_choice` (optimistic / lower_bound / upper_bound / certain) to handle compressed nodes during range traversal.

---

## 4. The RACE skip table

RACE is a **two-level extendible hashing** structure stored on the memory node, used as a **shortcut layer** that caches hot ART node pointers. When enabled (`#define SKIP_TABLE`), a lookup can skip several trie levels by consulting the RACE hash table first.

### Structure

```
Directory (2^depth entries)
└── Segment (64 buckets × 3 groups)
    └── Bucket (8 slots)
        └── Slot { fingerprint(8b), pos(5b), type(3b), offset(48b) }
```

- **Directory** — array of `DirEntry` pointers indexed by hash
- **Segment** — 12 KB unit; each segment has 3 groups of 64 buckets for probing
- **KVBlock** — contiguous `[k_len][v_len][key bytes][value bytes]` stored at `offset`

### Client / Server

- `RACE::Client` — runs on compute node; issues RDMA reads to find slots and RDMA CAS to insert/delete
- `RACE::Server` — runs on memory node; allocates the directory and segment pool

The skip table is **built once** after the bulk-load phase (`create_skip_table()`) and used read-only during the benchmark run.

---

## 5. RDMA layer

### Memory layout per thread

Each compute thread has:
1. **Remote allocation region** — a slice of the memory node's MR dedicated to this thread for allocating new ART nodes. Partitioned by `DisaggregatedMemoryController` based on `(compute_index, thread_index)`.
2. **Local MR** — a local DRAM buffer registered with the RDMA NIC (size = `--th_mb` MB). All RDMA operations DMA through this buffer.

### `DisaggregatedMemoryController` (DMC)

Per-thread wrapper over one or more `RDMAConnection` objects (one per memory node). Exposes:

```
get_root_start_fptr()   — remote address of the ART root node
get_alloc_start_fptr()  — start of this thread's remote allocation region
get_alloc_end_fptr()    — end of that region
get_local_start_ptr()   — start of local DRAM buffer (DMA staging area)
get_local_size()        — size of local DRAM buffer
```

### RDMA statistics collected per run

```
Avg. rdma read / op       — remote reads per tree operation
Avg. rdma write / op      — remote writes per tree operation
Avg. rdma cas / op        — CAS operations per tree operation
Avg. rdma rpc / op        — skip-table RPCs per tree operation
Avg. rdma read size / op  — bytes read per operation
```

---

## 6. Monitor protocol (TCP)

The monitor coordinates phases by sending/receiving small `uint32_t` tokens over TCP:

```
compute → monitor : 1      (I am compute node N)
monitor → compute : config (memory_num, thread_counts, th_mb, test_func, workload paths, …)
compute → monitor : 200    (RDMA QPs configured, ready)
monitor → compute : 200    (all nodes ready, start load phase)
compute → monitor : 600    (bulk-load done)
monitor → compute : 700    (all nodes finished load, start prepare)
compute → monitor : 800    (skip table / DYNAMIC analysis done)
monitor → compute : 900    (start benchmark run)
compute → monitor : double throughput (MOps/s)
compute → monitor : double latency (µs)
compute → monitor : double 0
monitor → compute : 999    (done, disconnect)
```

`test_func` selects the workload mode:
- `0` — YCSB file-based (`test_ycsb_run`)
- `1` — microbenchmark in-memory (`test_mb_run`)

---

## 7. Microbenchmark additions

The following was added to DART to replace file-based YCSB workloads with in-memory generated workloads, matching the DEX/CHIME benchmark methodology.

### New headers (`include/microbench/`)

| File | Class | Description |
|------|-------|-------------|
| `uniform_generator.h` | `uniform_key_generator_t` | `std::uniform_int_distribution` over `[1, N]`; seeded with `0xc70f6907` |
| `uniform.h` | `UniformRandom` | Fast LCG RNG (`seed = seed * 0xD04C3175 + 0x53DA9022`); used for op-type selection |
| `zipf.h` | `zipf_gen_state` + `mehcached_zipf_*` | Mehcached Zipfian generator; supports θ ∈ [0, 1) and θ = 0 (uniform), θ ≥ 40 (always 0); approximate pow for speed |

### New globals in `compute.cc`

```cpp
g_mb_key_space         // total key universe size
g_mb_bulk_load_num     // keys to pre-insert
g_mb_thread_op_num     // timed ops per thread
g_mb_thread_warmup_num // warmup ops per thread
g_mb_bulk_array[]      // pre-shuffled bulk-load key IDs
g_mb_warmup_array[]    // packed (op_type:8 | raw_key:56) warmup ops
g_mb_workload_array[]  // packed benchmark ops
```

### Workload generation (`mb_generate_workload`)

```
1. Build space_array[0..key_space-1] = {0, 1, 2, …}
2. Shuffle with mt19937(seed=0xc70f6907 + node_id)
3. Copy first bulk_load_num entries → g_mb_bulk_array
4. Init zipf_gen_state or uniform_key_generator_t
5. For each warmup/benchmark op:
   a. Sample raw_key from distribution [0, key_space)
   b. Draw random [0,100) with UniformRandom for op-type selection
   c. Pack:  entry = raw_key | (op_type << 56)
6. Store in g_mb_warmup_array / g_mb_workload_array
```

Key space is sized to accommodate bulk inserts plus expected new inserts from the workload:
```
key_space = bulk_load_num + (op_num + warmup_num) × insert_ratio/100 + 1000
```

### Key scrambling

```cpp
uint64_t mb_to_key(uint64_t raw) {
    return (RACE::FNVHash64(raw) + 1) % g_mb_key_space;
}
```

Applied at operation time — arrays store raw IDs; the hash is applied just before calling `prheart_tree.insert/search/…`. Both bulk-load and workload go through the same hash, so lookups find their keys.

### Thread functions

| Function | Phase | What it does |
|----------|-------|-------------|
| `test_mb_load` | Bulk-load | Each thread inserts `g_mb_bulk_array[start..end]` — its slice of the array. Converts each raw ID → FNV hash → big-endian 8-byte span → `prheart_tree.insert`. |
| `test_mb_run` | Warmup + benchmark | Iterates `g_mb_warmup_array` (untimed), then `g_mb_workload_array` (timed). Decodes op type from high 8 bits; dispatches to `search / insert / update / remove / scan`. |

### Operation encoding

```
packed_entry = raw_key | (op_type << 56)

op_type values:
  0 = Insert
  1 = Update
  2 = Lookup
  3 = Delete
  4 = Range
```

Range scan uses `scan(be(k), be(k + scan_num), result_vec)` — a window of `scan_num` consecutive hash-space slots starting at the hashed key.

---

## 8. Key data structures — quick reference

| Structure | Location | Description |
|-----------|----------|-------------|
| `PrheartTree` | `include/prheart/art-node.hpp` | ART index client; holds DMC pointer + root fptr + alloc region |
| `PrheartNode` | `include/prheart/art-node.hpp` | In-memory representation of a fetched ART node |
| `PrheartSlotData` | `include/prheart/art-data.hpp` | 64-bit packed slot (type / version / key_byte / fptr / length) |
| `DisaggregatedMemoryController` | `include/rdma/rdma-connection.hpp` | Per-thread abstraction over RDMA connections; partitions remote address space |
| `RACE::Client` | `include/race/race.h` | Skip-table client; RDMA-based extendible hash lookups |
| `RACE::Server` | `include/race/race.h` | Skip-table server; manages directory + segments on memory node |
| `Slice` | `include/race/race.h` | `{uint64_t len; char* data}` — key/value descriptor |
| `KVBlock` | `include/race/race.h` | Contiguous `[k_len][v_len][key][value]` stored in remote MR |
| `span` | `include/measure/shortname.hpp` | `std::span<uint8_t>` — used for all tree key/value arguments |
| `zipf_gen_state` | `include/microbench/zipf.h` | Mehcached Zipfian state (θ, α, zetan, eta, rand_state) |
| `UniformRandom` | `include/microbench/uniform.h` | LCG RNG for op-type selection |
| `uniform_key_generator_t` | `include/microbench/uniform_generator.h` | `std::uniform_int_distribution` key generator |

---

## 9. Source file layout

```
DART-main/
├── include/
│   ├── prheart/          ART index headers
│   │   ├── art-head.hpp    node type enum
│   │   ├── art-data.hpp    slot bit-packing, PrheartSlotData
│   │   ├── art-node.hpp    PrheartNode, PrheartTree declaration
│   │   └── prheart.hpp     aggregator
│   ├── race/             RACE skip-table headers
│   │   ├── race.h          Client, Server, Slot, Bucket, Segment, KVBlock
│   │   ├── generator.h     xoshiro256++, zipf99, uniform, FNVHash64
│   │   ├── config.h        RACE::Config (gflag parser)
│   │   ├── alloc.h         local + remote allocators
│   │   ├── aiordma.h       async RDMA coroutine primitives
│   │   └── perf.h          latency/cost accumulators
│   ├── rdma/             RDMA layer headers
│   │   ├── rdma-basic.hpp    RdmaStatistics, connect structs
│   │   ├── rdma-connection.hpp  RDMAConnection, DMC, RDMAConnectionMetadata
│   │   └── rdma.hpp          aggregator
│   ├── microbench/       NEW — in-memory workload generators
│   │   ├── uniform_generator.h  uniform_key_generator_t
│   │   ├── uniform.h            UniformRandom (LCG)
│   │   └── zipf.h               zipf_gen_state, mehcached_zipf_*
│   ├── ycsb/             YCSB file-based workload (original)
│   │   ├── ycsb-head.hpp        operat enum
│   │   ├── ycsb-fileloader.hpp  FileLoader
│   │   └── ycsb-timecounter.hpp timer
│   └── measure/          type aliases, key utilities, timers
│       ├── shortname.hpp   u8/u64/str/span/vec/tup
│       ├── keytype.hpp     span↔u64 conversions, BufferBlock
│       └── timecounter.hpp TimeCounter (throughput / latency)
│
├── src/
│   ├── main/
│   │   ├── monitor.cc    experiment orchestrator
│   │   ├── memory.cc     RDMA memory server
│   │   └── compute.cc    workload executor  ← primary file; modified for microbench
│   ├── prheart/          ART implementation
│   ├── race/             RACE implementation
│   ├── rdma/             RDMA implementation
│   └── ycsb/             YCSB file loader implementation
│
├── benchmark_run/
│   ├── run_dart_memnode_sweep.sh  NEW — microbench sweep, memory/monitor side
│   ├── run_dart_compute_sweep.sh  NEW — microbench sweep, compute side
│   ├── run_memory_node.sh         YCSB sweep, memory/monitor combined
│   ├── run_compute_auto.sh        YCSB sweep, compute auto-retry
│   ├── run_monitor.sh             YCSB monitor (3-terminal mode)
│   ├── run_memory.sh              YCSB memory (3-terminal mode)
│   └── workload_specs/            YCSB spec files (uniform/zipf configs)
│
├── script/               workload generation utilities
├── CMakeLists.txt        builds monitor, memory, compute
├── COMMANDS.md           full command reference
└── CONTEXT.md            this file
```

---

## 10. Benchmark execution flow (microbench, `test_func=1`)

```
monitor starts, opens port 9898
        │
memory connects → registered
        │
compute connects → receives:
    memory_num, compute_num, thread_counts, coro_num,
    th_mb, bucket, test_func=1, workload strings (ignored)
        │
compute: RDMA QPs set up, connect data exchanged via monitor TCP
        │
monitor broadcasts go=200 ───────────────────────► all compute nodes start
        │
compute (main thread):
    mb_generate_workload(run_thread_num)
        • shuffles key space
        • initialises zipf / uniform generator
        • builds bulk_array, warmup_array, workload_array
        │
compute spawns N × test_mb_load threads
    each thread:
        • creates DMC + PrheartTree
        • inserts bulk_array[start..end] via RDMA
        │
compute → monitor: 600 (load done)
monitor → compute: 700 (all nodes loaded)
        │
[if SKIP_TABLE] compute node 0 builds RACE skip table
        │
compute → monitor: 800 (prepare done)
monitor → compute: 900 (start run)
        │
compute spawns N × test_mb_run threads
    each thread:
        • warmup phase (untimed): iterates warmup_array slice
        • benchmark phase (timed): iterates workload_array slice
            for each entry:
                decode op_type = entry >> 56
                decode raw_key = entry & 0x00FFFFFFFFFFFFFF
                k = FNVHash64(raw_key) % key_space
                key_span = big_endian_8bytes(k)
                dispatch to prheart_tree.search/insert/update/remove/scan
        • records event_count + time → TimeCounter
        │
main thread joins all, aggregates TimeCounter
    → prints throughput (MOps/s) + latency (µs)
        │
compute → monitor: throughput, latency, 0.0
monitor → compute: 999 (end)
```

---

## 11. Compile-time feature flags (in `compute.cc`)

| Define | Effect |
|--------|--------|
| `#define SKIP_TABLE` | Enable RACE hash skip table; `create_skip_table()` runs after bulk-load |
| `#define DYNAMIC` | Enable `dfs()` for dynamic cost analysis after load |
| `#define PRINT_TREE` | Print full tree structure after load (slow, debug only) |
| `#define PRINT_OUTPUT` | Print per-op failures during benchmark |
| `#define GLOBAL_WORKLOAD` | (microbench source-code reference) global vs partitioned key space |

---

## 12. Performance tuning guide

| Goal | Parameter | Location |
|------|-----------|----------|
| More threads | `--load_thread_num` / `--run_thread_num` | monitor |
| Larger remote memory | `--mem_mb` | monitor |
| More local buffering per thread | `--th_mb` | monitor |
| Larger skip-table | `--bucket` | monitor |
| Bigger dataset | `--mb_bulk_load_num` | compute |
| Longer run | `--mb_op_num` | compute |
| Skew | `--mb_zipfian` (0.0–0.99) | compute |
| Uniform distribution | `--mb_uniform=true` | compute |
| Mixed workload | adjust all five `--mb_*_ratio` flags (must sum to 100) | compute |
| Range scan width | `--mb_scan_num` | compute |
