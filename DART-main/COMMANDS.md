# DART — Command Reference

All commands are run from the **DART-main/** directory.
Cluster: memory/monitor node = `10.30.1.9`, compute node = `10.30.1.6`.

---

## 1. Build

```bash
# Clone (first time only)
git clone <repo>
git submodule update --init --recursive

# Dependencies
sudo apt install libboost-context-dev libboost-coroutine-dev

# Build all three binaries → bin/monitor  bin/memory  bin/compute
cmake -B build
cmake --build build

# Rebuild after code changes
cmake --build build
```

---

## 2. System setup (run on every node before benchmarking)

```bash
# Hugepages — required for RDMA registration
sudo sysctl -w vm.nr_hugepages=16384

# Make persistent across reboots (optional)
echo "vm.nr_hugepages=16384" | sudo tee -a /etc/sysctl.conf
```

---

## 3. Process roles

DART always needs **three processes** started in this order:

| Process | Binary | Node | Role |
|---------|--------|------|------|
| Monitor | `bin/monitor` | 10.30.1.9 | Orchestrates the experiment; sends config to compute; collects throughput |
| Memory  | `bin/memory`  | 10.30.1.9 | Serves RDMA read/write requests; holds the tree data |
| Compute | `bin/compute` | 10.30.1.6 | Runs the workload; drives tree operations over RDMA |

Start order: **monitor → memory → compute** (monitor must be listening before others connect).

---

## 4. Single-node loopback test (local dev, all on one machine)

```bash
# Terminal 1 — monitor
bin/monitor \
  --test_func=1 \
  --memory_num=1 --compute_num=1 \
  --load_thread_num=4 --run_thread_num=4 \
  --coro_num=1 \
  --mem_mb=4096 --th_mb=10 \
  --bucket=256 \
  --workload_load=unused --workload_run=unused

# Terminal 2 — memory
bin/memory --monitor_addr=127.0.0.1:9898 --nic_index=0

# Terminal 3 — compute  (microbench, 1 M keys, 100% read, zipfian 0.99)
bin/compute \
  --monitor_addr=127.0.0.1:9898 \
  --nic_index=0 \
  --mb_bulk_load_num=1000000 \
  --mb_warmup_num=500000 \
  --mb_op_num=1000000 \
  --mb_read_ratio=100 \
  --mb_insert_ratio=0 \
  --mb_update_ratio=0 \
  --mb_delete_ratio=0 \
  --mb_range_ratio=0 \
  --mb_uniform=false \
  --mb_zipfian=0.99 \
  --mb_scan_num=100 \
  --mb_node_id=0
```

---

## 5. Microbenchmark — manual single run (two-node cluster)

### 5a. Memory/monitor node (10.30.1.9)

```bash
# Terminal 1 — monitor  (test_func=1 = microbench mode)
bin/monitor \
  --test_func=1 \
  --memory_num=1 --compute_num=1 \
  --load_thread_num=30 --run_thread_num=30 \
  --coro_num=1 \
  --mem_mb=32768 \
  --th_mb=8 \
  --bucket=256 \
  --workload_load=unused --workload_run=unused

# Terminal 2 — memory
bin/memory --monitor_addr=10.30.1.9:9898 --nic_index=0
```

### 5b. Compute node (10.30.1.6)

**Point lookup — 100% read, zipfian 0.99, 50 M keys:**
```bash
bin/compute \
  --monitor_addr=10.30.1.9:9898 \
  --nic_index=0 \
  --mb_bulk_load_num=50000000 \
  --mb_warmup_num=10000000 \
  --mb_op_num=50000000 \
  --mb_read_ratio=100 \
  --mb_insert_ratio=0 \
  --mb_update_ratio=0 \
  --mb_delete_ratio=0 \
  --mb_range_ratio=0 \
  --mb_uniform=false \
  --mb_zipfian=0.99 \
  --mb_scan_num=100 \
  --mb_node_id=0
```

**Range query — 100% range scan, zipfian 0.99:**
```bash
bin/compute \
  --monitor_addr=10.30.1.9:9898 \
  --nic_index=0 \
  --mb_bulk_load_num=50000000 \
  --mb_warmup_num=10000000 \
  --mb_op_num=50000000 \
  --mb_read_ratio=0 \
  --mb_insert_ratio=0 \
  --mb_update_ratio=0 \
  --mb_delete_ratio=0 \
  --mb_range_ratio=100 \
  --mb_uniform=false \
  --mb_zipfian=0.99 \
  --mb_scan_num=100 \
  --mb_node_id=0
```

**Mixed workload — 50% read, 50% update, uniform:**
```bash
bin/compute \
  --monitor_addr=10.30.1.9:9898 \
  --nic_index=0 \
  --mb_bulk_load_num=50000000 \
  --mb_warmup_num=10000000 \
  --mb_op_num=50000000 \
  --mb_read_ratio=50 \
  --mb_insert_ratio=0 \
  --mb_update_ratio=50 \
  --mb_delete_ratio=0 \
  --mb_range_ratio=0 \
  --mb_uniform=true \
  --mb_zipfian=0.99 \
  --mb_scan_num=100 \
  --mb_node_id=0
```

### Compute gflags reference

| Flag | Default | Description |
|------|---------|-------------|
| `--mb_bulk_load_num` | 10000000 | Keys pre-loaded before benchmarking |
| `--mb_warmup_num` | 5000000 | Untimed warmup ops (total across threads) |
| `--mb_op_num` | 10000000 | Timed benchmark ops (total across threads) |
| `--mb_read_ratio` | 100 | % Lookup ops — must sum to 100 with others |
| `--mb_insert_ratio` | 0 | % Insert ops |
| `--mb_update_ratio` | 0 | % Update ops |
| `--mb_delete_ratio` | 0 | % Delete ops |
| `--mb_range_ratio` | 0 | % Range scan ops |
| `--mb_uniform` | false | `true` = uniform distribution, `false` = zipfian |
| `--mb_zipfian` | 0.99 | Zipfian theta (ignored when `--mb_uniform=true`) |
| `--mb_scan_num` | 100 | Keys fetched per range scan op |
| `--mb_node_id` | 0 | Compute node id (seeds workload RNG differently per node) |

### Monitor gflags reference

| Flag | Default | Description |
|------|---------|-------------|
| `--test_func` | — | `0` = YCSB file-based, `1` = microbench in-memory |
| `--memory_num` | — | Number of memory nodes |
| `--compute_num` | — | Number of compute nodes |
| `--load_thread_num` | — | Threads for bulk-load phase |
| `--run_thread_num` | — | Threads for benchmark phase |
| `--coro_num` | 2 | Coroutines per thread (for RACE skip table) |
| `--mem_mb` | 1024 | Remote memory pool size (MB) |
| `--th_mb` | 0 | Per-thread local buffer size (MB) — affects ART node caching |
| `--bucket` | 256 | RACE skip-table hash buckets |
| `--workload_load` | — | Load workload file path (YCSB mode only; pass `unused` for microbench) |
| `--workload_run` | — | Run workload file path (YCSB mode only; pass `unused` for microbench) |
| `--run_max_request` | -1 | Max ops per run (YCSB mode only) |

---

## 6. Microbench automated sweep (distribution × cache × op-type)

**30 experiments total:** 5 distributions × 3 cache sizes × 2 op types.

```bash
# On 10.30.1.9 — start first
bash benchmark_run/run_dart_memnode_sweep.sh

# On 10.30.1.6 — start within ~60 s
bash benchmark_run/run_dart_compute_sweep.sh
```

Results land in `benchmark_run/results/`:
- `dart_monitor_<op>_<dist>_<cache>mb.txt` — throughput from monitor side
- `dart_compute_<op>_<dist>_<cache>mb.txt` — throughput + RDMA stats from compute side
- `dart_memnode_sweep_<timestamp>.log` — full memory/monitor log
- `dart_compute_sweep_<timestamp>.log` — full compute log

**Parse results:**
```bash
# Throughput (MOps/s)
grep "throughput" benchmark_run/results/dart_compute_*.txt

# Latency
grep "latency" benchmark_run/results/dart_compute_*.txt

# RDMA reads per op
grep "Avg. rdma read / op" benchmark_run/results/dart_compute_*.txt

# All stats for one run
cat benchmark_run/results/dart_compute_point_uniform_8mb.txt
```

**Sweep tuning** — edit the top of each script:

```bash
# benchmark_run/run_dart_memnode_sweep.sh
TH_MBS=(2 4 8)       # local cache per thread (MB) — memory node controls this
MEM_MB=32768          # remote memory pool size

# benchmark_run/run_dart_compute_sweep.sh
BULK_M=50             # million keys to bulk-load
WARMUP_M=10           # million untimed warmup ops
RUN_M=50              # million timed ops
SCAN_NUM=100          # keys per range scan
```

---

## 7. YCSB file-based benchmark (original mode, test_func=0)

### Generate workload files

```bash
# Download YCSB tool (once)
python3 script/workload_download.py

# Generate workload files (takes 5–10 min for 100 M records)
bash script/workload_gen.sh

# Split and distribute to multiple compute nodes (if needed)
python3 script/split_and_send_workload.py \
  --inputs a_load a_run \
  --outputs a_load_split a_run_split \
  --ips 10.30.1.6
```

### Run YCSB sweep (automated, 5 distributions)

```bash
# On 10.30.1.9 — memory + monitor combined (blocks until all 5 configs done)
bash benchmark_run/run_memory_node.sh

# On 10.30.1.6 — compute (auto-retries each config)
bash benchmark_run/run_compute_auto.sh
```

### Run YCSB manually (single config)

```bash
# On 10.30.1.9 — Terminal 1: monitor
bin/monitor \
  --test_func=0 \
  --memory_num=1 --compute_num=1 \
  --load_thread_num=30 --run_thread_num=30 \
  --coro_num=1 \
  --mem_mb=4096 --th_mb=10 \
  --bucket=256 \
  --workload_load=2m_load \
  --workload_run=uniform_run \
  --run_max_request=2000000

# On 10.30.1.9 — Terminal 2: memory
bin/memory --monitor_addr=10.30.1.9:9898 --nic_index=0

# On 10.30.1.6 — compute
bin/compute --monitor_addr=10.30.1.9:9898 --nic_index=0
```

---

## 8. Kill stale processes

```bash
# On any node — kill all DART binaries
pkill -9 monitor; pkill -9 memory; pkill -9 compute

# Check nothing is left
pgrep -la monitor; pgrep -la memory; pgrep -la compute

# Free any stuck RDMA resources (if IB errors appear)
sudo rdma_stats 2>/dev/null || true
```

---

## 9. Script inventory

| Script | Node | Purpose |
|--------|------|---------|
| `benchmark_run/run_dart_memnode_sweep.sh` | 10.30.1.9 | **Microbench sweep** — monitor + memory, all 30 experiments |
| `benchmark_run/run_dart_compute_sweep.sh` | 10.30.1.6 | **Microbench sweep** — compute, all 30 experiments |
| `benchmark_run/run_memory_node.sh` | 10.30.1.9 | YCSB sweep — monitor + memory combined |
| `benchmark_run/run_compute_auto.sh` | 10.30.1.6 | YCSB sweep — compute, auto-retry |
| `benchmark_run/run_monitor.sh` | 10.30.1.9 | YCSB — monitor only (3-terminal mode) |
| `benchmark_run/run_memory.sh` | 10.30.1.9 | YCSB — memory only (3-terminal mode) |
| `benchmark_run/run_compute_node.sh` | 10.30.1.6 | YCSB — compute, manual prompt per config |
| `benchmark_run/run_all_monitor.sh` | 10.30.1.9 | YCSB — monitor loop (no memory) |
| `benchmark_run/run_all_memory.sh` | 10.30.1.9 | YCSB — memory loop |
| `benchmark_run/run_all_compute.sh` | 10.30.1.6 | YCSB — compute loop |
