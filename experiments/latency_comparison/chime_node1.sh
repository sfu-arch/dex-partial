#!/bin/bash
# CHIME Latency Benchmark - Node 1+ (Worker/Memory Node)
# Run this on worker nodes AFTER node 0 is started
# Wait ~5 seconds after starting node 0

set -e

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHIME_BUILD_DIR="$SCRIPT_DIR/../../CHIME/build"
CHIME_YCSB_DIR="$SCRIPT_DIR/../../CHIME/ycsb"

# Check if benchmark exists
if [ ! -f "$CHIME_BUILD_DIR/ycsb_test_latency" ]; then
    echo "ERROR: ycsb_test_latency not found at $CHIME_BUILD_DIR/ycsb_test_latency"
    echo "Please build CHIME first: cd CHIME/build && cmake .. && make -j ycsb_test_latency"
    exit 1
fi

cd "$CHIME_BUILD_DIR"

# ============================================
# CONFIGURATION - MUST MATCH NODE 0!
# ============================================
NODE_COUNT=2          # Number of nodes (must match DEX)
THREAD_COUNT=16       # Threads per node (match DEX TOTAL_THREADS)
CORO_COUNT=1          # Coroutines per thread (1 for fair latency comparison)
KEY_TYPE="randint"    # Key type
WORKLOAD="c"          # Workload C = 100% reads (matches DEX READ_RATIO=100)

echo "=========================================="
echo "CHIME Latency Benchmark - Worker Node"
echo "100% Reads Workload (Workload C)"
echo "=========================================="

echo ""
echo "Configuration (must match Node 0!):"
echo "  Nodes: $NODE_COUNT"
echo "  Threads: $THREAD_COUNT"
echo "  Coroutines: $CORO_COUNT"
echo "  Workload: $WORKLOAD (100% reads)"
echo ""

# ============================================
# CLEANUP AND SETUP
# ============================================

# Kill any existing processes
echo ">>> Cleaning up previous processes..."
pkill -9 ycsb_test_latency 2>/dev/null || true
pkill -9 ycsb_test 2>/dev/null || true
sleep 1

# Setup hugepages
echo ">>> Setting up hugepages..."
echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
ulimit -l unlimited 2>/dev/null || true

# Verify hugepages
HP_FREE=$(cat /proc/meminfo | grep HugePages_Free | awk '{print $2}')
echo ">>> HugePages_Free: $HP_FREE"

# Ensure workloads.conf exists
echo ">>> Setting workloads.conf..."
echo "$CHIME_YCSB_DIR/workloads/" > ../workloads.conf

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
echo ">>> Running CHIME latency benchmark (Worker Node)..."
echo ">>> Joining cluster..."
echo ""

sudo ./ycsb_test_latency ${NODE_COUNT} ${THREAD_COUNT} ${CORO_COUNT} ${KEY_TYPE} ${WORKLOAD}

echo ""
echo "=========================================="
echo "Worker node complete!"
echo "=========================================="
