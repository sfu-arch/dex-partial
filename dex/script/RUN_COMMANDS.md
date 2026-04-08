# DEX B+ Tree Sweep — Run Commands

Node config: `cs-dis-srv09s` = compute node (node 0, IP `10.30.1.8`), `10.30.1.9` = memory node (node 1, also hosts memcached).

---

## 0. Prerequisites (both nodes, once per session)

```bash
# Enable hugepages (run on BOTH nodes)
sudo ./script/hugepage.sh

# Verify RDMA NICs are up (run on BOTH nodes)
./script/check_nic.sh
```

---

## 1. Build (compute node only)

```bash
cd ~/DEX-CHIME/dex
mkdir -p build && cd build

cmake .. -DCMAKE_BUILD_TYPE=Release

# Build — newbench is the single benchmark binary (newbench_latency was merged into it)
make -j$(nproc) newbench
```

Expected output: `newbench` binary in `build/`.

After any change to `include/cache/btree_node.h` (node sizes), rebuild before running:
```bash
make -j$(nproc) newbench
```

---

## 2. Copy scripts into build directory (compute node)

```bash
cp ../script/run_bp_sweep.sh .
cp ../script/restartMemc.sh  .
chmod +x run_bp_sweep.sh restartMemc.sh
```

---

## 3. Copy scripts into build directory (memory node)

```bash
# SSH to memory node
ssh -p 404 apa222@10.30.1.9

cd ~/DEX-CHIME/dex/build
cp ../script/run_memnode_sweep.sh .
chmod +x run_memnode_sweep.sh
```

---

## 4. Start memcached (compute node — once before first sweep)

The `restartMemc.sh` SSH is failing (publickey), so start memcached manually:

```bash
# On compute node (10.30.1.9 is where memcached runs — adjust if different)
sudo pkill -9 memcached 2>/dev/null; sleep 2
sudo memcached -u root -l 0.0.0.0 -p 11211 -c 10000 -d
sleep 2
printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 10.30.1.9 11211
printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 10.30.1.9 11211
# Expected: STORED  STORED
```

---

## 5. Run the full sweep (both nodes simultaneously)

Start **memory node first**, then compute node within ~60 seconds.
The memory node binary waits in DSMKeeper until the compute node connects.

### Memory node (terminal 1 on 10.30.1.9)
```bash
cd ~/DEX-CHIME/dex/build
./run_memnode_sweep.sh
```

### Compute node (terminal 1 on cs-dis-srv09s)
```bash
cd ~/DEX-CHIME/dex/build
./run_bp_sweep.sh
# log file is created automatically: sweep_YYYYMMDD_HHMM.log
```

The sweep runs (using `newbench` for all runs):
- **Point lookups** × 5 distributions × 3 cache sizes = 15 runs
- **Range queries** × 5 distributions × 3 cache sizes = 15 runs
- **Total: 30 runs** — expect ~15–20 min wall time

Each run produces: throughput + `[DEX]` miss stats + `Avg. rdma read/op` + latency percentiles.

---

## 6. Flush between experiments (automatic)

Both scripts call `flush_memc` / `flush_counters` after each run automatically.
If a run crashes mid-way, manually reset before restarting:

```bash
sudo pkill -9 memcached 2>/dev/null; sleep 2
sudo memcached -u root -l 0.0.0.0 -p 11211 -c 10000 -d
sleep 2
printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 10.30.1.9 11211
printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 10.30.1.9 11211
```

---

## 7. Parse results

### Remote load (key metric) per config
```bash
grep -E '\[SWEEP\]|rdma read / op' sweep_*.log
```

### Full per-run summary
```bash
grep -E '\[SWEEP\]|\[SWEEP_END\]|rdma read / op|Final cluster throughput' sweep_*.log
```

### DEX cache miss counters (printed every 2 s during each run)
```bash
grep '\[DEX\]' sweep_*.log
```

### Latency percentiles per run
```bash
grep 'P50\|P90\|P95\|P99' sweep_*.log
```

### Latency histogram files (raw 500 ns buckets)
```bash
ls build/latency_results/
# Filename: {point|range}_{dist}_{cache}mb_{read|range}.dat
# Header:   # P50 / P90 / P95 / P99 / P99.9 in ns
# Data:     latency_ns <TAB> count
```

---

## 8. Manual single run (quick test)

Both nodes run the **same binary** (`newbench`). Node ID is assigned by memcached
connection order — whoever connects first gets ID 0 (compute), second gets ID 1 (memory).

