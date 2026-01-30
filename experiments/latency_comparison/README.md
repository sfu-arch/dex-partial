# DEX vs CHIME Latency Comparison Experiment

This experiment compares the latency behavior of DEX and CHIME under 100% read workloads.

## Overview

The experiment captures per-operation latency histograms from both systems and generates superimposed comparison plots.

## Files

| File | Description |
|------|-------------|
| `dex_node0.sh` | DEX benchmark script for primary compute node |
| `dex_node1.sh` | DEX benchmark script for worker compute nodes |
| `chime_node0.sh` | CHIME benchmark script for primary compute node |
| `chime_node1.sh` | CHIME benchmark script for worker compute nodes |
| `plot_latency_comparison.py` | Python script to generate comparison plots |

## Prerequisites

### Build the latency benchmark binaries

**DEX:**
```bash
cd dex/build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j newbench_latency
```

**CHIME:**
```bash
cd CHIME/build
cmake ..
make -j ycsb_test_latency
```

### Generate workload files (for CHIME)

```bash
cd CHIME/ycsb
bash generate_full_workloads.sh
```

### Python dependencies for plotting

```bash
pip install numpy matplotlib
```

## Running the Experiment

### Step 1: Configure memcached IP

Edit `dex/memcached.conf` and `CHIME/memcached.conf` on ALL nodes:
```
<IP_OF_COMPUTE_NODE_0>
11211
```

### Step 2: Run DEX Benchmark

**On Compute Node 0 (Primary):**
```bash
cd dex/build
bash ../../experiments/latency_comparison/dex_node0.sh
```

**On Compute Node 1+ (Wait ~5s after Node 0):**
```bash
cd dex/build
bash ../../experiments/latency_comparison/dex_node1.sh
```

Wait for completion. The `dex_latency.dat` file will be created in the build directory.

### Step 3: Run CHIME Benchmark

**On Compute Node 0 (Primary):**
```bash
cd CHIME/build
bash ../../experiments/latency_comparison/chime_node0.sh
```

**On Compute Node 1+ (Wait ~5s after Node 0):**
```bash
cd CHIME/build
bash ../../experiments/latency_comparison/chime_node1.sh
```

Wait for completion. The `chime_latency.dat` file will be created in the build directory.

### Step 4: Generate Comparison Plots

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
