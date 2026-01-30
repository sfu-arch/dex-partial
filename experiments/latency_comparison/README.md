# DEX vs CHIME Latency Comparison Experiment

This experiment compares the latency behavior of DEX and CHIME under 100% read workloads.

## Overview

The experiment captures per-operation latency histograms from both systems and generates superimposed comparison plots.

## Files

| File | Description |
|------|-------------|
| `dex_node0.sh` | DEX benchmark script for compute node |
| `dex_node1.sh` | DEX benchmark script for memory node |
| `chime_microbench_node0.sh` | CHIME microbenchmark for compute node |
| `chime_microbench_node1.sh` | CHIME microbenchmark for memory node |
| `plot_latency_comparison.py` | Python script to generate comparison plots |

## Prerequisites

### Build the latency benchmark binaries

**DEX:**
```bash
cd dex
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j newbench_latency
```

**CHIME (new microbenchmark - no YCSB needed!):**
```bash
cd CHIME
mkdir -p build && cd build
cmake ..
make -j microbench_latency
```

### Python dependencies for plotting

```bash
pip install numpy matplotlib
```

## Running the Experiment

### Cluster Setup

- **Compute Node**: 10.30.1.9 (runs memcached + benchmark)
- **Memory Node**: 10.30.1.6 (RDMA memory server)

### Step 1: Start memcached on compute node

```bash
# On compute node (10.30.1.9)
memcached -d -m 2048 -p 11211
```

### Step 2: Configure memcached IP

Edit `dex/memcached.conf` and `CHIME/memcached.conf` on BOTH nodes:
```
--SERVER=10.30.1.9
```

### Step 3: Run DEX Benchmark

**On Memory Node (10.30.1.6) - start FIRST:**
```bash
bash /path/to/experiments/latency_comparison/dex_node1.sh
```

**On Compute Node (10.30.1.9) - wait 3-5 seconds, then:**
```bash
bash /path/to/experiments/latency_comparison/dex_node0.sh
```

Wait for completion. The `dex_latency.dat` file will be created.

### Step 4: Run CHIME Benchmark

**On Memory Node (10.30.1.6) - start FIRST:**
```bash
bash /path/to/experiments/latency_comparison/chime_microbench_node1.sh
```

**On Compute Node (10.30.1.9) - wait 3-5 seconds, then:**
```bash
bash /path/to/experiments/latency_comparison/chime_microbench_node0.sh
```

Wait for completion. The `chime_latency.dat` file will be created.

### Step 5: Generate Comparison Plots

Copy both `.dat` files to the same directory, then:

```bash
python plot_latency_comparison.py --dex dex_latency.dat --chime chime_latency.dat
```

This generates:
- `latency_comparison.png` - Combined 4-panel plot
- `latency_comparison_histogram.png` - Histogram only
- `latency_comparison_cdf.png` - CDF only

## Output Files

### Latency Data Format

Both `dex_latency.dat` and `chime_latency.dat` use the same format:

```
# System Latency Histogram
# Total ops: 5000000
# Avg: 15.23 us
# P50: 12 us, P90: 25 us, P95: 35 us, P99: 78 us, P99.9: 156 us
# latency_us	count
5	1234567
6	987654
7	654321
...
```

### Plot Description

The main comparison plot includes:

1. **Top-Left: Histogram Comparison**
   - Superimposed latency distributions
   - X-axis: Latency in microseconds
   - Y-axis: Percentage of operations

2. **Top-Right: CDF Comparison**
   - Cumulative distribution functions
   - Shows percentile comparisons

3. **Bottom-Left: Log-Scale Tail Latency**
   - Same histogram with log-scale Y-axis
   - Reveals tail latency behavior

4. **Bottom-Right: Statistics Bar Chart**
   - Direct comparison of key metrics
   - Average, P50, P90, P95, P99, P99.9

## Customizing the Experiment

### Modify Workload Parameters

**DEX (in `dex_node0.sh` and `dex_node1.sh`):**
```bash
READ_RATIO=100        # Change for different read/write mix
ZIPF_THETA=0.99       # Change skew (0.0-0.99)
RUN_M=5               # Number of operations (millions)
TOTAL_THREADS=16      # Thread count
```

**CHIME (in `chime_node0.sh` and `chime_node1.sh`):**
```bash
WORKLOAD="c"          # a/b/c/d/e for different mixes
THREAD_COUNT=16       # Threads per node
```

### Workload Types

| YCSB Workload | Read % | Update % | Insert % | Scan % |
|---------------|--------|----------|----------|--------|
| A | 50 | 50 | 0 | 0 |
| B | 95 | 5 | 0 | 0 |
| C | 100 | 0 | 0 | 0 |
| D | 95 | 0 | 5 | 0 |
| E | 0 | 0 | 5 | 95 |

## Interpreting Results

### Key Metrics

- **P50 (Median)**: Typical latency experienced by most operations
- **P99**: Tail latency affecting 1% of operations
- **P99.9**: Extreme tail latency

### Expected Behavior

- **DEX**: Should show lower average latency due to caching and RPC optimization
- **CHIME**: May show different tail latency characteristics due to its B-tree traversal approach

### Common Issues

1. **Missing workload files**: Run `generate_full_workloads.sh` for CHIME
2. **High latencies**: Check RDMA connectivity, hugepages configuration
3. **memcached errors**: Ensure memcached is running on Node 0

## Citation

If you use this experiment, please cite:
- DEX: [paper reference]
- CHIME: [paper reference]
