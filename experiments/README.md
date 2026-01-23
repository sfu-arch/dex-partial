# DEX vs CHIME Benchmark Experiments

## Architecture Overview

Both DEX and CHIME use a **Disaggregated Memory (DM)** architecture:

```
┌─────────────────────────────────────────────────────────────────────────┐
│                    DISAGGREGATED MEMORY CLUSTER                          │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                          │
│   COMPUTE NODES (run benchmark clients)                                  │
│   ─────────────────────────────────────                                  │
│   ┌─────────────┐    ┌─────────────┐                                    │
│   │  Node 0     │    │  Node 1     │    ... (more compute nodes)        │
│   │  (Primary)  │    │  (Worker)   │                                    │
│   │             │    │             │                                    │
│   │ - Starts    │    │ - Joins     │                                    │
│   │   memcached │    │   cluster   │                                    │
│   │ - Bulk load │    │ - Run bench │                                    │
│   │ - Run bench │    │             │                                    │
│   └──────┬──────┘    └──────┬──────┘                                    │
│          │                  │                                            │
│          └────────┬─────────┘                                            │
│                   │ RDMA Network                                         │
│          ┌────────┴─────────┐                                            │
│          │                  │                                            │
│   ┌──────┴──────┐    ┌──────┴──────┐                                    │
│   │  Memory     │    │  Memory     │    ... (more memory nodes)         │
│   │  Node 0     │    │  Node 1     │                                    │
│   │             │    │             │                                    │
│   │ - Stores    │    │ - Stores    │                                    │
│   │   B+ tree   │    │   B+ tree   │                                    │
│   │   data      │    │   data      │                                    │
│   └─────────────┘    └─────────────┘                                    │
│                                                                          │
│   MEMORY NODES (provide remote memory via RDMA)                          │
│   ─────────────────────────────────────────────                          │
│                                                                          │
└─────────────────────────────────────────────────────────────────────────┘
```

## Important: Node Roles

| Node Type | DEX Role | CHIME Role |
|-----------|----------|------------|
| **Compute Node 0** | Runs `run.sh` (starts memcached, bulk loads, benchmarks) | Runs `restartMemc.sh` + `ycsb_test` |
| **Compute Node 1+** | Runs `run_other.sh` (joins cluster, benchmarks) | Runs `ycsb_test` |
| **Memory Nodes** | Part of DSM cluster (automatic) | Part of DSM cluster (automatic) |

**Note:** In DEX/CHIME, memory nodes are implicitly managed. The `nodenum` parameter specifies total nodes (compute + memory). Memory threads run on each node.

---

## Quick Start

### Prerequisites on ALL nodes:

```bash
# Install RDMA packages
sudo apt update
sudo apt install -y rdma-core libibverbs-dev librdmacm-dev ibverbs-utils

# Install other dependencies
sudo apt install -y memcached libmemcached-dev libcityhash-dev \
    libboost-all-dev libtbb-dev libnuma-dev cmake g++

# Setup hugepages (required on ALL nodes)
echo 36864 | sudo tee /proc/sys/vm/nr_hugepages
sudo ulimit -l unlimited
```

### Step 1: Clone on ALL nodes

```bash
git clone https://github.com/sfu-arch/dex-partial.git
cd dex-partial
```

### Step 2: Configure memcached IP

Edit `dex/memcached.conf` and `CHIME/memcached.conf` on ALL nodes:
```
<IP_OF_COMPUTE_NODE_0>
11211
```

### Step 3: Build on ALL nodes

```bash
# Build DEX
cd dex
./script/hugepage.sh
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j
cp ../script/*.sh .
cd ../..

# Build CHIME
cd CHIME
mkdir -p build && cd build
cmake ..
make -j
cp ../script/*.sh .
cd ../..
```

### Step 4: Run Experiments

See individual experiment directories for specific instructions.

---

## Experiment: qw1_zipfian_skew

This experiment compares DEX and CHIME under different Zipfian skew parameters.

### Running DEX

**On Compute Node 0:**
```bash
cd dex-partial/dex/build
bash ../../experiments/qw1_zipfian_skew/dex_node0.sh
```

**On Compute Node 1+ (wait 5 seconds after Node 0):**
```bash
cd dex-partial/dex/build
bash ../../experiments/qw1_zipfian_skew/dex_node1.sh
```

### Running CHIME

**On Compute Node 0:**
```bash
cd dex-partial/CHIME/build
bash ../../experiments/qw1_zipfian_skew/chime_node0.sh
```

**On Compute Node 1+ (wait 5 seconds after Node 0):**
```bash
cd dex-partial/CHIME/build
bash ../../experiments/qw1_zipfian_skew/chime_node1.sh
```

---

## Parameter Reference

### DEX newbench Parameters

```
./newbench <nodenum> <read%> <insert%> <update%> <delete%> <range%> \
           <threads> <mem_threads> <cache_mb> <uniform> <zipf_theta> \
           <bulk_M> <warmup_M> <run_M> <correct> <timebase> <early> \
           <index> <rpc> <admit> <tune> <max_thread>
```

| Parameter | Description | Example |
|-----------|-------------|---------|
| nodenum | Total nodes in cluster | 2 |
| read% | Read operation percentage | 50 |
| threads | Total threads across all compute nodes | 36 |
| mem_threads | Memory threads per node | 4 |
| cache_mb | Cache size in MB | 256 |
| uniform | 0=Zipfian, 1=Uniform | 0 |
| zipf_theta | Zipfian skew (0.0-0.99) | 0.99 |
| index | 0=DEX | 0 |

### CHIME ycsb_test Parameters

```
./ycsb_test <CN_num> <client_num> <coro_num> <key_type> <workload>
```

| Parameter | Description | Example |
|-----------|-------------|---------|
| CN_num | Number of compute nodes | 2 |
| client_num | Clients per compute node | 18 |
| coro_num | Coroutines per client | 2 |
| key_type | Key type | randint |
| workload | YCSB workload | a |