```bash
# Argument positions (22 args after binary name = argc 23):
#   nodecount R I U D Rng  threads mem_t cache  uni theta  bulk warm run  chk tb es  idx rpc admit tune maxth

# Memory node — start FIRST (waits in DSMKeeper until compute connects)
sudo ./newbench 2 100 0 0 0 0  36 4 128  1 0.99  50 10 50  0 1 0  0 0 0.1 0  36

# Compute node — start within ~60 s of memory node
./restartMemc.sh
sudo ./newbench 2 100 0 0 0 0  36 4 128  1 0.99  50 10 50  0 1 0  0 0 0.1 0  36

# Point lookup, zipf θ=0.99, 256 MB cache
sudo ./newbench 2 100 0 0 0 0  36 4 256  0 0.99  50 10 50  0 1 0  0 0 0.1 0  36

# Range query, uniform, 256 MB cache
sudo ./newbench 2 0 0 0 0 100  36 4 256  1 0.99  50 10 50  0 1 0  0 0 0.1 0  36
```

### Argument order reference (22 args → argc=23)
```
./newbench \
  <nodecount>                          # 1  total physical nodes (mem+compute)
  <read%> <insert%> <update%> <delete%> <range%>  # 2-6  must sum to 100
  <total_threads> <mem_threads>        # 7-8
  <cache_mb>                           # 9   compute-node cache size
  <uniform_workload>                   # 10  1=uniform  0=zipf
  <zipf_theta>                         # 11  ignored when uniform=1
  <bulk_M> <warmup_M> <run_M>          # 12-14  ×1 000 000 ops
  <check_correctness>                  # 15  0=no  1=yes
  <time_based>                         # 16  0=op-count  1=60-s cap
  <early_stop>                         # 17  0=all-finish  1=first-finish-kills
  <index>                              # 18  0=DEX  1=Sherman  2=SMART
  <rpc_rate>                           # 19  fraction of misses routed to RPC
  <admission_rate>                     # 20  fraction of misses admitted to cache
  <autotune>                           # 21  0=fixed params  1=sweep admission_rate_vec
  <max_threads_per_cnode>              # 22  typically 36
```

---

## 9. Node size reference

Defined in `include/cache/btree_node.h` — change and rebuild to experiment.

| Parameter | Value | Effect |
|---|---|---|
| `innerNodeSize` | 256 B | fanout = (256−72)/16 = **11** |
| `leafNodeSize`  | 512 B | capacity = (512−96)/16 = **26** entries |
| `pageSize` (RDMA/cache unit) | 512 B | `max(innerNodeSize, leafNodeSize)` |
| Tree height (50 M keys, seq insert) | **10** | verified in output |
| Leaf nodes | 3.85 M × 512 B = **1.88 GB** | |
| Inner nodes | 961 K × 512 B = **469 MB** | |
| Total DSM usage | **≈ 2.35 GB** | fits in 8 GB DSM |

Other valid configs (change both constants + rebuild):

| innerNodeSize | leafNodeSize | fanout | leaf cap | depth (50 M) |
|---|---|---|---|---|
| 192 B | 192 B | 7 | 6 | 16 (sequential, original try) |
| **256 B** | **512 B** | **11** | **26** | **10 ✓ (current)** |
| 512 B | 1024 B | 27 | 58 | 7 |
| 1024 B | 1024 B | 59 | 58 | 5 (original paper) |

---

## 10. What to look for

| Condition | Expected behaviour |
|---|---|
| **Uniform, 128 MB cache** | `rdma_read/op ≈ 2+`, `leaf_miss ≈ inner_miss`, low throughput (~2 Mops/s) |
| **Uniform, 512 MB cache** | `rdma_read/op` drops as inner nodes start fitting in cache |
| **Zipf θ=0.99, any cache** | `rdma_read/op` much lower; hot leaves admitted despite `admit_rate=0.1` |
| **θ increases (0.30→0.99)** | Monotone drop in `rdma_read/op` and `leaf_miss` |
| **Range vs point (uniform)** | Range has higher `rdma_read/op`; traverses multiple leaves, no offloading |
| **`dirty_wb`** | 0 for read-only; non-zero for update/insert workloads |
| **`inner_miss` vs `leaf_miss`** | Under uniform both grow; under zipf `leaf_miss` drops faster (hot leaves re-admitted) |
