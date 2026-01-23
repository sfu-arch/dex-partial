#!/bin/bash
#
# CHIME Benchmark - Run on COMPUTE NODE 0 (Primary Node)
#
# This script:
# 1. Generates YCSB workloads if needed
# 2. Restarts memcached for coordination
# 3. Splits workloads across nodes
# 4. Runs the CHIME benchmark
#
# Usage: bash chime_node0.sh
#

set -e

echo "=============================================="
echo "  CHIME Benchmark - COMPUTE NODE 0 (Primary)"
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
# CONFIGURATION - Modify these as needed
# ============================================
CN_NUM=2            # Number of compute nodes
CLIENT_NUM=8        # Clients per compute node (should match LOADER_NUM=8 in code)
CORO_NUM=2          # Coroutines per client
KEY_TYPE="randint"  # Key type
WORKLOAD="a"        # YCSB workload (a=50/50 read/update)

# ============================================
# EXECUTION
# ============================================

echo ""
echo "Configuration:"
echo "  Compute Nodes: $CN_NUM"
echo "  Clients per node: $CLIENT_NUM"
echo "  Coroutines: $CORO_NUM"
echo "  Workload: YCSB-$WORKLOAD"

# Setup hugepages
echo ">>> Setting up hugepages..."
echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
ulimit -l unlimited 2>/dev/null || true

# Get memcached config
MEMC_IP=$(head -1 ../memcached.conf)
MEMC_PORT=$(sed -n '2p' ../memcached.conf)

echo "  Memcached: $MEMC_IP:$MEMC_PORT"
echo ""

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

# Restart memcached with fresh state
echo ">>> Restarting memcached on $MEMC_IP:$MEMC_PORT..."
sudo pkill memcached 2>/dev/null || true
sleep 1
memcached -l 0.0.0.0 -p $MEMC_PORT -c 10000 -d
sleep 1

# Initialize memcached counters using printf (echo -e doesn't work reliably with nc)
echo ">>> Initializing memcached counters..."
printf "set serverNum 0 0 1\r\n0\r\n" | nc -q1 localhost $MEMC_PORT
printf "set clientNum 0 0 1\r\n0\r\n" | nc -q1 localhost $MEMC_PORT

# Verify counters are set
echo ">>> Verifying memcached..."
VERIFY=$(printf "get serverNum\r\n" | nc -q1 localhost $MEMC_PORT | head -1)
if [[ "$VERIFY" != *"VALUE"* ]]; then
    echo "ERROR: Failed to initialize memcached counters"
    echo "Got: $VERIFY"
    exit 1
fi
echo ">>> Memcached initialized OK"
sleep 1

# Split workloads for distributed execution
echo ">>> Splitting workloads for $CN_NUM nodes, $CLIENT_NUM clients each..."
python3 ../ycsb/split_workload.py ${WORKLOAD} ${KEY_TYPE} ${CN_NUM} ${CLIENT_NUM}

# Run benchmark
echo ""
echo ">>> Running CHIME benchmark (Node 0 - Primary)..."
echo ">>> Waiting for other nodes to connect..."
echo ""

./ycsb_test ${CN_NUM} ${CLIENT_NUM} ${CORO_NUM} ${KEY_TYPE} ${WORKLOAD}

echo ""
echo "=============================================="
echo "  CHIME Benchmark Complete (Node 0)"
echo "=============================================="
