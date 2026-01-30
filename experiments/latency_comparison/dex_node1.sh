#!/bin/bash
# DEX Latency Benchmark - Node 1+ (Worker/Memory Node)
# Run this on worker nodes AFTER node 0 is started
# Wait ~5 seconds after starting node 0

set -e

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEX_BUILD_DIR="$SCRIPT_DIR/../../dex/build"

# Check if we're in the right directory
if [ ! -f "$DEX_BUILD_DIR/newbench_latency" ]; then
    echo "ERROR: newbench_latency not found at $DEX_BUILD_DIR/newbench_latency"
    echo "Please build DEX first: cd dex/build && cmake .. && make -j newbench_latency"
    exit 1
fi

cd "$DEX_BUILD_DIR"

# ============================================
# CONFIGURATION - MUST MATCH NODE 0!
# ============================================
NODE_COUNT=2          # Total nodes (compute + memory)
READ_RATIO=100        # 100% reads
INSERT_RATIO=0
UPDATE_RATIO=0
DELETE_RATIO=0
RANGE_RATIO=0
TOTAL_THREADS=16      # Total threads across all compute nodes
MEM_THREADS=4         # Memory threads per node
CACHE_MB=256          # Cache size in MB
UNIFORM=0             # 0=Zipfian, 1=Uniform
ZIPF_THETA=0.99       # Zipfian skew
BULK_LOAD_M=10        # Bulk load (millions)
WARMUP_M=1            # Warmup ops (millions)
RUN_M=5               # Run ops (millions)
CHECK=0               # Check correctness
TIME_BASED=0          # 0=op-based, 1=time-based
EARLY_STOP=0          # Disable early stop for latency measurement
INDEX=0               # 0=DEX
RPC_RATE=1.0          # RPC rate
ADMIT_RATE=1.0        # Admission rate
AUTO_TUNE=0           # Auto-tune disabled
MAX_THREAD=16         # Max threads per node

echo "=========================================="
echo "DEX Latency Benchmark - Worker Node"
echo "100% Reads Workload"
echo "=========================================="

echo ""
echo "Configuration (must match Node 0!):"
echo "  Nodes: $NODE_COUNT"
echo "  Threads: $TOTAL_THREADS"
echo "  Cache: ${CACHE_MB}MB"
echo "  Workload: ${READ_RATIO}% read"
echo ""

# ============================================
# CLEANUP AND SETUP
# ============================================

# Kill any existing processes
echo ">>> Cleaning up previous processes..."
pkill -9 newbench_latency 2>/dev/null || true
pkill -9 newbench 2>/dev/null || true
sleep 1

# Setup hugepages (36864 = ~72GB with 2MB pages)
echo ">>> Setting up hugepages..."
echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
ulimit -l unlimited 2>/dev/null || true

# Verify hugepages
HP_FREE=$(cat /proc/meminfo | grep HugePages_Free | awk '{print $2}')
echo ">>> HugePages_Free: $HP_FREE"

# Check memcached connectivity
MEMC_IP=$(head -1 ../memcached.conf)
MEMC_PORT=$(sed -n '2p' ../memcached.conf)
echo ">>> Checking memcached at $MEMC_IP:$MEMC_PORT..."
nc -z -w 5 $MEMC_IP $MEMC_PORT || {
    echo "ERROR: Cannot connect to memcached at $MEMC_IP:$MEMC_PORT"
    echo "Make sure Node 0 is running first!"
    exit 1
}
echo ">>> Memcached connection OK"

# ============================================
# RUN BENCHMARK
# ============================================
echo ""
echo ">>> Running DEX latency benchmark (Worker Node)..."
echo ">>> Joining cluster..."
echo ""

sudo ./newbench_latency ${NODE_COUNT} ${READ_RATIO} ${INSERT_RATIO} ${UPDATE_RATIO} \
    ${DELETE_RATIO} ${RANGE_RATIO} ${TOTAL_THREADS} ${MEM_THREADS} \
    ${CACHE_MB} ${UNIFORM} ${ZIPF_THETA} ${BULK_LOAD_M} ${WARMUP_M} ${RUN_M} \
    ${CHECK} ${TIME_BASED} ${EARLY_STOP} ${INDEX} ${RPC_RATE} ${ADMIT_RATE} \
    ${AUTO_TUNE} ${MAX_THREAD}

echo ""
echo "=========================================="
echo "Worker node complete!"
echo "=========================================="
