#!/bin/bash
# DEX Latency Benchmark - Node 0 (Primary Compute Node)
# Run this on the primary compute node first
# 
# This script runs 100% reads workload and captures latency histogram

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
# CONFIGURATION
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
echo "DEX Latency Benchmark - Node 0 (Primary)"
echo "100% Reads Workload"
echo "=========================================="

echo ""
echo "Configuration:"
echo "  Nodes: $NODE_COUNT"
echo "  Threads: $TOTAL_THREADS"
echo "  Cache: ${CACHE_MB}MB"
echo "  Workload: ${READ_RATIO}% read"
echo "  Distribution: $([ $UNIFORM -eq 0 ] && echo "Zipfian (theta=$ZIPF_THETA)" || echo "Uniform")"
echo ""

# ============================================
# CLEANUP AND SETUP
# ============================================

# Kill any existing processes
echo ">>> Cleaning up previous processes..."
pkill -9 newbench_latency 2>/dev/null || true
pkill -9 newbench 2>/dev/null || true
pkill -9 memcached 2>/dev/null || true
sleep 2

# Setup hugepages (36864 = ~72GB with 2MB pages)
echo ">>> Setting up hugepages..."
echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
ulimit -l unlimited 2>/dev/null || true

# Verify hugepages
HP_FREE=$(cat /proc/meminfo | grep HugePages_Free | awk '{print $2}')
echo ">>> HugePages_Free: $HP_FREE"

# Start fresh memcached
echo ">>> Starting fresh memcached..."
memcached -u root -l 0.0.0.0 -p 11211 -c 10000 -d
sleep 2

# Initialize memcached keys
echo ">>> Initializing memcached keys..."
printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc localhost 11211
printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc localhost 11211
sleep 1

# ============================================
# RUN BENCHMARK
# ============================================
echo ""
echo ">>> Running DEX latency benchmark..."
echo ">>> Waiting for worker node to connect..."
echo ""

sudo ./newbench_latency ${NODE_COUNT} ${READ_RATIO} ${INSERT_RATIO} ${UPDATE_RATIO} \
    ${DELETE_RATIO} ${RANGE_RATIO} ${TOTAL_THREADS} ${MEM_THREADS} \
    ${CACHE_MB} ${UNIFORM} ${ZIPF_THETA} ${BULK_LOAD_M} ${WARMUP_M} ${RUN_M} \
    ${CHECK} ${TIME_BASED} ${EARLY_STOP} ${INDEX} ${RPC_RATE} ${ADMIT_RATE} \
    ${AUTO_TUNE} ${MAX_THREAD}

echo ""
echo "=========================================="
echo "Benchmark complete!"
echo "Latency data saved to: $(pwd)/dex_latency.dat"
echo "=========================================="
