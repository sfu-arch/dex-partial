#!/bin/bash
#
# CHIME Node 1 (Memory Node) with Microbenchmark
# Run this on the memory node (e.g., 10.30.1.6)
#
# This uses the new microbench_latency which doesn't depend on YCSB files
#

set -e

# Configuration - UPDATE THESE PATHS FOR YOUR SYSTEM
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
CHIME_DIR="${REPO_DIR}/CHIME"
MEMCACHED_IP="10.30.1.9"  # Compute node IP where memcached runs
EXPERIMENT_DIR="${SCRIPT_DIR}"

# Benchmark Parameters (must match compute node)
NODE_COUNT=2              # 1 compute + 1 memory
READ_RATIO=100            # 100% reads
INSERT_RATIO=0
UPDATE_RATIO=0
DELETE_RATIO=0
RANGE_RATIO=0
TOTAL_THREADS=16          # Total threads across all compute nodes
MEM_THREADS=4
CACHE_MB=256
UNIFORM=0                 # 0=Zipfian, 1=Uniform
ZIPFIAN=0.99
BULK_LOAD=10              # 10M keys to load initially
WARMUP=1                  # 1M warmup operations
OPS=5                     # 5M operations to measure
CHECK_CORRECT=0
TIME_BASED=0              # 0=operation-based (complete all ops)
EARLY_STOP=0
TREE_INDEX=0
RPC_RATE=1.0
ADMISSION_RATE=1.0
AUTO_TUNE=0
MAX_THREAD=16

echo "=========================================="
echo " CHIME Node 1 (Memory) - Microbenchmark"
echo "=========================================="

# Clean up any previous runs
echo "Cleaning up previous processes..."
sudo pkill -9 microbench_latency 2>/dev/null || true
sleep 2

# Setup hugepages (memory node needs more for data storage)
echo "Setting up hugepages..."
sudo bash -c "echo 36864 > /proc/sys/vm/nr_hugepages"
sleep 1
actual_hugepages=$(cat /proc/sys/vm/nr_hugepages)
echo "Hugepages allocated: $actual_hugepages"

# Set unlimited memlock
ulimit -l unlimited 2>/dev/null || true

# Configure memcached IP
echo "Configuring memcached IP..."
if [ -f "${CHIME_DIR}/memcached.conf" ]; then
    sed -i "s/--SERVER=.*/--SERVER=${MEMCACHED_IP}/" ${CHIME_DIR}/memcached.conf
    cat ${CHIME_DIR}/memcached.conf
else
    echo "Warning: memcached.conf not found at ${CHIME_DIR}"
fi

# Build if needed
if [ ! -f "${CHIME_DIR}/build/microbench_latency" ]; then
    echo "Building CHIME microbench_latency..."
    mkdir -p build
    cd build
    cmake ..
    make microbench_latency -j$(nproc)
fi

# Create experiment directory
mkdir -p ${EXPERIMENT_DIR}

# Change to CHIME/build directory so ../memcached.conf can be found
cd ${CHIME_DIR}/build
echo "Working directory: $(pwd)"

echo ""
echo "Running CHIME microbenchmark (memory node)..."
echo "Parameters must match compute node!"
echo ""

# Run benchmark
sudo ./microbench_latency \
    ${NODE_COUNT} \
    ${READ_RATIO} ${INSERT_RATIO} ${UPDATE_RATIO} ${DELETE_RATIO} ${RANGE_RATIO} \
    ${TOTAL_THREADS} ${MEM_THREADS} \
    ${CACHE_MB} ${UNIFORM} ${ZIPFIAN} \
    ${BULK_LOAD} ${WARMUP} ${OPS} \
    ${CHECK_CORRECT} ${TIME_BASED} ${EARLY_STOP} \
    ${TREE_INDEX} ${RPC_RATE} ${ADMISSION_RATE} \
    ${AUTO_TUNE} ${MAX_THREAD}

echo ""
echo "CHIME memory node complete!"
