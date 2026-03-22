# DEX vs DART: Cache Crossover Analysis

## When DEX is Worse Than DART — and How to Observe It

---

## 1. The Setup We're Working With

```
┌─────────────────────┐        InfiniBand RDMA       ┌─────────────────────┐
│   Compute Node      │ ◄──────────────────────────► │   Memory Node       │
│   10.30.1.6         │                              │   10.30.1.9         │
│   30 threads        │                              │   8 GB DSM          │
│   DEX client        │                              │   B+-tree pages     │
│   DART compute      │                              │   DART ART nodes    │
└─────────────────────┘                              └─────────────────────┘
```

**Fixed for all experiments below:**
- 1 compute node, 1 memory node
- 30 client threads
- 50M keys bulk-loaded
- Uniform workload (no skew) to stress-test the cache

---

## 2. Where DEX Beats DART (The Ideal Case)

With a large enough cache and skewed access, DEX caches hot leaf pages locally and avoids RDMA entirely for repeated reads.

```
         Throughput (Mops/s)
    12 ┤
       │                              ████  DEX (θ=0.99, 256MB cache)
    10 ┤                              ████
       │
     8 ┤              ████████████████████  DEX (uniform, 256MB cache)
       │
     6 ┤
       │
     4 ┤  ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░  CHIME
       │
     2 ┤  ▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒▒  DART (flat, always ~1.67)
       │
     0 └──────────────────────────────────────────────
         32MB     64MB    128MB   256MB   512MB  Cache
```

DEX here: cache hit on internal nodes AND hot leaf pages → 0 RDMA → blazing fast.

---

## 3. Where DEX Falls Below DART

### Scenario A: Cache too small to hold the hot path

DEX's IndexCache holds **both internal nodes and leaf pages in one pool**.

```
                       DEX IndexCache (e.g., 32 MB)
┌──────────────────────────────────────────────────────────────┐
│  Internal nodes (levels 0-3): tiny, easily fit               │
│  [root][L1×32][L2×1024] ← 1+32+1024 × 1KB = ~1 MB          │
│                                                              │
│  Level-4 nodes: 1,048,576 × 1KB = 1 GB  ← CANNOT FIT       │
│  Leaves:        ~3M × 1KB              ← CANNOT FIT         │
│                                                              │
│  So: only levels 0-3 cached, level 4 + leaf = 2 RDMA misses │
└──────────────────────────────────────────────────────────────┘

DEX total RTTs per lookup = 2 (miss on L4) + 1 (miss on leaf) = 3 RTTs
DART total RTTs per lookup = 1 (bucket) + 1 (leaf) = 2 RTTs
→ DART wins
```

### Scenario B: Fat leaf pages dilute the cache

`kLeafPageSize` is a **compile-time constant** in [dex/include/Common.h:159](dex/include/Common.h).

Currently:
```cpp
// dex/include/Common.h line 158-159
constexpr uint32_t kInternalPageSize = 1024;  // 1 KB — compile-time only
constexpr uint32_t kLeafPageSize     = 1024;  // 1 KB — compile-time only
```

If you recompile with `kLeafPageSize = 4096` (4 KB), here is what happens to a 256 MB cache:

```
Default (1KB leaves):           Modified (4KB leaves):
Cache = 256 MB                  Cache = 256 MB
Leaf slots = 256K pages         Leaf slots = 64K pages
Dataset leaves = ~3M pages      Dataset leaves = ~800K pages
Hit rate (uniform) = 8.5%       Hit rate (uniform) = 8.0% ← similar

BUT each cache miss now fetches 4KB instead of 1KB:
→ 4× RDMA bandwidth per miss
→ Cache fills faster, evicts more aggressively under uniform load
→ Internal node eviction increases → more L4 misses
→ DEX total RTTs = 3-4 per lookup
DART stays at 2 RTTs
→ DART wins
```

### Scenario C: Small inner nodes → taller tree

`kInternalPageSize` controls fanout. Smaller pages = fewer keys per node = lower fanout = **taller tree**.

```
kInternalPageSize = 1024B  →  fanout ≈ 60  →  height for 50M keys = ceil(log_60(50M)) ≈ 4
kInternalPageSize = 128B   →  fanout ≈ 7   →  height for 50M keys = ceil(log_7(50M))  ≈ 7

Height 4, small cache (32MB):
  Cache holds levels 0-2 (1+60+3600 = ~4MB for 1KB nodes)
  Misses on level 3 + leaf = 2 RDMA RTTs
  DEX total = 2 RTTs → ties DART

Height 7, small cache (32MB):
  Cache holds levels 0-4 at most
  Misses on levels 5-6 + leaf = 3 RDMA RTTs
  DEX total = 3 RTTs > DART's 2 RTTs
  → DART WINS
```

