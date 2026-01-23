#!/bin/bash
#
# DEX Benchmark - Run on COMPUTE NODE 0 (Primary Node)
#
# This script:
# 1. Restarts memcached for coordination
# 2. Runs the DEX benchmark (bulk loads data + runs workload)
#
# Usage: bash dex_node0.sh
#

set -e

echo "=============================================="
echo "  DEX Benchmark - COMPUTE NODE 0 (Primary)"
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
# CONFIGURATION - Modify these as needed
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
echo "Configuration:"
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

# Restart memcached (this node is the coordinator)
echo ">>> Restarting memcached..."
if [ -f "./restartMemc.sh" ]; then
    bash ./restartMemc.sh || {
        echo "WARNING: restartMemc.sh failed, trying manual start..."
        MEMC_IP=$(head -1 ../memcached.conf)
        MEMC_PORT=$(sed -n '2p' ../memcached.conf)
        pkill memcached 2>/dev/null || true
        sleep 1
        memcached -u $USER -l $MEMC_IP -p $MEMC_PORT -c 10000 -d
        sleep 1
        echo -e "set serverNum 0 0 1\r\n0\r\nquit\r" | nc $MEMC_IP $MEMC_PORT
        echo -e "set clientNum 0 0 1\r\n0\r\nquit\r" | nc $MEMC_IP $MEMC_PORT
    }
else
    echo "WARNING: restartMemc.sh not found"
fi
sleep 2

# Run benchmark
echo ""
echo ">>> Running DEX benchmark (Node 0 - Primary)..."
echo ">>> Waiting for other nodes to connect..."
echo ""

sudo ./newbench $NODENUM $READ $INSERT $UPDATE $DELETE $RANGE \
    $THREADS $MEM_THREADS $CACHE_MB \
    $UNIFORM $ZIPF_THETA \
    $BULK_LOAD $WARMUP $RUN_NUM \
    $CORRECT $TIMEBASE $EARLY $INDEX $RPC $ADMIT $TUNE $MAX_THREAD

echo ""
echo "=============================================="
echo "  DEX Benchmark Complete (Node 0)"
echo "=============================================="
