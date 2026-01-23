#!/bin/bash
#
# DEX Benchmark - Run on COMPUTE NODE 1+ (Worker Nodes)
#
# This script joins an existing DEX cluster and runs the benchmark.
# 
# IMPORTANT: Start this AFTER Node 0 is running (wait ~5 seconds)
#
# Usage: bash dex_node1.sh
#

set -e

echo "=============================================="
echo "  DEX Benchmark - COMPUTE NODE 1+ (Worker)"
echo "=============================================="

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEX_BUILD_DIR="$SCRIPT_DIR/../../dex/build"

# Check if we're in the right directory
if [ ! -f "$DEX_BUILD_DIR/newbench" ]; then
    echo "ERROR: newbench not found at $DEX_BUILD_DIR/newbench"
    echo "Please build DEX first: cd dex && mkdir build && cd build && cmake .. && make -j"
    exit 1
fi

cd "$DEX_BUILD_DIR"

# ============================================
# CONFIGURATION - MUST MATCH NODE 0!
# ============================================
NODENUM=2           # Total nodes in cluster (compute + memory)
THREADS=36          # Total threads across ALL compute nodes
MEM_THREADS=4       # Memory threads per node
CACHE_MB=256        # Cache size in MB

# Workload: YCSB-A equivalent (50% read, 50% update)
READ=50
INSERT=0
UPDATE=50
DELETE=0
RANGE=0

# Benchmark parameters
BULK_LOAD=50        # 50M keys bulk loaded
WARMUP=10           # 10M warmup ops
RUN_NUM=50          # 50M benchmark ops

# Zipfian parameters (0=Zipfian, 1=Uniform)
UNIFORM=0           # Use Zipfian distribution
ZIPF_THETA=0.99     # Skew parameter

# Other parameters
CORRECT=0           # Don't check correctness
TIMEBASE=1          # Time-based execution
EARLY=1             # Early stop enabled
INDEX=0             # 0=DEX
RPC=1
ADMIT=0.1
TUNE=0
MAX_THREAD=36       # Max threads per node

# ============================================
# EXECUTION
# ============================================

echo ""
echo "Configuration (must match Node 0!):"
echo "  Nodes: $NODENUM"
echo "  Threads: $THREADS"
echo "  Cache: ${CACHE_MB}MB"
echo "  Workload: ${READ}% read, ${UPDATE}% update"
echo "  Distribution: $([ $UNIFORM -eq 0 ] && echo "Zipfian (theta=$ZIPF_THETA)" || echo "Uniform")"
echo ""

# Setup hugepages
echo ">>> Setting up hugepages..."
echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
ulimit -l unlimited 2>/dev/null || true

# Check memcached connectivity
MEMC_IP=$(head -1 ../memcached.conf)
MEMC_PORT=$(sed -n '2p' ../memcached.conf)
echo ">>> Checking memcached at $MEMC_IP:$MEMC_PORT..."
nc -z $MEMC_IP $MEMC_PORT || {
    echo "ERROR: Cannot connect to memcached at $MEMC_IP:$MEMC_PORT"
    echo "Make sure Node 0 is running first!"
    exit 1
}
echo ">>> Memcached connection OK"

# Run benchmark (NO restartMemc - just join the cluster)
echo ""
echo ">>> Running DEX benchmark (Worker Node)..."
echo ">>> Joining cluster..."
echo ""

sudo ./newbench $NODENUM $READ $INSERT $UPDATE $DELETE $RANGE \
    $THREADS $MEM_THREADS $CACHE_MB \
    $UNIFORM $ZIPF_THETA \
    $BULK_LOAD $WARMUP $RUN_NUM \
    $CORRECT $TIMEBASE $EARLY $INDEX $RPC $ADMIT $TUNE $MAX_THREAD

echo ""
echo "=============================================="
echo "  DEX Benchmark Complete (Worker Node)"
echo "=============================================="