### The Full Crossover Map

```
                         Leaf Page Size
                  1 KB          2 KB          4 KB
               ┌─────────────┬─────────────┬─────────────┐
  Cache 512MB  │  DEX wins   │  DEX wins   │  DEX wins   │
               ├─────────────┼─────────────┼─────────────┤
  Cache 256MB  │  DEX wins   │  DEX wins   │  DEX ~ DART │
               ├─────────────┼─────────────┼─────────────┤
  Cache 128MB  │  DEX wins   │  DEX ~ DART │  DART wins  │
               ├─────────────┼─────────────┼─────────────┤
  Cache  64MB  │  DEX ~ DART │  DART wins  │  DART wins  │
               ├─────────────┼─────────────┼─────────────┤
  Cache  32MB  │  DART wins  │  DART wins  │  DART wins  │
               └─────────────┴─────────────┴─────────────┘
                        (uniform workload assumed)
                        Under Zipf skew: shift left one column — DEX gains back
```

---

## 4. The RTT Arithmetic

```
DEX per lookup:
  Level 0 (root)    → always cached (1 node)            = 0 RDMA
  Level 1-2         → almost always cached (tiny)        = 0 RDMA
  Level 3           → depends on cache size             = 0 or 1 RDMA
  Level 4 (bottom)  → misses if 1M nodes > cache        = 0 or 1 RDMA
  Leaf              → misses under uniform load          = 0 or 1 RDMA
                                                    ─────────────────
  Best case (large cache + skew):                    1 RTT  (~0.5µs)
  Typical (medium cache + uniform):                  2 RTTs (~3µs)
  Worst case (small cache + uniform):                3-4 RTTs (~8µs)

DART per lookup:
  Directory lookup  → local (~1MB always cached)         = 0 RDMA
  Bucket fetch      → always 1 RDMA READ (128 bytes)     = 1 RDMA
  Leaf fetch        → always 1 RDMA READ (~1KB)          = 1 RDMA
                                                    ─────────────────
  Always:                                            2 RTTs (~18µs)

Note: DART's 2 RTTs cost ~18µs while DEX's 2 RTTs cost ~3µs.
This is because DART uses async coroutine-based RDMA (1 coro/thread)
while DEX uses synchronous RDMA with coroutine pipelining.
Crossover on THROUGHPUT happens before crossover on LATENCY.
```

---

## 5. DEX — Exact Variables and How to Run

### Compile-Time Constants (must recompile to change)

File: [dex/include/Common.h](dex/include/Common.h)

| Line | Constant | Default | What it controls |
|------|----------|---------|-----------------|
| 143 | `kIndexCacheSize` | `512` MB | Default cache (overridden at runtime by argv[9]) |
| 158 | `kInternalPageSize` | `1024` B | Internal node size → controls fanout |
| 159 | `kLeafPageSize` | `1024` B | Leaf page size → controls cache pressure |
| 50 | `MAX_APP_THREAD` | `36` | Max threads per compute node |
| 99 | `dsmSize` | `8` GB | Total remote memory pool |
| 103 | `rdmaBufferSize` | `1` GB | Local RDMA buffer |
| 135 | `kMaxLevelOfTree` | `7` | Max tree height (assertion guard) |

**To change leaf size** (triggers the crossover):
```cpp
// dex/include/Common.h
constexpr uint32_t kLeafPageSize = 4096;  // change from 1024 to 4096
```
Then rebuild: `cd dex/build && make -j$(nproc)`

### Runtime Arguments (newbench_latency)

File: [dex/test/newbench_latency.cpp:290](dex/test/newbench_latency.cpp)

```
./newbench_latency  <arg1> <arg2> ... <arg22>

Pos  Name              Your setup    What it does
───  ────────────────  ───────────   ─────────────────────────────────────────
 1   kNodeCount        2             Total nodes (1 memory + 1 compute = 2)
 2   kReadRatio        100           % reads  (must sum to 100 with below)
 3   kInsertRatio      0             % inserts
 4   kUpdateRatio      0             % updates
 5   kDeleteRatio      0             % deletes
 6   kRangeRatio       0             % range scans
 7   totalThreadCount  30            Client threads on this compute node
 8   memThreadCount    4             Threads on memory node
 9   cache_mb          [32-512]   ★  Cache size in MB  ← KEY KNOB
10   uniform_workload  1             1=uniform, 0=use zipfian theta
11   zipfian_theta     0.99          Zipf θ (only used if arg10=0)
12   bulk_load_num     50            Millions of keys to preload
13   warmup_num        10            Millions of warmup ops (not measured)
14   op_num            50            Millions of measured ops
15   check_correctness 0             0=no verification, 1=verify
16   time_based        1             1=ops-based timing, 0=time-based
17   early_stop        0             Forced to 0 in code (latency mode)
18   tree_index        0             0=DEX, 1=Sherman, 2=SMART
19   rpc_rate          1             RPC delegation ratio (1=max)
20   admission_rate    0.1           Cache admission rate (0.1 = admit 10%)
21   auto_tune         0             0=fixed params
22   kMaxThread        30            Max threads per node
```

