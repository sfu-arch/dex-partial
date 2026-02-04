#!/bin/bash
# CHIME Range + Lookup Latency Benchmark - Node 1+ (Worker/Memory Node)
# Run this on worker nodes AFTER node 0 is started
# Wait ~5 seconds after starting node 0

set -e

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHIME_BUILD_DIR="$SCRIPT_DIR/../../CHIME/build"

# Check if benchmark exists
if [ ! -f "$CHIME_BUILD_DIR/microbench_latency" ]; then
    echo "ERROR: microbench_latency not found at $CHIME_BUILD_DIR/microbench_latency"
    echo "Please build CHIME first: cd CHIME/build && cmake .. && make -j microbench_latency"
    exit 1
fi

cd "$CHIME_BUILD_DIR"

# ============================================
# CONFIGURATION - MUST MATCH NODE 0!
# ============================================
NODE_COUNT=2          # Total nodes (compute + memory)
READ_RATIO=70         # 70% reads/lookups
INSERT_RATIO=0
UPDATE_RATIO=0
DELETE_RATIO=0
RANGE_RATIO=30        # 30% range scans
TOTAL_THREADS=16      # Total threads
MEM_THREADS=4         # Memory threads per node
CACHE_MB=256          # Cache size in MB
UNIFORM=0             # 0=Zipfian, 1=Uniform
ZIPF_THETA=0.99       # Zipfian skew
BULK_LOAD_M=10        # Bulk load (millions)
WARMUP_M=1            # Warmup ops (millions)
RUN_M=5               # Run ops (millions) - SAME AS DEX
CHECK=0               # Check correctness
TIME_BASED=0          # 0=op-based, 1=time-based
EARLY_STOP=0          # Disable early stop for latency measurement
INDEX=0               # Not used for CHIME
RPC_RATE=0.0          # Not applicable for CHIME
ADMIT_RATE=1.0        # Not applicable for CHIME
AUTO_TUNE=0           # Auto-tune disabled
MAX_THREAD=16         # Max threads per node

echo "=========================================="
echo "CHIME Range+Lookup Benchmark - Worker Node"
echo "70% Lookups + 30% Range Scans"
echo "=========================================="

echo ""
echo "Configuration (must match Node 0!):"
echo "  Nodes: $NODE_COUNT"
echo "  Threads: $TOTAL_THREADS"
echo "  Cache: ${CACHE_MB}MB"
echo "  Workload: ${READ_RATIO}% lookup + ${RANGE_RATIO}% range"
echo ""

# ============================================
# CLEANUP AND SETUP
# ============================================

# Kill any existing processes
echo ">>> Cleaning up previous processes..."
pkill -9 microbench_latency 2>/dev/null || true
sleep 1

# Setup hugepages
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
echo ">>> Running CHIME range+lookup benchmark (Worker Node)..."
echo ">>> Joining cluster..."
echo ""

sudo ./microbench_latency ${NODE_COUNT} ${READ_RATIO} ${INSERT_RATIO} ${UPDATE_RATIO} \
    ${DELETE_RATIO} ${RANGE_RATIO} ${TOTAL_THREADS} ${MEM_THREADS} \
    ${CACHE_MB} ${UNIFORM} ${ZIPF_THETA} ${BULK_LOAD_M} ${WARMUP_M} ${RUN_M} \
    ${CHECK} ${TIME_BASED} ${EARLY_STOP} ${INDEX} ${RPC_RATE} ${ADMIT_RATE} \
    ${AUTO_TUNE} ${MAX_THREAD}

echo ""
echo "=========================================="
echo "Worker node complete!"
echo "=========================================="
