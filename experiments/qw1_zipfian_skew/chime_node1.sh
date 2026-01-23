#!/bin/bash
#
# CHIME Benchmark - Run on COMPUTE NODE 1+ (Worker Nodes)
#
# This script joins an existing CHIME cluster and runs the benchmark.
#
# IMPORTANT: 
# 1. Start this AFTER Node 0 is running (wait ~5 seconds)
# 2. Workloads must be generated/split on this node too
#
# Usage: bash chime_node1.sh
#

set -e

echo "=============================================="
echo "  CHIME Benchmark - COMPUTE NODE 1+ (Worker)"
echo "=============================================="

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHIME_DIR="$SCRIPT_DIR/../../CHIME"
CHIME_BUILD_DIR="$CHIME_DIR/build"

# Check if we're in the right directory
if [ ! -f "$CHIME_BUILD_DIR/ycsb_test" ]; then
    echo "ERROR: ycsb_test not found at $CHIME_BUILD_DIR/ycsb_test"
    echo "Please build CHIME first: cd CHIME && mkdir build && cd build && cmake .. && make -j"
    exit 1
fi

cd "$CHIME_BUILD_DIR"

# ============================================
# CONFIGURATION - MUST MATCH NODE 0!
# ============================================
CN_NUM=2            # Number of compute nodes
CLIENT_NUM=8        # Clients per compute node (should match LOADER_NUM=8 in code)
CORO_NUM=2          # Coroutines per client
KEY_TYPE="randint"  # Key type
WORKLOAD="a"        # YCSB workload (a=50/50 read/update)

# ============================================
# EXECUTION
# ============================================

# Get memcached config
MEMC_IP=$(head -1 ../memcached.conf)
MEMC_PORT=$(sed -n '2p' ../memcached.conf)

echo ""
echo "Configuration (must match Node 0!):"
echo "  Compute Nodes: $CN_NUM"
echo "  Clients per node: $CLIENT_NUM"
echo "  Coroutines: $CORO_NUM"
echo "  Workload: YCSB-$WORKLOAD"
echo "  Memcached: $MEMC_IP:$MEMC_PORT"
echo ""

# Setup hugepages
echo ">>> Setting up hugepages..."
echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
ulimit -l unlimited 2>/dev/null || true

# Generate workloads using simple generator (no YCSB dependency)
LOAD_FILE="$CHIME_DIR/ycsb/workloads/load_${KEY_TYPE}_workload${WORKLOAD}"
if [ ! -f "$LOAD_FILE" ] || [ ! -s "$LOAD_FILE" ]; then
    echo ">>> Generating workloads (using simple generator)..."
    cd "$CHIME_DIR/ycsb"
    python3 generate_workloads_simple.py small
    cd "$CHIME_BUILD_DIR"
fi

# Verify workloads have content
LOAD_LINES=$(wc -l < "$LOAD_FILE" 2>/dev/null || echo "0")
if [ "$LOAD_LINES" -lt 1000 ]; then
    echo ">>> Workload files are empty or too small ($LOAD_LINES lines), regenerating..."
    cd "$CHIME_DIR/ycsb"
    rm -rf workloads
    python3 generate_workloads_simple.py small
    cd "$CHIME_BUILD_DIR"
fi

echo ">>> Workload file has $(wc -l < "$LOAD_FILE") records"

# Check memcached connectivity (just a quick test, not fatal if it fails)
echo ">>> Memcached server: $MEMC_IP:$MEMC_PORT"
echo ">>> (Connectivity will be verified when benchmark starts)"

# Skip strict check - the benchmark itself will fail if memcached is unreachable
# The previous check was too aggressive and caused false negatives

# Split workloads for distributed execution
echo ">>> Splitting workloads for $CN_NUM nodes, $CLIENT_NUM clients each..."
python3 ../ycsb/split_workload.py ${WORKLOAD} ${KEY_TYPE} ${CN_NUM} ${CLIENT_NUM}

# Run benchmark (NO restartMemc - just join the cluster)
echo ""
echo ">>> Running CHIME benchmark (Worker Node)..."
echo ">>> Joining cluster..."
echo ""

./ycsb_test ${CN_NUM} ${CLIENT_NUM} ${CORO_NUM} ${KEY_TYPE} ${WORKLOAD}

echo ""
echo "=============================================="
echo "  CHIME Benchmark Complete (Worker Node)"
echo "=============================================="
