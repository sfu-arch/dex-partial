# DEX vs CHIME: Disaggregated Memory B+-Tree Comparison

This repository contains modified versions of **DEX** and **CHIME** for comparative evaluation of disaggregated memory index structures.

---

## 📋 Table of Contents

1. [Overview](#overview)
2. [Architecture Comparison](#architecture-comparison)
3. [Changes Made](#changes-made)
4. [System Requirements](#system-requirements)
5. [Build Instructions](#build-instructions)
6. [Running Experiments](#running-experiments)
7. [Configuration Reference](#configuration-reference)
8. [Troubleshooting](#troubleshooting)

---

## 🔍 Overview

### What is Disaggregated Memory?

```
┌─────────────────────────────────────────────────────────────────────┐
│                    DISAGGREGATED MEMORY ARCHITECTURE                │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│   ┌─────────────┐         RDMA Network         ┌─────────────┐     │
│   │   Compute   │◄──────────────────────────►  │   Memory    │     │
│   │    Node     │      (InfiniBand/RoCE)       │    Node     │     │
│   │             │                              │             │     │
│   │  ┌───────┐  │                              │  ┌───────┐  │     │
│   │  │ CPU   │  │   One-sided RDMA Read/Write  │  │ B+Tree│  │     │
│   │  │ Cache │  │◄────────────────────────────►│  │ Data  │  │     │
│   │  └───────┘  │      (No remote CPU)         │  └───────┘  │     │
│   └─────────────┘                              └─────────────┘     │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### DEX (Scalable Range Indexing)
- **Paper**: "DEX: Scalable Range Indexing on Disaggregated Memory"
- **Key Innovation**: Path-aware RadixCache with unswizzling
- **Cache Strategy**: Prefix-based caching

### CHIME (Disaggregated B+-Tree)
- **Paper**: "CHIME: A Cache-Efficient and High-Performance Hybrid Index on Disaggregated Memory"
- **Key Innovation**: Hopscotch hashing leaves, bitmap locks, speculative reads
- **Cache Strategy**: Range-based TreeCache

---

## 🏗️ Architecture Comparison

```
┌─────────────────────────────────────────────────────────────────────┐
│                         DEX ARCHITECTURE                            │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│   Compute Node                         Memory Node                  │
│   ┌─────────────────────┐              ┌─────────────────────┐     │
│   │                     │              │                     │     │
│   │  ┌───────────────┐  │    RDMA      │  ┌───────────────┐  │     │
│   │  │  RadixCache   │  │◄────────────►│  │   B+-Tree     │  │     │
│   │  │  (Prefix-     │  │              │  │   (Standard   │  │     │
│   │  │   based)      │  │              │  │    Leaves)    │  │     │
│   │  └───────────────┘  │              │  └───────────────┘  │     │
│   │         │           │              │                     │     │
│   │  ┌──────▼────────┐  │              │                     │     │
│   │  │  Unswizzling  │  │              │                     │     │
│   │  │  (Pointer     │  │              │                     │     │
│   │  │   Translation)│  │              │                     │     │
│   │  └───────────────┘  │              │                     │     │
│   │                     │              │                     │     │
│   └─────────────────────┘              └─────────────────────┘     │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│                        CHIME ARCHITECTURE                           │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│   Compute Node                         Memory Node                  │
│   ┌─────────────────────┐              ┌─────────────────────┐     │
│   │                     │              │                     │     │
│   │  ┌───────────────┐  │    RDMA      │  ┌───────────────┐  │     │
│   │  │  TreeCache    │  │◄────────────►│  │   B+-Tree     │  │     │
│   │  │  (Range-      │  │              │  │   (Hopscotch  │  │     │
│   │  │   based)      │  │              │  │    Leaves)    │  │     │
│   │  └───────────────┘  │              │  └───────────────┘  │     │
│   │         │           │              │         │           │     │
│   │  ┌──────▼────────┐  │              │  ┌──────▼────────┐  │     │
│   │  │  IdxCache     │  │              │  │  Bitmap Locks │  │     │
│   │  │  (Speculative │  │              │  │  (Vacancy-    │  │     │
│   │  │   Read)       │  │              │  │   aware)      │  │     │
│   │  └───────────────┘  │              │  └───────────────┘  │     │
│   │         │           │              │                     │     │
│   │  ┌──────▼────────┐  │              │                     │     │
│   │  │Write Combining│  │              │                     │     │
│   │  │Read Delegation│  │              │                     │     │
│   │  └───────────────┘  │              │                     │     │
│   │                     │              │                     │     │
│   └─────────────────────┘              └─────────────────────┘     │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### Feature Comparison Table

| Feature | DEX | CHIME |
|---------|-----|-------|
| **Cache Type** | RadixCache (prefix-based) | TreeCache (range-based) |
| **Leaf Structure** | Standard B+-tree | Hopscotch hashing |
| **Lock Mechanism** | Standard RDMA locks | Bitmap with vacancy awareness |
| **Pointer Handling** | Unswizzling | Direct addressing |
| **Speculative Read** | ❌ | ✅ (IdxCache) |
| **Write Combining** | ❌ | ✅ |
| **Read Delegation** | ❌ | ✅ |
| **RDMA Transport** | RC (Reliable Connection) | DC/RC (Dynamic/Reliable) |

---

## 🔧 Changes Made

### 1. RDMA Compatibility Layer (CHIME)

**Problem**: CHIME requires MLNX_OFED experimental verbs (`ibv_exp_*`) which are not available on all systems.

**Solution**: Created `CHIME/include/RdmaCompat.h` - a compatibility layer that:

```
┌─────────────────────────────────────────────────────────────────────┐
│                    RDMA COMPATIBILITY LAYER                         │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│   ┌─────────────────┐                    ┌─────────────────┐       │
│   │  Application    │                    │  Application    │       │
│   │  (CHIME Code)   │                    │  (CHIME Code)   │       │
│   └────────┬────────┘                    └────────┬────────┘       │
│            │                                      │                 │
│            ▼                                      ▼                 │
│   ┌─────────────────┐                    ┌─────────────────┐       │
│   │ ibv_exp_* calls │                    │ ibv_exp_* calls │       │
│   │ (experimental)  │                    │ (experimental)  │       │
│   └────────┬────────┘                    └────────┬────────┘       │
│            │                                      │                 │
│            ▼                                      ▼                 │
│   ┌─────────────────┐                    ┌─────────────────┐       │
│   │   MLNX_OFED     │                    │  RdmaCompat.h   │       │
│   │   (Required)    │                    │  (Wrapper)      │       │
│   └────────┬────────┘                    └────────┬────────┘       │
│            │                                      │                 │
│            ▼                                      ▼                 │
│   ┌─────────────────┐                    ┌─────────────────┐       │
│   │  DC Transport   │                    │  RC Transport   │       │
│   │  Device Memory  │                    │  Host Memory    │       │
│   │  Masked Atomics │                    │  Std Atomics    │       │
│   └─────────────────┘                    └─────────────────┘       │
│                                                                     │
│        WITH MLNX_OFED                      WITHOUT MLNX_OFED       │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

**Files Changed**:
- `CHIME/include/RdmaCompat.h` - NEW: Compatibility wrapper (500+ lines)
- `CHIME/include/Rdma.h` - Added include for RdmaCompat.h
- `CHIME/CMakeLists.txt` - Auto-detection of RDMA driver
- `CHIME/src/rdma/StateTrans.cpp` - Wrapped DC-specific code with `#if USE_DC_TRANSPORT`

### 2. Memory Configuration Reduction

**Problem**: Servers have limited memlock (8MB - 48GB) and kernel keys limits (200).

**Solution**: Reduced memory footprint for both systems.

```
┌─────────────────────────────────────────────────────────────────────┐
│                    MEMORY CONFIGURATION                             │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│   ORIGINAL CONFIG:                      REDUCED CONFIG:             │
│                                                                     │
│   DEX:                                  DEX:                        │
│   ├── dsmSize: 64 GB                    ├── dsmSize: 8 GB           │
│   └── rdmaBufferSize: 2 GB              └── rdmaBufferSize: 1 GB    │
│   Total: 66 GB                          Total: 9 GB                 │
│                                                                     │
│   CHIME:                                CHIME:                      │
│   ├── dsmSize: 64 GB                    ├── dsmSize: 8 GB           │
│   └── rdmaBufferSize: 4 GB              └── rdmaBufferSize: 1 GB    │
│   Total: 68 GB                          Total: 9 GB                 │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

**Files Changed**:
- `CHIME/include/Common.h`:
  - `dsmSize`: 64 → 8 GB
  - `rdmaBufferSize`: 4 → 1 GB
  - `MAX_APP_THREAD`: 65 → 17 (to stay under kernel keys limit)
- `dex/include/Common.h`:
  - `dsmSize`: 64 → 8 GB
  - `rdmaBufferSize`: 2 → 1 GB

### 3. Huge Page Fallback

**Problem**: Huge pages may not be available or properly configured.

**Solution**: Added fallback to regular pages with detailed error reporting.

```
┌─────────────────────────────────────────────────────────────────────┐
│                    MEMORY ALLOCATION FLOW                           │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│                    hugePageAlloc(size)                              │
│                           │                                         │
│                           ▼                                         │
│                  ┌────────────────┐                                │
│                  │ Try MAP_HUGETLB │                                │
│                  │ (2MB pages)     │                                │
│                  └────────┬───────┘                                │
│                           │                                         │
│              ┌────────────┴────────────┐                           │
│              │                         │                            │
│         SUCCESS                    FAILED                          │
│              │                         │                            │
│              ▼                         ▼                            │
│    ┌─────────────────┐      ┌─────────────────┐                    │
│    │ Return huge     │      │ Try regular     │                    │
│    │ page memory     │      │ pages + POPULATE│                    │
│    └─────────────────┘      └────────┬────────┘                    │
│                                      │                              │
│                         ┌────────────┴────────────┐                │
│                         │                         │                 │
│                    SUCCESS                    FAILED               │
│                         │                         │                 │
│                         ▼                         ▼                 │
│              ┌─────────────────┐      ┌─────────────────┐          │
│              │ Return regular  │      │ Print error     │          │
│              │ page memory     │      │ Return nullptr  │          │
│              └─────────────────┘      └─────────────────┘          │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

**Files Changed**:
- `CHIME/include/HugePageAlloc.h` - Added fallback + diagnostics
- `dex/include/HugePageAlloc.h` - Added fallback + diagnostics

### 4. Enhanced Error Reporting

**Problem**: RDMA memory registration failures were silent.

**Solution**: Added detailed error messages with errno interpretation.

**Files Changed**:
- `CHIME/src/rdma/Resource.cpp` - Added errno reporting for ibv_reg_mr failures

---

## 💻 System Requirements

### Hardware
- InfiniBand or RoCE capable NICs (Mellanox ConnectX-5 recommended)
- Minimum 16GB RAM per node (32GB+ recommended)

### Software
- Linux Kernel 4.x+ (tested with 6.3.2)
- RDMA drivers (rdma-core or MLNX_OFED)
- GCC 9+ with C++17 support
- CMake 3.10+
- memcached (for node coordination)
- Libraries: boost, cityhash, libnuma, libibverbs

### System Limits

Check your limits before running:

```bash
# Memory lock limit (need at least 10GB = 10485760 KB)
ulimit -l

# Kernel keys limit (need at least 200)
cat /proc/sys/kernel/keys/maxkeys

# Huge pages (optional but recommended)
cat /proc/meminfo | grep HugePages_Free
```

**Minimum Requirements**:
| Resource | Minimum | Recommended |
|----------|---------|-------------|
| memlock | 10 GB | unlimited |
| maxkeys | 200 | 10000 |
| Huge Pages | 0 (fallback available) | 5000 pages (10GB) |

---

## 🔨 Build Instructions

### Clone Repository

```bash
git clone https://github.com/sfu-arch/dex-partial.git
cd dex-partial
```

### Build CHIME

```bash
cd CHIME
mkdir -p build && cd build

# Basic build (with RDMA compatibility layer)
cmake -DENABLE_CACHE=on ..

# Optional: Enable CHIME-specific features
cmake -DENABLE_CACHE=on \
      -DHOPSCOTCH_LEAF_NODE=on \
      -DSPECULATIVE_READ=on \
      -DVACANCY_AWARE_LOCK=on ..

make -j8
```

**CMake Options for CHIME**:
| Option | Description | Default |
|--------|-------------|---------|
| `ENABLE_CACHE` | Enable TreeCache | OFF |
| `HOPSCOTCH_LEAF_NODE` | Use Hopscotch hashing in leaves | OFF |
| `SPECULATIVE_READ` | Enable speculative point queries | OFF |
| `VACANCY_AWARE_LOCK` | Enable bitmap locks | OFF |
| `FORCE_STD_VERBS` | Force standard RDMA verbs (no MLNX_OFED) | AUTO |

### Build DEX

```bash
cd dex
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j8
```

---

## 🚀 Running Experiments

### Prerequisites

1. **Start memcached** (on one node):
```bash
memcached -u $USER -l 0.0.0.0 -p 11211 -c 10000 -d
```

2. **Configure memcached.conf** (on all nodes):
```bash
# Create config file pointing to memcached server
echo -e "MEMCACHED_IP\n11211" > memcached.conf
# Example for localhost:
echo -e "127.0.0.1\n11211" > ~/dex-partial/CHIME/memcached.conf
echo -e "127.0.0.1\n11211" > ~/dex-partial/dex/memcached.conf
```

### Single-Node Testing

```bash
# CHIME single-node test
cd ~/dex-partial/CHIME/build
./ycsb_test 1 8 8 randint a
# Args: kNodeCount kThreadCount kCoroCnt keyType workload

# DEX single-node test
cd ~/dex-partial/dex/build
./newbench 1 100 0 0 0 0 8 4 256 0 0.99 50 10 50 0 1 1 0 1 0.1 0 8
```

### Multi-Node Testing (2 nodes)

**Node Assignment**:
```
┌─────────────────────────────────────────────────────────────────────┐
│                      2-NODE DEPLOYMENT                              │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│   Node 0 (Memory Node)              Node 1 (Compute Node)          │
│   ┌─────────────────────┐          ┌─────────────────────┐         │
│   │ - Stores B+-tree    │          │ - Runs client       │         │
│   │ - Starts FIRST      │◄────────►│   threads           │         │
│   │ - Also runs compute │   RDMA   │ - Starts SECOND     │         │
│   │   threads           │          │                     │         │
│   └─────────────────────┘          └─────────────────────┘         │
│                                                                     │
│   # Start first:                    # Start second:                │
│   ./ycsb_test 2 8 8 randint a      ./ycsb_test 2 8 8 randint a    │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

**On Memory Node (start FIRST)**:
```bash
cd ~/dex-partial/CHIME/build
./ycsb_test 2 8 8 randint a
```

**On Compute Node (start SECOND)**:
```bash
cd ~/dex-partial/CHIME/build
./ycsb_test 2 8 8 randint a
```

### YCSB Workloads

| Workload | Read % | Update % | Insert % | Scan % | Description |
|----------|--------|----------|----------|--------|-------------|
| a | 50 | 50 | 0 | 0 | Update heavy |
| b | 95 | 5 | 0 | 0 | Read mostly |
| c | 100 | 0 | 0 | 0 | Read only |
| d | 95 | 0 | 5 | 0 | Read latest |
| e | 0 | 0 | 5 | 95 | Short ranges |

---

## ⚙️ Configuration Reference

### CHIME Configuration (`CHIME/include/Common.h`)

```cpp
// Memory Configuration
constexpr uint64_t dsmSize = 8;           // GB - Shared memory pool
constexpr uint64_t rdmaBufferSize = 1;    // GB - RDMA buffer per node

// Thread Configuration
#define MAX_APP_THREAD 17                  // Max application threads
#define MAX_CORO_NUM 8                     // Coroutines per thread

// Node Configuration
#define MEMORY_NODE_NUM 1                  // Number of memory nodes
#define MAX_MACHINE 20                     // Max cluster size
```

### DEX Configuration (`dex/include/Common.h`)

```cpp
// Memory Configuration
constexpr uint64_t dsmSize = 8;           // GB - Shared memory pool
constexpr uint64_t rdmaBufferSize = 1;    // GB - RDMA buffer per node

// Thread Configuration
#define MAX_APP_THREAD 36                  // Max application threads
#define MAX_CORO_NUM 8                     // Coroutines per thread
```

### RDMA Compatibility (`CHIME/include/RdmaCompat.h`)

```cpp
// Auto-detected at compile time:
#define USE_DC_TRANSPORT 0/1    // Dynamic Connected transport
#define USE_DEVICE_MEMORY 0/1   // On-chip memory
#define USE_MASKED_ATOMICS 0/1  // Extended atomic operations
```

---

## 🔧 Troubleshooting

### Error: "Memory registration failed: errno=12"

**Cause**: memlock limit too low

**Solution**:
```bash
# Check current limit
ulimit -l

# Ask admin to increase (in /etc/security/limits.conf):
# username soft memlock unlimited
# username hard memlock unlimited
```

### Error: "Cannot allocate memory" with huge pages

**Cause**: Insufficient huge pages

**Solution**:
```bash
# Check available huge pages
cat /proc/meminfo | grep HugePages_Free

# Allocate more (requires sudo)
sudo sh -c 'echo 5000 > /proc/sys/vm/nr_hugepages'
```

### Error: "Kernel keys limit exceeded"

**Cause**: Too many threads registering RDMA memory

**Solution**: Reduce `MAX_APP_THREAD` or ask admin to increase limits:
```bash
# Check limit
cat /proc/sys/kernel/keys/maxkeys

# Increase (requires sudo)
sudo sysctl -w kernel.keys.maxkeys=10000
```

### Error: "SERVER HAS FAILED AND IS DISABLED"

**Cause**: memcached not running

**Solution**:
```bash
# Start memcached
memcached -u $USER -l 0.0.0.0 -p 11211 -c 10000 -d

# Verify
ps aux | grep memcached
```

### Build Error: "ibv_exp_* not declared"

**Cause**: MLNX_OFED not installed

**Solution**: The RdmaCompat.h layer should handle this automatically. If still failing:
```bash
# Force standard verbs
cmake -DFORCE_STD_VERBS=ON ..
```

---

## 📁 Repository Structure

```
dex-partial/
├── README.md                    # This file
├── CHIME/
│   ├── include/
│   │   ├── RdmaCompat.h        # NEW: RDMA compatibility layer
│   │   ├── Common.h            # MODIFIED: Reduced memory config
│   │   ├── HugePageAlloc.h     # MODIFIED: Fallback to regular pages
│   │   └── ...
│   ├── src/
│   │   └── rdma/
│   │       ├── Resource.cpp    # MODIFIED: Better error messages
│   │       └── StateTrans.cpp  # MODIFIED: DC transport guards
│   ├── test/
│   │   └── ycsb_test.cpp       # Main benchmark
│   └── CMakeLists.txt          # MODIFIED: RDMA detection
│
├── dex/
│   ├── include/
│   │   ├── Common.h            # MODIFIED: Reduced memory config
│   │   ├── HugePageAlloc.h     # MODIFIED: Fallback to regular pages
│   │   └── ...
│   └── test/
│       └── newbench.cpp        # Main benchmark
│
└── experiments/
    └── qw1_zipfian_skew/
        ├── run_experiment.py   # NEW: Unified experiment runner
        └── README.md           # Experiment documentation
```

---

## 📊 Expected Output

### Successful CHIME Run
```
kNodeCount 1, kThreadCount 8, kCoroCnt 8
ycsb_load: ../ycsb/workloads/load_randint_workloada
ycsb_trans: ../ycsb/workloads/txn_randint_workloada
Memlock limit: soft=48003 MB, hard=48003 MB, requested=8192 MB
Using NUMA node 1 (max available: 1)
Huge pages allocated at 0x7xxx, size=8192 MB
shared memory size: 8GB, 0x7xxx
rdma cache size: 1GB
Machine NR: 1
Memory registration succeeded: lkey=xxx, rkey=xxx
...
[Throughput results]
```

### Successful DEX Run
```
Compute node count = 1
kNodeCount 1, kReadRatio 100, ...
shared memory size: 8GB, 0x7xxx
cache size: 1GB
Machine NR: 1
...
[Throughput results]
```

---

## 📚 References

1. **DEX Paper**: "DEX: Scalable Range Indexing on Disaggregated Memory" (VLDB 2024)
2. **CHIME Paper**: "CHIME: A Cache-Efficient and High-Performance Hybrid Index on Disaggregated Memory"
3. **RDMA Programming**: [rdma-core documentation](https://github.com/linux-rdma/rdma-core)

---

## 👤 Author

Modified by: apa222  
Repository: https://github.com/sfu-arch/dex-partial

---

*Last updated: January 22, 2026*