### Commands to Run the Cache Crossover Experiment

Run these on the **compute node (10.30.1.6)** after restarting memcached.

```bash
# Step 1: Restart memcached on memory node FIRST (10.30.1.9)
# ssh 10.30.1.9 "pkill memcached; sleep 1; memcached -u nobody -m 64 -p 11211 &"

# Step 2: On compute node — sweep cache sizes (uniform workload)
cd dex/build

# 32 MB — expect DEX ≈ or worse than DART
sudo ./newbench_latency 2 100 0 0 0 0  30 4  32  1 0.99  50 10 50  0 1 0  0 1 0.1 0 30

# 64 MB — crossover zone
sudo ./newbench_latency 2 100 0 0 0 0  30 4  64  1 0.99  50 10 50  0 1 0  0 1 0.1 0 30

# 128 MB — DEX starts to pull ahead
sudo ./newbench_latency 2 100 0 0 0 0  30 4  128 1 0.99  50 10 50  0 1 0  0 1 0.1 0 30

# 256 MB — DEX clearly better
sudo ./newbench_latency 2 100 0 0 0 0  30 4  256 1 0.99  50 10 50  0 1 0  0 1 0.1 0 30

# 512 MB — DEX dominant
sudo ./newbench_latency 2 100 0 0 0 0  30 4  512 1 0.99  50 10 50  0 1 0  0 1 0.1 0 30
```

**What to observe in output:**
```
Final cluster throughput: X.XXX Mops/s     ← compare against DART's ~1.67 Mops/s

========== DEX Read LATENCY STATISTICS ==========
P50 latency: XXXX ns                        ← drops as cache size increases
P99 latency: XXXX ns                        ← spikes under cache thrash
```

Output files written by the benchmark:
```
dex_read_latency.dat    # latency histogram for point reads
dex_range_latency.dat   # latency histogram for range scans
```

### The Existing run.sh (Reference)

File: [dex/script/run.sh](dex/script/run.sh)

The script at line 45 runs:
```bash
sudo ./newbench $nodenum \
  ${read[$op]} ${insert[$op]} ${update[$op]} ${delete[$op]} ${range[$op]} \
  ${threads[$t]} ${mem_threads[1]} \
  ${cache[3]} \      # ← cache[3] = 256 MB  (array: 0 64 128 256 512 1024)
  $uni ${zipf[0]} \  # ← uni=0 (zipf), zipf=0.99
  $bulk $warmup $runnum \  # ← 50M load, 10M warmup, 50M run
  $correct $timebase $early $idx $rpc $admit $tune 36
```

To sweep caches, change `${cache[3]}` to `${cache[1]}` through `${cache[5]}`.

---

## 6. DART — Exact Variables and How to Run

### DART's Fixed Parameters (Compile-Time, race.h)

File: [DART-main/include/race/race.h](DART-main/include/race/race.h)

| Line | Constant | Value | What it controls |
|------|----------|-------|-----------------|
| 22 | `SLOT_PER_BUCKET` | `8` | Slots per 128-byte bucket |
| 23 | `BUCKET_BITS` | `6` | 64 buckets per segment |
| 24 | `BUCKET_PER_SEGMENT` | `64` | Buckets per 12KB segment |
| 25 | `INIT_DEPTH` | `15` | Directory depth → 2^15 = 32K entries |
| 26 | `MAX_DEPTH` | `20` | Max possible depth (never reached — splits disabled) |
| 27 | `DIR_SIZE` | `2^20` | Pre-allocated directory space (~1 MB local) |

**These never change at runtime.** This is why DART's performance is flat.

### IPs Hardcoded in Source

File: [DART-main/src/main/compute.cc:30](DART-main/src/main/compute.cc)

```cpp
const char* ips[] = {"10.30.1.9","10.30.1.9","10.30.1.6"};
//                    memory node  memory node  compute node
```

### DART Runtime Parameters

DART uses **gflags** (not positional args). Flags defined in compute.cc:

