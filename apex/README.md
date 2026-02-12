# APEX: Adaptive Prefix-Embedded eXternal Index

A novel disaggregated-memory B+tree index combining the best ideas from
[DEX](https://github.com/sfu-arch/dex-partial) and
[CHIME](https://github.com/dmemsys/CHIME).

## Architecture

APEX uses five core components, fitting in **72 MB** of compute-node memory:

| Component | Size | Tier | Purpose |
|-----------|------|------|---------|
| **CPT** (Compressed Prefix Trie) | 15 MB | L2/L3 | Key → leaf page routing |
| **ASM** (Adaptive Slot Map) | 51 MB | DRAM | Suffix → physical position mapping |
| **VE-ASM** (Value-Embedded ASM) | 5 MB | **L3 cache** | Hot-key value cache (≈80ns) |
| **VCS** (Version Chain Sync) | ~1 MB | DRAM | Read-without-lock consistency |
| **Leaf Pages** (Sorted Slot Array) | Remote | Memory Node | 4 KB pages with slot-array indirection |

### Key Innovation: Targeted 16-byte RDMA Reads

Unlike CHIME (142–1132 byte reads) or DEX (41-byte reads), APEX reads only
**16 bytes** per point lookup: the exact `{suffix, value, version}` tuple at a
known physical position, determined locally via the ASM.

## Building

```bash
cd apex
mkdir build && cd build
cmake ..
make -j$(nproc)
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `ENABLE_VEASM` | ON | Value-Embedded ASM (5MB L3 tier) |
| `ENABLE_VCS` | ON | Version Chain Sync |
| `ENABLE_BLOOM` | ON | Bloom filter in leaf pages |
| `FORCE_STD_VERBS` | OFF | Force RC transport (no MLNX_OFED) |

## Running

### Prerequisites
- Two-node RDMA cluster (memory node + compute node)
- Mellanox ConnectX-5 or later NICs
- memcached running on the memory node
- Update `memcached.conf` with your memory node's IP

### Latency Benchmark
```bash
# On memory node:
./run_experiment.sh node0

# On compute node (within 30s):
./run_experiment.sh node1
```

### YCSB Throughput Benchmark
```bash
./run_experiment.sh ycsb a 16   # YCSB-A with 16 threads
./run_experiment.sh ycsb c 16   # YCSB-C (read-only)
```

### Full Experiment Suite
```bash
./run_all_experiments.sh
```

## Project Structure

```
apex/
├── CMakeLists.txt              # Build configuration
├── run_experiment.sh           # Single-experiment runner
├── run_all_experiments.sh      # Full experiment suite
├── workloads.conf              # Workload definitions
├── memcached.conf              # Memcached IP configuration
├── APEX_README.md              # Detailed architecture document
│
├── include/
│   ├── Common.h                # APEX-specific constants & types (Key=uint64_t)
│   ├── Config.h                # DSM and cache configuration
│   ├── Key.h                   # Key conversion utilities
│   │
│   ├── LeafPage.h              # Sorted leaf page with slot-array indirection
│   ├── CompressedPrefixTrie.h  # CPT: key → leaf address routing (15 MB)
│   ├── AdaptiveSlotMap.h       # ASM: suffix → physical position (51 MB)
│   ├── ValueEmbeddedASM.h      # VE-ASM: hot-key L3 cache tier (5 MB)
│   ├── VersionChainSync.h      # VCS: read-without-lock consistency
│   ├── ApexIndex.h             # Main index class (orchestrates all components)
│   │
│   ├── DSM.h                   # Disaggregated shared memory (from CHIME)
│   ├── GlobalAddress.h         # 16-bit nodeID + 48-bit offset address
│   ├── Rdma.h                  # RDMA primitives (ibverbs)
│   ├── RdmaCompat.h            # MLNX_OFED / standard verbs compatibility
│   ├── RdmaBuffer.h            # Per-coroutine RDMA registered buffers
│   ├── RdmaCache.h             # RDMA memory region cache
│   ├── Connection.h            # Connection types
│   ├── ThreadConnection.h      # Per-thread RDMA connections
│   ├── DirectoryConnection.h   # Memory node directory connections
│   ├── DSMKeeper.h             # Memcached-based coordination
│   ├── Keeper.h                # Connection keepalive
│   ├── Directory.h             # Remote memory directory
│   ├── LocalAllocator.h        # Thread-local allocation
│   ├── GlobalAllocator.h       # Global memory allocation
│   ├── AbstractMessageConnection.h
│   ├── RawMessageConnection.h
│   ├── HugePageAlloc.h         # Huge page memory allocation
│   ├── Bitmap.h, Hash.h, Timer.h, Debug.h, WRLock.h
│   └── third_party/            # format.h, inlineskiplist.h, etc.
│
├── src/                        # DSM infrastructure (from CHIME)
│   ├── DSM.cpp, DSMKeeper.cpp, Directory.cpp, ...
│   └── rdma/                   # RDMA operations, resources, state transitions
│
├── test/
│   ├── latency_bench.cpp       # Latency benchmark (P50/P99/P99.9)
│   ├── ycsb_bench.cpp          # YCSB throughput benchmark
│   └── zipf.h                  # Zipfian distribution generator
│
└── script/
    ├── installLibs.sh          # Install dependencies
    ├── installMLNX.sh          # Install Mellanox OFED
    └── restartMemc.sh          # Restart memcached
```

## Key Differences from CHIME / DEX

| Aspect | DEX | CHIME | APEX |
|--------|-----|-------|------|
| Index type | ART (radix trie) | B+tree (hopscotch) | B+tree (sorted + slot-array) |
| Key type | `uint64_t` | `array<uint8_t,8>` | `uint64_t` |
| Read size | 41 B | 142–1132 B | **16 B** |
| Cache hot path | IndexCache (256 MB, DRAM) | Hotspot buffer (30 MB) | **VE-ASM (5 MB, L3)** |
| Consistency | Broadcast invalidation | Lease-based | Version chain sync |
| Compute memory | 256 MB | 256 MB | **72 MB** |

## License

Research prototype. See [LICENSE](../CHIME/LICENSE) for the base infrastructure license.
