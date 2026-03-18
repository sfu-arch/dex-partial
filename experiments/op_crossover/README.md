# Operating Point Crossover Experiments

These experiments identify where each system wins and loses, using matched configurations.

## Three Crossover Questions

### 1. When is DEX WORSE than CHIME? (CHIME's best case: path-aware cache)
- **Condition**: Small cache + uniform workload
- **Why**: CHIME caches only navigation metadata (~36 MB total). DEX caches full B+tree pages
  and needs ~170 MB for 10M keys to avoid RDMA saturation. Below 64 MB, DEX times out.
- **Scripts**: `exp_A_cache_crossover_node{0,1}.sh`
- **Axis swept**: Cache size (16, 32, 64, 128, 256 MB) with uniform distribution

### 2. When is CHIME WORSE than DEX? (DEX's best case: aggressive page cache)
- **Condition**: Large cache + high skew + range queries
- **Why**: DEX at 256 MB with θ=0.99 serves most reads from local cache (zero RDMA).
  CHIME always issues at least 1 RDMA per lookup. Range scans: DEX sequential layout
  gives 8× lower P50 than CHIME's non-contiguous layout.
- **Scripts**: `exp_B_skew_crossover_node{0,1}.sh`
- **Axis swept**: Zipf θ (uniform, 0.6, 0.8, 0.9, 0.99) with 256 MB cache

### 3. When is DART WORSE than CHIME? (DART's best case: predictable floor)
- **Condition**: DART is almost always slower in absolute throughput. DART's best case is
  minimum-cache + uniform where CHIME's cache advantage is smallest.
- **Scripts**: `exp_C_dart_vs_chime_node{0,1}.sh`
- **Axis swept**: CHIME cache size (4, 16, 32, 64, 100 MB) vs DART fixed ~1 MB

### 4. Node Size Structural Sweep
- **Condition**: Vary leaf/internal node sizes to shift operating points
- DEX: kLeafPageSize / kInternalPageSize (compile-time in dex/include/Common.h)
- CHIME: leafSpanSize / internalSpanSize (compile-time in CHIME/include/Common.h)
- **Scripts**: `exp_D_node_size_node{0,1}.sh`
- **Axis swept**: Node sizes {256, 512, 1024, 2048} bytes

## How to Run

Each experiment has a node0 and node1 script.
- **Node 0** = memory server (runs first, provides RDMA memory)
- **Node 1** = compute client (runs after node0, executes queries, saves .dat files)

```bash
# On Node 0:
bash exp_A_cache_crossover_node0.sh

# On Node 1 (start ~5s after node0):
bash exp_A_cache_crossover_node1.sh
```

## Comparable Configurations

All comparisons hold constant:
- Thread count: 30
- Key count: 10M (2M for DART)
- Operation count: 10M per config point
- Warmup: 1M ops

Only one axis varies per experiment.

## Results

Results are saved in `results/{dex,chime,dart}/`.
Use `plot_crossovers.py` to generate graphs.