```
--monitor_addr     "10.30.1.9:9898"   Monitor's IP:port
--nic_index        0                  RDMA NIC to use
--ib_port          1                  InfiniBand port
--numa_node_total_num  2              Total NUMA groups (for CPU pinning)
--numa_node_group  0                  Which NUMA group this process uses
```

Monitor flags (passed to `bin/monitor`):

| Flag | Your setup | What it controls |
|------|-----------|-----------------|
| `--test_func` | `0` | 0=YCSB load+run, other=diagnostic |
| `--memory_num` | `1` | Number of memory nodes |
| `--compute_num` | `1` | Number of compute nodes |
| `--load_thread_num` | `30` | Threads for bulk load phase |
| `--run_thread_num` | `30` | ★ Threads for benchmark phase |
| `--coro_num` | `1` | Coroutines per thread (1 = synchronous) |
| `--mem_mb` | `4096` | Remote memory in MB (4 GB) |
| `--th_mb` | `10` | Per-thread RDMA buffer in MB |
| `--workload_load` | `2m_load` | YCSB load file (inserts) |
| `--workload_run` | `uniform_run` | YCSB run file (reads/scans) |
| `--bucket` | `256` | RACE bucket count hint |
| `--run_max_request` | `2000000` | Total ops to run |

### Commands to Run DART (1 compute + 1 memory node, 30 threads)

**Step 0: Generate workloads** (run on BOTH nodes once)
```bash
cd DART-main
bash benchmark_run/gen_workloads.sh
```

**Step 1: On memory node (10.30.1.9) — Terminal 1: start memory**
```bash
cd DART-main
bin/memory --monitor_addr=10.30.1.9:9898 --nic_index=0 &
```

**Step 2: On memory node (10.30.1.9) — Terminal 2: start monitor**
```bash
cd DART-main
bin/monitor \
    --test_func=0 \
    --memory_num=1 \
    --compute_num=1 \
    --load_thread_num=30 \
    --run_thread_num=30 \
    --coro_num=1 \
    --mem_mb=4096 \
    --th_mb=10 \
    --workload_load=2m_load \
    --workload_run=uniform_run \
    --bucket=256 \
    --run_max_request=2000000
```

**Step 3: On compute node (10.30.1.6)**
```bash
cd DART-main
bin/compute --monitor_addr=10.30.1.9:9898 --nic_index=0
```

**Or use the bundled scripts** (handle sequencing automatically):
```bash
# On 10.30.1.9:
bash DART-main/benchmark_run/run_memory_node.sh   # runs all 5 skew configs

# On 10.30.1.6:
bash DART-main/benchmark_run/run_compute_node.sh  # paired with above
```

### Varying Workload Distribution for DART

DART reads workload from YCSB files. To change distribution, change `--workload_run`:

```bash
--workload_run=uniform_run       # uniform access
--workload_run=zipf03_run        # Zipfian θ=0.3
--workload_run=zipf05_run        # Zipfian θ=0.5
--workload_run=zipf08_run        # Zipfian θ=0.8
--workload_run=zipf099_run       # Zipfian θ=0.99 (most skewed)
```

**Expected result: throughput ≈ 1.67 Mops/s for ALL of the above** — confirming DART's skew insensitivity.

---

## 7. Side-by-Side Experiment to See the Crossover

Run these back-to-back to directly compare at each cache level:

```bash
# ── EXPERIMENT: Cache Crossover (uniform, 30 threads) ──────────────────────

# --- DEX at 32 MB cache ---
# (restart memcached between each DEX run)
sudo ./newbench_latency 2 100 0 0 0 0 30 4 32 1 0.99 50 10 50 0 1 0 0 1 0.1 0 30
# Record: "DEX 32MB throughput = X Mops/s, P50 = X µs"

# --- DEX at 64 MB cache ---
sudo ./newbench_latency 2 100 0 0 0 0 30 4 64 1 0.99 50 10 50 0 1 0 0 1 0.1 0 30

# --- DEX at 128 MB cache ---
sudo ./newbench_latency 2 100 0 0 0 0 30 4 128 1 0.99 50 10 50 0 1 0 0 1 0.1 0 30

# --- DEX at 256 MB cache ---
sudo ./newbench_latency 2 100 0 0 0 0 30 4 256 1 0.99 50 10 50 0 1 0 0 1 0.1 0 30

# --- DART (any cache — irrelevant, ~1.67 Mops/s always) ---
# Run benchmark_run/run_memory_node.sh + run_compute_node.sh
```

**Expected observations:**

