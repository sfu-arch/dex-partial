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
if [ ! -f "${CHIME_DIR}/build/microbench_latency" ]; then
    echo "Building microbench_latency..."
    cd ${CHIME_DIR}/build
    cmake ..
    make microbench_latency -j$(nproc)
fi

# Run from CHIME/build directory
cd ${CHIME_DIR}/build
echo "Working directory: $(pwd)"

# ============================================
# DEX-style benchmark parameters (same as node0)
# ============================================
READ_RATIO=100
INSERT_RATIO=0
UPDATE_RATIO=0
DELETE_RATIO=0
RANGE_RATIO=0
MEM_THREAD_COUNT=4
CACHE_SIZE_MB=256
UNIFORM_WORKLOAD=0
ZIPFIAN_THETA=0.99
BULK_LOAD_M=10
WARMUP_M=1
OP_NUM_M=5
CHECK_CORRECT=0
TIME_BASED=0
EARLY_STOP=0
TREE_INDEX=0
RPC_RATE=1.0
ADMISSION_RATE=1.0
AUTO_TUNE=0

echo ""
echo "Running CHIME microbench_latency (memory node)..."
echo "  Nodes: ${NODE_COUNT}"
echo "  Threads: ${THREAD_COUNT}"
echo ""

# Run benchmark
sudo ./microbench_latency ${NODE_COUNT} ${READ_RATIO} ${INSERT_RATIO} ${UPDATE_RATIO} \
    ${DELETE_RATIO} ${RANGE_RATIO} ${THREAD_COUNT} ${MEM_THREAD_COUNT} \
    ${CACHE_SIZE_MB} ${UNIFORM_WORKLOAD} ${ZIPFIAN_THETA} ${BULK_LOAD_M} \
    ${WARMUP_M} ${OP_NUM_M} ${CHECK_CORRECT} ${TIME_BASED} ${EARLY_STOP} \
    ${TREE_INDEX} ${RPC_RATE} ${ADMISSION_RATE} ${AUTO_TUNE} ${THREAD_COUNT}

echo ""
echo "CHIME memory node complete!"
