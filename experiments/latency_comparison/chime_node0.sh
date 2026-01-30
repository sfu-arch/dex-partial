#!/bin/bash
# CHIME Latency Benchmark - Node 0 (Primary Compute Node)
# Run this on the primary compute node first
#
# This script runs 100% reads workload (workload C) and captures latency histogram

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
# CONFIGURATION - Match DEX parameters
# ============================================
NODE_COUNT=2          # Number of nodes (must match DEX)
THREAD_COUNT=16       # Threads per node (match DEX TOTAL_THREADS)
CORO_COUNT=1          # Coroutines per thread (1 for fair latency comparison)
KEY_TYPE="randint"    # Key type
WORKLOAD="c"          # Workload C = 100% reads (matches DEX READ_RATIO=100)

echo "=========================================="
echo "CHIME Latency Benchmark - Node 0 (Primary)"
echo "100% Reads Workload (Workload C)"
echo "=========================================="

echo ""
echo "Configuration:"
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
pkill -9 memcached 2>/dev/null || true
sleep 2

# Setup hugepages
echo ">>> Setting up hugepages..."
echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
ulimit -l unlimited 2>/dev/null || true

# Verify hugepages
HP_FREE=$(cat /proc/meminfo | grep HugePages_Free | awk '{print $2}')
echo ">>> HugePages_Free: $HP_FREE"

# Generate workloads if needed
echo ">>> Checking workload files..."
WORKLOAD_DIR=$(cat ../workloads.conf 2>/dev/null || echo "$CHIME_YCSB_DIR/workloads/")
if [ ! -f "${WORKLOAD_DIR}load_${KEY_TYPE}_workload${WORKLOAD}0" ]; then
    echo ">>> Generating workload files..."
    cd "$CHIME_YCSB_DIR"
    python3 generate_workloads_simple.py small
    cd "$CHIME_BUILD_DIR"
fi

# Ensure workloads.conf exists
echo ">>> Setting workloads.conf..."
echo "$CHIME_YCSB_DIR/workloads/" > ../workloads.conf

# Split workloads for nodes
echo ">>> Splitting workloads for $NODE_COUNT nodes..."
cd "$CHIME_YCSB_DIR"
python3 split_workload.py $WORKLOAD $KEY_TYPE $NODE_COUNT $THREAD_COUNT 2>/dev/null || true
cd "$CHIME_BUILD_DIR"

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
echo ">>> Running CHIME latency benchmark..."
echo ">>> Waiting for worker node to connect..."
echo ""

sudo ./ycsb_test_latency ${NODE_COUNT} ${THREAD_COUNT} ${CORO_COUNT} ${KEY_TYPE} ${WORKLOAD}

echo ""
echo "=========================================="
echo "Benchmark complete!"
echo "Latency data saved to: $(pwd)/chime_latency.dat"
echo "=========================================="
