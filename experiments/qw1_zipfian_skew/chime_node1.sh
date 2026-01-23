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
CLIENT_NUM=18       # Clients per compute node
CORO_NUM=2          # Coroutines per client
KEY_TYPE="randint"  # Key type
WORKLOAD="a"        # YCSB workload (a=50/50 read/update)

# ============================================
# EXECUTION
# ============================================

echo ""
echo "Configuration (must match Node 0!):"
echo "  Compute Nodes: $CN_NUM"
echo "  Clients per node: $CLIENT_NUM"
echo "  Coroutines: $CORO_NUM"
echo "  Workload: YCSB-$WORKLOAD"
echo ""

# Setup hugepages
echo ">>> Setting up hugepages..."
echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
ulimit -l unlimited 2>/dev/null || true

# Generate YCSB workloads if not present (each node needs local copy)
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
        
        # Fix Python 2 to Python 3 compatibility in YCSB bin script
        echo ">>> Patching YCSB for Python 3..."
        sed -i 's/except subprocess.CalledProcessError, err:/except subprocess.CalledProcessError as err:/' YCSB/bin/ycsb
        sed -i '1s|#!/usr/bin/env python|#!/usr/bin/env python3|' YCSB/bin/ycsb
        
        cd "$CHIME_BUILD_DIR"
    fi
    
    # Generate workloads (small set for testing)
    cd "$CHIME_DIR/ycsb"
    bash generate_small_workloads.sh
    cd "$CHIME_BUILD_DIR"
fi

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
