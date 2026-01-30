#!/bin/bash
#
# CHIME Node 1 (Memory Node) - Simple Benchmark
# Run this FIRST before node 0
#

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
CHIME_DIR="${REPO_DIR}/CHIME"
MEMCACHED_IP="10.30.1.9"  # Compute node IP where memcached runs

NODE_COUNT=2
THREAD_COUNT=16

echo "=========================================="
echo " CHIME Node 1 (Memory) - Simple Benchmark"
echo "=========================================="

# Clean up any previous runs
echo "Cleaning up previous processes..."
sudo pkill -9 simple_bench 2>/dev/null || true
sudo pkill -9 microbench_latency 2>/dev/null || true
sudo pkill -9 ycsb_test 2>/dev/null || true
sudo pkill -9 ycsb_test_latency 2>/dev/null || true
sleep 2

# Setup hugepages
echo "Setting up hugepages..."
sudo bash -c "echo 36864 > /proc/sys/vm/nr_hugepages"
sleep 1
echo "Hugepages allocated: $(cat /proc/sys/vm/nr_hugepages)"

# Set unlimited memlock
ulimit -l unlimited 2>/dev/null || true

# Configure memcached IP
echo "Configuring memcached IP..."
if [ -f "${CHIME_DIR}/memcached.conf" ]; then
    sed -i "s/--SERVER=.*/--SERVER=${MEMCACHED_IP}/" ${CHIME_DIR}/memcached.conf
    cat ${CHIME_DIR}/memcached.conf
fi

# Build if needed
if [ ! -f "${CHIME_DIR}/build/simple_bench" ]; then
    echo "Building simple_bench..."
    cd ${CHIME_DIR}/build
    cmake ..
    make simple_bench -j$(nproc)
fi

# Run from CHIME/build directory
cd ${CHIME_DIR}/build
echo "Working directory: $(pwd)"

echo ""
echo "Running CHIME simple_bench (memory node)..."
echo "  Nodes: ${NODE_COUNT}"
echo "  Threads: ${THREAD_COUNT}"
echo ""

# Run benchmark
sudo ./simple_bench ${NODE_COUNT} ${THREAD_COUNT}

echo ""
echo "CHIME memory node complete!"
