# QW3: Tree Height Crossover Experiment Results

## Objective

Analyze the impact of tree height on DEX B+ tree performance by comparing workloads with different key counts (20M vs 100M keys) while keeping the cache size fixed at 64MB.

## Experimental Configuration

| Parameter | Value |
|-----------|-------|
| Cache Size | 64 MB |
| Page Size (Leaf) | 1024 bytes |
| Page Size (Inner) | 1024 bytes |
| Max Leaf Entries | 58 |
| Max Inner Entries | 59 |
| Read Ratio | 100% |
| Total Operations | 10,000,000 |
| Warmup Operations | 1,000,000 |
| Thread Count | 30 |
| Memory Threads | 4 |
| Node Count | 2 |
| Workload Distribution | Uniform |

## DEX Leaf Entry Structure

```cpp
class LeafEntry {
  uint8_t f_version : 4;   // 4 bits
  Key key;                  // 8 bytes (uint64_t)
  Value value;              // 8 bytes (uint64_t)
  uint8_t r_version : 4;   // 4 bits
} __attribute__((packed));
```

Entry size: ~17 bytes per key-value pair

Leaf cardinality calculation:
```
kLeafCardinality = (1024 - sizeof(Header) - 2 - 8) / sizeof(LeafEntry) = 58
```

## Results Summary

### DEX 20M Keys

| Metric | Value |
|--------|-------|
| Bulk Load Keys | 20,000,000 |
| Tree Height | 5 |
| Leaf Nodes | 689,655 |
| Leaf Size | 673.49 MB |
| Inner Nodes | 24,626 |
| Inner Size | 24.05 MB |
| Average Latency | 4647.53 ns |
| P50 Latency | 4500 ns |
| P90 Latency | 6500 ns |
| P95 Latency | 7000 ns |
| P99 Latency | 8500 ns |
| P99.9 Latency | 11500 ns |
| Throughput | 5.895 Mops/s |

### DEX 100M Keys

| Metric | Value |
|--------|-------|
| Bulk Load Keys | 100,000,000 |
| Tree Height | 6 |
| Leaf Nodes | 3,448,275 |
| Leaf Size | 3367.46 MB |
| Inner Nodes | 123,147 |
| Inner Size | 120.26 MB |
| Average Latency | 5441.61 ns |
| P50 Latency | 5500 ns |
| P90 Latency | 7000 ns |
| P95 Latency | 7500 ns |
| P99 Latency | 9000 ns |
| P99.9 Latency | 11500 ns |
| Throughput | 5.134 Mops/s |

## Comparison Analysis

| Metric | 20M Keys | 100M Keys | Difference |
|--------|----------|-----------|------------|
| Tree Height | 5 | 6 | +1 level |
| Leaf Nodes | 689,655 | 3,448,275 | +5x |
| Total Data Size | 697.54 MB | 3487.72 MB | +5x |
| Avg Latency | 4647.53 ns | 5441.61 ns | +17.1% |
| P50 Latency | 4500 ns | 5500 ns | +22.2% |
| P99 Latency | 8500 ns | 9000 ns | +5.9% |
| Throughput | 5.895 Mops/s | 5.134 Mops/s | -12.9% |

## Key Observations

1. Tree Height Impact: Increasing from 20M to 100M keys causes the tree height to grow from 5 to 6 levels, adding one additional RDMA round-trip per lookup.

2. Latency Degradation: The additional tree level results in a 17.1% increase in average latency (from 4.65us to 5.44us), approximately 794ns per additional level.

3. Throughput Reduction: With the same 64MB cache, throughput drops by 12.9% due to increased cache misses and the extra traversal level.

4. Cache Pressure: At 100M keys, the total index size (3.49GB) is 54x larger than the 64MB cache, compared to only 10.9x at 20M keys. This results in more cache evictions and RDMA fetches.

5. Tail Latency: P99.9 latency remains similar (11.5us) for both workloads, indicating that worst-case scenarios are dominated by factors other than tree height (e.g., network congestion, cache conflicts).

## Node Configuration

The DEX B+ tree uses a fanout of approximately 58-59 entries per node:

- With 58 entries per leaf, the tree can store:
  - Height 5: ~58^4 = 11.3M entries directly addressable from root
  - Height 6: ~58^5 = 656M entries directly addressable from root

- This explains why 20M keys fit in height 5 while 100M keys require height 6.

## Cache Efficiency

With 64MB cache and 1024-byte pages:
- Cache capacity: 65,536 pages
- At 20M keys: Can cache ~9.5% of leaf nodes
- At 100M keys: Can cache ~1.9% of leaf nodes

The 5x reduction in cache coverage explains the significant performance degradation.
