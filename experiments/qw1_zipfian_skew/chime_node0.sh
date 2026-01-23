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
CLIENT_NUM=18       # Clients per compute node
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
echo ""

# Setup hugepages
echo ">>> Setting up hugepages..."
echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
ulimit -l unlimited 2>/dev/null || true

# Generate YCSB workloads if not present
LOAD_FILE="$CHIME_DIR/ycsb/workloads/load_${KEY_TYPE}_workload${WORKLOAD}"
if [ ! -f "$LOAD_FILE" ]; then
    echo ">>> Generating YCSB workloads..."
    
    # Download YCSB if needed
    if [ ! -d "$CHIME_DIR/ycsb/YCSB" ]; then
        echo ">>> Downloading YCSB..."
        cd "$CHIME_DIR/ycsb"
        curl -O --location https://github.com/brianfrankcooper/YCSB/releases/download/0.11.0/ycsb-0.11.0.tar.gz
        tar xfvz ycsb-0.11.0.tar.gz
        mv ycsb-0.11.0 YCSB
        cd "$CHIME_BUILD_DIR"
    fi
    
    # Generate workloads (small set for testing)
    cd "$CHIME_DIR/ycsb"
    bash generate_small_workloads.sh
    cd "$CHIME_BUILD_DIR"
fi

# Restart memcached
echo ">>> Restarting memcached..."
bash ../script/restartMemc.sh || {
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
sleep 2

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
