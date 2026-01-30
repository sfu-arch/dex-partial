#!/bin/bash
#
# CHIME Benchmark - Compute Node (10.30.1.9)
# Run this AFTER starting the memory node
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
CHIME_DIR="${REPO_DIR}/CHIME"

# ============================================
# BENCHMARK PARAMETERS - Edit these!
# ============================================
NODE_COUNT=2
THREAD_COUNT=16
READ_RATIO=100        # 100 = 100% reads
ZIPFIAN=0.99          # Zipfian skew (0.99 = highly skewed)
BULK_LOAD_M=10        # Bulk load 10M keys
OP_NUM_M=5            # Run 5M operations
# ============================================

echo "=========================================="
echo " CHIME Benchmark - Compute Node"
echo "=========================================="

# Clean up
echo "Cleaning up..."
sudo pkill -9 chime_bench 2>/dev/null || true
sudo pkill -9 simple_bench 2>/dev/null || true
sudo pkill -9 microbench_latency 2>/dev/null || true
sleep 2

# Hugepages
echo "Setting up hugepages..."
sudo bash -c "echo 36864 > /proc/sys/vm/nr_hugepages"
echo "Hugepages: $(cat /proc/sys/vm/nr_hugepages)"

ulimit -l unlimited 2>/dev/null || true

# Build
if [ ! -f "${CHIME_DIR}/build/chime_bench" ]; then
    echo "Building chime_bench..."
    mkdir -p ${CHIME_DIR}/build
    cd ${CHIME_DIR}/build
    cmake ..
    make chime_bench -j$(nproc)
fi

# Run from build directory (so it finds ../memcached.conf)
cd ${CHIME_DIR}/build

echo ""
echo "Parameters:"
echo "  Nodes: ${NODE_COUNT}"
echo "  Threads: ${THREAD_COUNT}"
echo "  Read ratio: ${READ_RATIO}%"
echo "  Zipfian: ${ZIPFIAN}"
echo "  Bulk load: ${BULK_LOAD_M}M keys"
echo "  Operations: ${OP_NUM_M}M"
echo ""

sudo ./chime_bench ${NODE_COUNT} ${THREAD_COUNT} ${READ_RATIO} ${ZIPFIAN} ${BULK_LOAD_M} ${OP_NUM_M}

echo ""
echo "Done! Latency saved to: ${CHIME_DIR}/build/chime_latency.dat"
