# QW3: Tree Height / Key Count Crossover Experiment

## Objective
Find the operating point where CHIME outperforms DEX under varying tree heights
(controlled by bulk-loaded key count) with a **fixed 64MB cache**.

## Hypothesis
- **DEX**: Performance degrades with more keys because:
  - Full page (1024B) fetched on every cache miss
  - 64MB cache can only hold ~65K pages → ~19% of 10M-key tree
  - Under uniform distribution: ~81% cache miss rate
  
- **CHIME**: Performance remains stable because:
  - Hopscotch reads only 8 entries (~150B) per point lookup
  - Tree height increase only affects inner node traversal (same cost as DEX)
  - Leaf access cost is decoupled from leaf size

## Experiment Parameters

### Fixed Parameters
| Parameter | Value | Rationale |
|-----------|-------|-----------|
| Cache size | **64 MB** | Stress test caching |
| Threads | 30 | Match existing experiments |
| Read ratio | 100% | Isolate point lookup performance |
| Range ratio | 0% | Focus on hopscotch benefit |
| Operations | 10M | Enough for stable latency measurement |
| Warmup | 1M | Warm up cache |

### Sweep Variables

#### Key Counts (bulk_load)
| Keys (Millions) | ~DEX Leaf Nodes | ~Tree Height |
|-----------------|-----------------|--------------|
| 1 | ~17K | 4 |
| 5 | ~86K | 4-5 |
| 10 | ~172K | 5 |
| 20 | ~345K | 5-6 |
| 50 | ~862K | 6 |
| 100 | ~1.7M | 6-7 |

#### Access Distributions
- **Uniform** (zipfian=0.0): Worst case for caching
- **Zipfian θ=0.6**: Mild skew
- **Zipfian θ=0.99**: High skew (best case for caching)

## Cache Analysis at 64MB

| System | Page Size | Cache Capacity | Notes |
|--------|-----------|----------------|-------|
| DEX | 1024B | 65,536 pages | Caches full pages |
| CHIME | ~1KB | ~65K nodes | But reads only 8 entries per lookup |

### Working Set Coverage (10M keys)
- **DEX**: 65,536 / 344,827 = **19%** leaf coverage
- Under uniform: **~81% cache miss rate**

## Expected Results

### Under Uniform Distribution:
- DEX latency increases significantly with key count
- CHIME latency increases only slightly (inner node traversal)
- **Crossover point**: Where CHIME's lower per-lookup cost beats DEX's caching

### Under Zipfian 0.99:
- Both benefit from skew, but DEX benefits more
- Crossover point shifts to higher key counts (or may not occur)

## File Structure
```
qw3_tree_height_crossover/
├── README.md                    # This file
├── qw3_dex_node0.sh            # DEX primary node script
├── qw3_dex_node1.sh            # DEX secondary node script
├── qw3_chime_node0.sh          # CHIME memory node script
├── qw3_chime_node1.sh          # CHIME compute node script
├── plot_qw3.py                 # Plotting script
└── results/
    ├── dex/                    # DEX output logs
    └── chime/                  # CHIME output logs
```

## Usage

### Step 1: Run DEX experiments
```bash
# On Node 0 (primary):
./qw3_dex_node0.sh

# On Node 1 (secondary, start after node0 is ready):
./qw3_dex_node1.sh
```

### Step 2: Run CHIME experiments
```bash
# On Node 0 (memory node):
./qw3_chime_node0.sh

# On Node 1 (compute node, start after node0 is ready):
./qw3_chime_node1.sh
```

### Step 3: Plot results
```bash
python plot_qw3.py
```

## Key Metrics Collected
1. **P50 Latency** (median)
2. **P99 Latency** (tail)
3. **Average Latency**
4. **Throughput** (ops/sec)
5. **Tree Height** (from logs)
6. **Cache Hit Rate** (DEX only, from internal counters)
