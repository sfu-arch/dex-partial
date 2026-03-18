# ATLAS Project Context

**Project**: ATLAS — Interactive Benchmarking of RDMA Databases Under Diverse Query Distributions
**Author**: Abhiram Prasanna, SFU
**Repo**: https://github.com/sfu-arch/dex-partial.git
**Cluster**: 10.30.1.9 (memory/memcached server) · 10.30.1.6 (compute client)

---

## 1. What This Project Is

ATLAS is an experimental framework for identifying **operating points** of RDMA database systems.
It sweeps three axes:
- **Workload axis** — uniform vs Zipfian skew, point reads vs range scans
- **Structure axis** — node sizes, fanout, tree height (compile-time knobs)
- **Cache axis** — client-side cache capacity

Three systems are benchmarked: **DEX**, **CHIME**, and **DART**.

### Core paper questions answered by experiments
1. **When is DEX worse than CHIME?** → small cache + uniform workload (CHIME's path-aware cache wins)
2. **When is CHIME worse than DEX?** → large cache + high Zipf skew + range scans (DEX page cache dominates)
3. **When is DART worse than CHIME?** → any workload with cache headroom (DART has a fixed ~1.67 Mops floor)
4. **What is each system's best case?** → DEX: 256 MB + θ=0.99; CHIME: small cache + uniform; DART: uniform + minimal cache (predictable, no cliff)

---

## 2. Directory Structure

```
DEX-CHIME/
├── dex/                        DEX (Sherman B+tree with page cache)
│   ├── include/Common.h          compile-time config: kLeafPageSize, kInternalPageSize (default 1024B each)
│   ├── test/newbench_latency.cpp runtime benchmark: cache_mb is argv[9]
│   └── memcached.conf            10.30.1.9 : 11211
│
├── CHIME/                      CHIME (B+tree with metadata cache)
│   ├── include/Common.h          compile-time: kIndexCacheSize=100MB, leafSpanSize=64, internalSpanSize=64
│   ├── test/latency_bench.cpp    [NEW] benchmark matching DEX arg interface
│   └── memcached.conf            10.10.1.2 : 11211
│
├── DART-main/                  DART (ART + RACE skip-table, fixed ~1MB cache)
│   ├── src/main/{compute,memory,monitor}.cc
│   └── benchmark_run/            existing DART run scripts
│
├── experiments/
│   ├── qw1_zipfian_skew/         existing: skew sweep (DEX + CHIME, 70/30 mix)
│   ├── qw2_coherent_trie/        existing: CHIME coherence ablation
│   ├── qw3_tree_height_crossover/ existing: tree height cost (DEX 20M vs 100M keys)
│   ├── apex_skew/                existing
│   ├── cache_size_analysis/      existing: README with cache math
│   └── op_crossover/             [NEW] operating point crossover experiments
│       ├── exp_A_cache_crossover_node{0,1}.sh
│       ├── exp_B_skew_crossover_node{0,1}.sh
│       ├── exp_C_dart_vs_chime_node{0,1}.sh
│       ├── exp_D_node_size_node{0,1}.sh
│       ├── plot_crossovers.py
│       ├── results/dex/
│       ├── results/chime/
│       └── results/dart/
│
├── context.md                  [THIS FILE]
└── atlas_paper.tex             paper draft
```

---

## 3. System Configurations (Defaults)

### DEX
| Parameter | Value | Where |
|---|---|---|
| kLeafPageSize | 1024 B | `dex/include/Common.h:159` |
| kInternalPageSize | 1024 B | `dex/include/Common.h:158` |
| kIndexCacheSize | 512 MB (default) | `dex/include/Common.h:143` |
| cache_mb (runtime) | passed as argv[9] | overrides kIndexCacheSize |
| dsmSize | 8 GB | `dex/include/Common.h:99` |
| MAX_APP_THREAD | 36 | |
| Memcached | 10.30.1.9:11211 | `dex/memcached.conf` |

### CHIME
| Parameter | Value | Where |
|---|---|---|
| kIndexCacheSize | 100 MB | `CHIME/include/Common.h:109` — **compile-time** |
| kHotspotBufSize | 30 MB | `CHIME/include/Common.h:110` |
| leafSpanSize | 64 entries | `CHIME/include/Common.h:141` — **compile-time** |
| internalSpanSize | 64 entries | `CHIME/include/Common.h:155` — **compile-time** |
| MEMORY_NODE_NUM | 1 | `CHIME/include/Common.h:30` |
| dsmSize | 64 GB | `CHIME/include/Common.h` |
| Memcached | 10.10.1.2:11211 | `CHIME/memcached.conf` |

> **Important**: CHIME cache size cannot be changed at runtime — must edit `Common.h` and recompile.
> The experiment scripts do this automatically.

### DART
| Parameter | Value | Notes |
|---|---|---|
| Skip-table cache | ~1 MB | local, always resident |
| Per-thread RDMA buffer | 10 MB × 30 threads | scratch, overwritten per lookup |
| Avg RTTs/lookup | ~4.5 | fixed regardless of skew |
| Avg bytes/lookup | ~1820 B | fixed regardless of skew |
| Peak throughput | ~1.67 Mops | cannot improve with cache tuning |
| Architecture | 3-process | monitor + memory + compute |

---

## 4. Benchmark Binary Interfaces

### DEX — `newbench_latency`
```
./newbench_latency \
  <node_count>     \   # 2
  <read_ratio>     \   # 0-100
  <insert_ratio>   \   # 0
  <update_ratio>   \   # 0
  <delete_ratio>   \   # 0
  <range_ratio>    \   # 0-100
  <total_threads>  \   # 30
  <mem_threads>    \   # 4
  <cache_mb>       \   # runtime cache size
  <uniform>        \   # 1=uniform, 0=zipfian
  <zipf_theta>     \   # e.g. 0.99
  <bulk_load_M>    \   # millions of keys to preload
  <warmup_M>       \   # millions of warmup ops
  <run_M>          \   # millions of measurement ops
  <check>          \   # 0
  <time_based>     \   # 0
  <early_stop>     \   # 0
  <tree_index>     \   # 0=DEX/Sherman
  <rpc_rate>       \   # 0.0
  <admit_rate>     \   # 1.0
  <auto_tune>      \   # 0
  <max_thread>        # 30
```

Output files (written in cwd): `dex_read_latency.dat`, `dex_range_latency.dat`

### CHIME — `latency_bench` (NEW)
```
./latency_bench \
  <node_count>   \   # 2
  <thread_count> \   # 30
  <read_ratio>   \   # 0-100
  <range_ratio>  \   # 0-100
  <total_ops>    \   # absolute count e.g. 10000000
  <range_size>   \   # keys per range scan
  <zipf_theta>   \   # e.g. 0.99 (ignored if uniform=1)
  <uniform>      \   # 1=uniform, 0=zipfian
  [bulk_load_M]      # optional, millions of keys, default 10
```

Output files (written in cwd): `chime_read_latency.dat`, `chime_range_latency.dat`

### Latency `.dat` file format (same for both)
```
# header lines starting with #
# latency_ns<TAB>count
500     12
1000    45
1500    88
...
```
Bucket size: 500 ns. Range: 0–50 ms (100 000 buckets).

---

## 5. Memcached Coordination

Both DEX and CHIME use memcached for multi-node DSM initialization.
**Node 0** (10.30.1.9) hosts memcached and is the **memory server**.
**Node 1** (10.30.1.6) is the **compute client**.

The DSM init protocol uses two keys:
- `serverNum` — memory servers increment this on startup
- `clientNum` — compute clients increment this on startup

**Critical rule**: Memcached must be **killed and restarted** between runs, not just flushed.
Stale `serverNum`/`clientNum` values from a previous run cause the DSM to hang indefinitely.

The scripts additionally use `exp_iter` (an integer counter) as a cross-node run sentinel:
- Node 0 increments `exp_iter` after restarting memcached for each run
- Node 1 polls `exp_iter` and starts the compute binary only after seeing the new value

**Common hang causes and fixes**:
| Symptom | Cause | Fix |
|---|---|---|
| Binary hangs at startup | stale `serverNum`/`clientNum` | kill+restart memcached, not just flush_all |
| `wait_for_iter` loops forever | `\r\n` in memcached response | already fixed: `tr -d '\r'` in all scripts |
| Node0 server exits before node1 connects | node1 still compiling | already fixed: all builds moved to Phase 1 upfront |
| RDMA registration error | stale `/dev/shm/dsm_*` files | `sudo rm -f /dev/shm/dsm_* /dev/shm/rdma_*` |

---

## 6. New Files Created (this session)

### `CHIME/test/latency_bench.cpp`
The CHIME benchmark binary that was missing. Key properties:
- Uses CityHash64 for key derivation (identical to DEX — ensures fair comparison)
- Supports Zipfian and uniform distributions via the same `zipf.h` library
- Tracks per-thread latency histograms (no atomic contention)
- Only the compute node (myNodeID >= MEMORY_NODE_NUM) runs the workload and saves .dat files
- Memory node participates in barriers only

### `experiments/op_crossover/` — 4 experiment sets

Each experiment has a `_node0.sh` (memory server, runs on 10.30.1.9) and `_node1.sh` (compute, runs on 10.30.1.6).

**Script design pattern** (all 8 scripts follow this):
1. **Phase 1: Build** — all binary variants compiled and saved to `/tmp/` with encoded names (e.g. `/tmp/latency_bench_cache32`)
2. **Phase 2+: Run loop** — node0 restarts memcached + sets `exp_iter`; node1 polls `exp_iter` then runs compute

---

## 7. Execution Guide

### Prerequisites (run once on each server after cloning)

```bash
# Verify RDMA libraries
ldconfig -p | grep -E "ibverbs|memcached|cityhash|tbb|numa|boost"

# Install if any are missing
sudo apt-get install -y \
    libibverbs-dev libmemcached-dev libnuma-dev \
    libcityhash-dev libtbb-dev \
    libboost-coroutine-dev libboost-context-dev libboost-system-dev

# Hugepages — set permanently (survives reboot)
echo "vm.nr_hugepages = 36864" | sudo tee -a /etc/sysctl.conf
sudo sysctl -p

# Memcached — must be installed on 10.30.1.9
sudo apt-get install -y memcached
```

### Clone / Pull

```bash
# First time
git clone https://github.com/sfu-arch/dex-partial.git DEX-CHIME
cd DEX-CHIME

# Subsequent pulls
git pull origin main
```

---

### Experiment A — Cache Size Crossover

**Question**: When is DEX worse than CHIME?
**Config**: uniform, 100% reads, 10M keys, 30 threads
**DEX cache sweep**: 64, 128, 256, 512 MB
**CHIME cache sweep**: 32, 64, 100 MB
**Expected result**: At 64 MB, CHIME beats DEX; at 256 MB, DEX pulls ahead.

```bash
# On 10.30.1.9 (memory server):
cd DEX-CHIME/experiments/op_crossover
bash exp_A_cache_crossover_node0.sh

# On 10.30.1.6 (compute — start AFTER node0 prints "PHASE 2"):
cd DEX-CHIME/experiments/op_crossover
bash exp_A_cache_crossover_node1.sh
```

**Results saved to** (on node 1): `results/dex/expA_dex_cache{64,128,256,512}mb_*`
and `results/chime/expA_chime_cache{32,64,100}mb_*`

---

### Experiment B — Zipfian Skew Crossover

**Question**: When is CHIME worse than DEX?
**Config**: 256 MB cache for both, 70% read + 30% range, 10M keys, 30 threads
**Skew sweep**: uniform, θ=0.6, 0.8, 0.9, 0.99
**Expected result**: Both improve with skew; DEX scales 57%, CHIME 28%. DEX wins at θ≥0.8.

```bash
# On 10.30.1.9:
bash exp_B_skew_crossover_node0.sh

# On 10.30.1.6:
bash exp_B_skew_crossover_node1.sh
```

**Results**: `results/dex/expB_dex_{uniform,zipf_0.6,...}_*` and `results/chime/expB_chime_*`

---

### Experiment C — DART vs CHIME

**Question**: When is DART worse than CHIME, and what is DART's best case?
**Config**: 2M keys, 30 threads, 100% reads, uniform AND θ=0.99
**CHIME cache sweep**: 4, 16, 32, 64, 100 MB
**DART**: fixed ~1 MB (no sweep)
**Expected result**: CHIME wins once cache ≥ ~16 MB; DART is the floor at all sizes.

```bash
# On 10.30.1.9:
bash exp_C_dart_vs_chime_node0.sh

# On 10.30.1.6:
bash exp_C_dart_vs_chime_node1.sh
```

> **Note for DART**: DART workload files must be pre-generated on node 1.
> If `DART-main/workload/split/` is missing:
> ```bash
> cd DEX-CHIME/DART-main
> python3 benchmark_script/gen_workload.py   # or check benchmark_run/gen_workloads.sh
> ```

**Results**: `results/chime/expC_chime_*` and `results/dart/expC_dart_*`

---

### Experiment D — Node Size Structural Sweep

**Question**: How does node size shift operating points?
**Config**: 128 MB cache, uniform, 100% reads, 10M keys, 30 threads
**DEX leaf sizes**: 512, 1024, 2048 bytes (patches `kLeafPageSize` + `kInternalPageSize`)
**CHIME span sizes**: 32, 64, 128 entries (patches `leafSpanSize` + `internalSpanSize`)
**Expected result**: Smaller nodes need more cache to cover the same index; larger nodes are more cache-efficient but have lower fanout.

```bash
# On 10.30.1.9:
bash exp_D_node_size_node0.sh

# On 10.30.1.6:
bash exp_D_node_size_node1.sh
```

**Results**: `results/dex/expD_dex_leaf{512,1024,2048}_*` and `results/chime/expD_chime_span{32,64,128}_*`

---

### Existing Experiments (already in repo)

#### QW1 — Zipfian Skew Sweep
```bash
cd experiments/qw1_zipfian_skew

# On 10.30.1.9:
bash qw1_dex_node0.sh     # runs DEX memory server through all skew points
bash qw1_chime_node0.sh   # then CHIME memory server

# On 10.30.1.6:
bash qw1_dex_node1.sh
bash qw1_chime_node1.sh
```
Config: 70/30 read/range, 10M keys, 256 MB cache, 30 threads.

#### QW3 — Tree Height Crossover
```bash
cd experiments/qw3_tree_height_crossover

# On 10.30.1.9:
bash qw3_dex_node0.sh
bash qw3_chime_node0.sh

# On 10.30.1.6:
bash qw3_dex_node1.sh
bash qw3_chime_node1.sh
```
Config: 64 MB cache, 100% reads, uniform, 100M keys (height 6).

#### DART — Existing benchmark scripts
```bash
cd DART-main/benchmark_run

# On 10.30.1.9 (monitor + memory):
bash run_monitor.sh    # start first
bash run_memory.sh     # start second

# On 10.30.1.6 (compute):
bash run_compute_node.sh
```

---

## 8. Plotting Results

After running experiments on the servers, copy the `results/` directory back to your laptop:

```bash
# From local machine:
scp -r user@10.30.1.6:DEX-CHIME/experiments/op_crossover/results \
    /path/to/local/DEX-CHIME/experiments/op_crossover/

# Generate all figures:
cd experiments/op_crossover
python3 plot_crossovers.py --exp all

# Or individual experiments:
python3 plot_crossovers.py --exp A
python3 plot_crossovers.py --exp B
python3 plot_crossovers.py --exp C
python3 plot_crossovers.py --exp D
```

Figures saved to `experiments/op_crossover/figures/` as both `.pdf` and `.png`.

Requires: `numpy`, `matplotlib`
```bash
pip install numpy matplotlib
```

---

## 9. Compile-Time Knobs Reference

When modifying structural parameters, edit the header file, rebuild, then restore defaults.
The experiment scripts do this automatically, but here are the locations for manual changes:

### DEX — `dex/include/Common.h`
```cpp
constexpr uint32_t kInternalPageSize = 1024;  // line 158 — bytes per internal node
constexpr uint32_t kLeafPageSize = 1024;       // line 159 — bytes per leaf node
constexpr int kIndexCacheSize = 512;           // line 143 — MB (overridden by runtime arg)
```

### CHIME — `CHIME/include/Common.h`
```cpp
#define MEMORY_NODE_NUM 1                      // line 30  — how many memory nodes
constexpr int kIndexCacheSize  = 100;          // line 109 — MB total index cache
constexpr int kHotspotBufSize  = 30;           // line 110 — MB for hotspot fingerprints
constexpr uint32_t leafSpanSize    = 64;       // line 141 — entries per leaf node
constexpr uint32_t internalSpanSize = 64;      // line 155 — entries per internal node
```

After editing: `cd CHIME && rm -rf build && mkdir build && cd build && cmake .. && make -j$(nproc) latency_bench`

---

## 10. Troubleshooting

### Experiment hangs immediately
```bash
# On node0: kill everything and reset
sudo pkill -9 newbench_latency; sudo pkill -9 latency_bench
sudo pkill -9 memcached
sudo rm -f /dev/shm/dsm_* /dev/shm/rdma_*
sleep 3
sudo memcached -u root -l 0.0.0.0 -p 11211 -c 10000 -m 256 -d
sleep 2
printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -q1 -w 2 10.30.1.9 11211
printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -q1 -w 2 10.30.1.9 11211
```

### CHIME hangs after bulk load
CHIME's tree root init (`init_root=true`) is only for the compute node. If node0 runs as compute accidentally (wrong connection order), the memory server hangs. Ensure node0 starts its binary BEFORE node1.

### Hugepages not available
```bash
grep HugePages_Free /proc/meminfo
# if 0:
echo 36864 | sudo tee /proc/sys/vm/nr_hugepages
# if still failing, check available memory (need ~72 GB for 36864 × 2MB pages)
# scale down: try 8192 hugepages for smaller experiments
```

### Build fails on CHIME
```bash
# check which library is missing:
cd CHIME && mkdir build && cd build
cmake .. 2>&1 | grep -i "not found\|missing\|error"
# common: boost coroutine — install libboost-all-dev
sudo apt-get install -y libboost-all-dev
```

### DEX timeout at 32 MB cache
This is expected — DEX needs ~170 MB to serve 10M keys without RDMA saturation.
At 32 MB, the upper B+tree levels don't fit and every lookup requires full traversal → NIC saturated → timeout.
This is the key Exp A finding (DEX's worst case / CHIME's best case).
