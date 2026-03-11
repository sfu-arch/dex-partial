# DART Benchmark Scripts

## Overview
Runs DART with 100M records, 10M read operations at different skew levels,
matching DEX's benchmark configuration (30 threads, read-only workload).

**Configs:** Uniform, Zipfian 0.3, 0.5, 0.8, 0.99

## Execution Order

### Step 1: Generate workloads (on BOTH nodes)
```bash
cd DART-main
bash benchmark_run/gen_workloads.sh
```
This generates 100M load + 10M run files for each distribution.
Takes a while for 100M records (~5-10 min per file).

### Step 2a: Start memory node (on 10.30.1.9)
```bash
cd DART-main
bash benchmark_run/run_memory_node.sh
```

### Step 2b: Start compute node (on 10.30.1.6)
**Option A — Automatic (recommended):** auto-retries until monitor is ready
```bash
cd DART-main
bash benchmark_run/run_compute_auto.sh
```

**Option B — Manual:** prompts you before each config
```bash
cd DART-main
bash benchmark_run/run_compute_node.sh
```

### Step 3: Collect results
Results are saved to `benchmark_run/results/` on both nodes.
- `dart_<config>.txt` — monitor output (on 10.30.1.9)
- `dart_compute_<config>.txt` — compute output with throughput/latency (on 10.30.1.6)
