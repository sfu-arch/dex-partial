#!/bin/bash
# CHIME Range + Lookup Latency Benchmark - Node 1 (COMPUTE Node)
# In CHIME: Node 0 = Memory Server, Node 1 = Compute Node
# Run this on the COMPUTE node AFTER starting node 0!
#
# Uses chime_bench.cpp which is simpler and more reliable

set -e

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHIME_BUILD_DIR="$SCRIPT_DIR/../../CHIME/build"

# Check if benchmark exists
if [ ! -f "$CHIME_BUILD_DIR/chime_bench" ]; then
    echo "ERROR: chime_bench not found at $CHIME_BUILD_DIR/chime_bench"
    echo "Please build CHIME first: cd CHIME/build && cmake .. && make -j chime_bench"
    exit 1
fi

cd "$CHIME_BUILD_DIR"

# ============================================
# CONFIGURATION - 70% Lookup + 30% Range  
# MUST MATCH NODE 0!
# ============================================
NODE_COUNT=2          # Total nodes
THREAD_COUNT=16       # Threads per node
READ_RATIO=70         # 70% lookups
ZIPF_THETA=0.99       # Zipfian skew
BULK_LOAD_M=10        # Bulk load (millions)
OPS_M=5               # Operations (millions)
RANGE_RATIO=30        # 30% range scans

echo "=========================================="
echo "CHIME Range+Lookup Benchmark - Node 1 (COMPUTE NODE)"
echo "In CHIME: Node 0 = Memory, Node 1 = Compute"
echo "70% Lookups + 30% Range Scans"
echo "500ns Latency Buckets (built into chime_bench)"
echo "=========================================="

echo ""
echo "Configuration (must match Node 0!):"
echo "  Nodes: $NODE_COUNT"
echo "  Threads: $THREAD_COUNT"
echo "  Workload: ${READ_RATIO}% lookup + ${RANGE_RATIO}% range"
echo "  Distribution: Zipfian (theta=$ZIPF_THETA)"
echo "  Bulk load: ${BULK_LOAD_M}M keys"
echo "  Operations: ${OPS_M}M"
echo ""

# ============================================
# CLEANUP AND SETUP
# ============================================

# Kill any existing processes
echo ">>> Cleaning up previous processes..."
pkill -9 chime_bench 2>/dev/null || true
pkill -9 microbench_latency 2>/dev/null || true
sudo pkill -9 chime_bench 2>/dev/null || true
sleep 1

# Setup hugepages
echo ">>> Setting up hugepages..."
echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
ulimit -l unlimited 2>/dev/null || true

# Verify hugepages
HP_FREE=$(cat /proc/meminfo | grep HugePages_Free | awk '{print $2}')
echo ">>> HugePages_Free: $HP_FREE"

# Configure memcached.conf for CHIME (memory server IP)
echo ">>> Configuring memcached.conf..."
MEMC_SERVER_IP="10.30.1.9"
MEMC_PORT="11211"
echo -e "${MEMC_SERVER_IP}\n${MEMC_PORT}" > ../memcached.conf
echo ">>> memcached.conf set to ${MEMC_SERVER_IP}:${MEMC_PORT}"

# Check memcached is reachable
echo ">>> Checking memcached at ${MEMC_SERVER_IP}:${MEMC_PORT}..."
nc -z ${MEMC_SERVER_IP} ${MEMC_PORT} && echo ">>> Memcached connection OK" || echo "WARNING: Cannot reach memcached"

# ============================================
# RUN BENCHMARK
# ============================================
echo ""
echo ">>> Running CHIME range+lookup benchmark (Worker Node)..."
echo ">>> Joining cluster..."
echo ""

# chime_bench args: <nodes> <threads> [read_ratio] [zipfian] [bulk_M] [ops_M] [range_ratio]
sudo ./chime_bench ${NODE_COUNT} ${THREAD_COUNT} ${READ_RATIO} ${ZIPF_THETA} \
    ${BULK_LOAD_M} ${OPS_M} ${RANGE_RATIO}

echo ""
echo "=========================================="
echo "Benchmark complete!"
echo "=========================================="