```
Cache    DEX Throughput   DEX P50     DART Throughput   DART P50   Winner
──────   ──────────────   ───────     ───────────────   ────────   ──────
 32 MB   ~broken/low      ~15-20µs    ~1.67 Mops/s      ~18µs     DART
 64 MB   ~1.5-2 Mops/s    ~8-12µs    ~1.67 Mops/s      ~18µs     DART ≈ DEX
128 MB   ~3-4 Mops/s      ~4-6µs     ~1.67 Mops/s      ~18µs     DEX
256 MB   ~7 Mops/s        ~1-2µs     ~1.67 Mops/s      ~18µs     DEX (clear)
512 MB   ~8+ Mops/s       <1µs       ~1.67 Mops/s      ~18µs     DEX (dominant)
```

Note: Even when DEX "wins" on throughput, DART's P50 is more stable (no variance from cache eviction jitter). DEX P99 can spike 10-50× under cache pressure while DART P99 ≈ 2× P50.

---

## 8. Why DART's Latency Is Higher Even When It "Wins" on Throughput

This seems paradoxical. DART wins on **throughput** at 32-64 MB, yet its **latency** is ~18 µs vs DEX's ~15 µs. How?

```
DEX at 32MB cache (uniform):
  Most ops = 2-3 RDMA RTTs = ~8-15 µs per op
  But many ops hit L0-L2 cache → near-zero latency
  Average pulled down by cache hits
  Throughput still low because the 3-RTT ops are slow

DART at any config:
  Every op = exactly 2 RDMA RTTs = ~18 µs per op
  But async coroutine pipeline: while one op waits for RDMA,
  the coroutine yields and another op starts its RDMA
  → Higher throughput than DEX's synchronous misses at same RTT count

The key: DART's coroutine pipelining (coro_num in monitor) allows
overlapping multiple RDMA operations per thread.
DEX at low cache hit rate does synchronous RDMA → thread stalls.
```

---

## 9. Quick Reference Card

### DEX — One-Line Command (1 memory + 1 compute, 30 threads, 256MB cache, uniform)
```bash
sudo ./newbench_latency 2 100 0 0 0 0 30 4 256 1 0.99 50 10 50 0 1 0 0 1 0.1 0 30
#                       │ │           │  │  │   │ │    │  │  │  │ │ │ │ │   │   └─ kMaxThread=30
#                       │ │           │  │  │   │ │    │  │  │  │ │ │ │ │   └─── admission=0.1
#                       │ │           │  │  │   │ │    │  │  │  │ │ │ │ └─────── rpc_rate=1
#                       │ │           │  │  │   │ │    │  │  │  │ │ │ └───────── index=0 (DEX)
#                       │ │           │  │  │   │ │    │  │  │  │ │ └─────────── early_stop=0
#                       │ │           │  │  │   │ │    │  │  │  │ └───────────── time_based=1
#                       │ │           │  │  │   │ │    │  │  │  └─────────────── check=0
#                       │ │           │  │  │   │ │    │  │  └────────────────── op_num=50M
#                       │ │           │  │  │   │ │    │  └───────────────────── warmup=10M
#                       │ │           │  │  │   │ │    └──────────────────────── bulk=50M
#                       │ │           │  │  │   │ └───────────────────────────── zipf=0.99
#                       │ │           │  │  │   └────────────────────────────── uniform=1
#                       │ │           │  │  └────────────────────────────────── cache=256MB ★
#                       │ │           │  └───────────────────────────────────── memThreads=4
#                       │ │           └──────────────────────────────────────── threads=30
#                       │ └──────────────────────────────────────────────────── read=100%
#                       └────────────────────────────────────────────────────── nodes=2
```

### DART — One-Line Command (memory node)
```bash
# Terminal 1: bin/memory --monitor_addr=10.30.1.9:9898 --nic_index=0
# Terminal 2:
bin/monitor --test_func=0 --memory_num=1 --compute_num=1 \
            --load_thread_num=30 --run_thread_num=30 --coro_num=1 \
            --mem_mb=4096 --th_mb=10 \
            --workload_load=2m_load --workload_run=uniform_run \
            --bucket=256 --run_max_request=2000000
# Compute node:
bin/compute --monitor_addr=10.30.1.9:9898 --nic_index=0
```

### Key Variable to Trigger Crossover

| To see... | Change this |
|-----------|-------------|
| Cache cliff (DEX breaks at low cache) | argv[9] in DEX: try 32 → 64 → 128 → 256 |
| Fat leaf dilution | `kLeafPageSize` in Common.h (recompile) |
| Height effect | `kInternalPageSize` in Common.h (recompile, smaller = taller tree) |
| Skew recovery | argv[10]=0 (zipf) + argv[11]=0.99, increase cache |
| DART flatness | Change `--workload_run` across all zipf levels, observe constant throughput |
