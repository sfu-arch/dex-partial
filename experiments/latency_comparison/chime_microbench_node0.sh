#!/bin/bash
#
# CHIME Node 0 (Compute Node) - Simple Benchmark
# Run this AFTER starting node 1 (memory node)
#

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
CHIME_DIR="${REPO_DIR}/CHIME"

NODE_COUNT=2
THREAD_COUNT=16

echo "=========================================="
echo " CHIME Node 0 (Compute) - Simple Benchmark"
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

# Flush memcached to clear stale data from previous runs
echo "Flushing memcached..."
echo "flush_all" | nc -q1 127.0.0.1 11211 2>/dev/null || \
echo "flush_all" | nc 127.0.0.1 11211 2>/dev/null || \
echo "Warning: Could not flush memcached"
sleep 1

# Initialize memcached keys
echo "Initializing memcached keys..."
(printf "set serverNum 0 0 1\r\n0\r\n"; printf "set clientNum 0 0 1\r\n0\r\n"; sleep 0.5) | nc -q1 127.0.0.1 11211 2>/dev/null || \
(printf "set serverNum 0 0 1\r\n0\r\n"; printf "set clientNum 0 0 1\r\n0\r\n"; sleep 0.5) | nc 127.0.0.1 11211 2>/dev/null || \
echo "Warning: Could not initialize memcached keys"

# Verify memcached keys
echo "Verifying memcached keys..."
echo "get serverNum" | nc -q1 127.0.0.1 11211 2>/dev/null || true
echo "get clientNum" | nc -q1 127.0.0.1 11211 2>/dev/null || true

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
# DEX-style benchmark parameters
# ============================================
# Usage: ./microbench_latency kNodeCount kReadRatio kInsertRatio kUpdateRatio 
#        kDeleteRatio kRangeRatio totalThreadCount memThreadCount
#        cacheSize(MB) uniform_workload zipfian_theta bulk_load_num(M)
#        warmup_num(M) op_num(M) check_correctness time_based early_stop
#        index rpc_rate admission_rate auto_tune kMaxThread

READ_RATIO=100        # 100% reads
INSERT_RATIO=0
UPDATE_RATIO=0
DELETE_RATIO=0
RANGE_RATIO=0
MEM_THREAD_COUNT=4    # Memory threads
CACHE_SIZE_MB=256     # Cache size in MB
UNIFORM_WORKLOAD=0    # 0=Zipfian, 1=Uniform
ZIPFIAN_THETA=0.99    # Zipfian skew
BULK_LOAD_M=10        # 10M keys bulk loaded
WARMUP_M=1            # 1M warmup ops
OP_NUM_M=5            # 5M benchmark ops
CHECK_CORRECT=0
TIME_BASED=0
EARLY_STOP=0
TREE_INDEX=0
RPC_RATE=1.0
ADMISSION_RATE=1.0
AUTO_TUNE=0

echo ""
echo "Running CHIME microbench_latency (compute node)..."
echo "  Nodes: ${NODE_COUNT}"
echo "  Threads: ${THREAD_COUNT}"
echo "  Read/Insert/Update/Delete/Range: ${READ_RATIO}/${INSERT_RATIO}/${UPDATE_RATIO}/${DELETE_RATIO}/${RANGE_RATIO}"
echo "  Zipfian theta: ${ZIPFIAN_THETA} (uniform=${UNIFORM_WORKLOAD})"
echo "  Bulk load: ${BULK_LOAD_M}M, Warmup: ${WARMUP_M}M, Ops: ${OP_NUM_M}M"
echo ""

# Run benchmark
sudo ./microbench_latency ${NODE_COUNT} ${READ_RATIO} ${INSERT_RATIO} ${UPDATE_RATIO} \
    ${DELETE_RATIO} ${RANGE_RATIO} ${THREAD_COUNT} ${MEM_THREAD_COUNT} \
    ${CACHE_SIZE_MB} ${UNIFORM_WORKLOAD} ${ZIPFIAN_THETA} ${BULK_LOAD_M} \
    ${WARMUP_M} ${OP_NUM_M} ${CHECK_CORRECT} ${TIME_BASED} ${EARLY_STOP} \
    ${TREE_INDEX} ${RPC_RATE} ${ADMISSION_RATE} ${AUTO_TUNE} ${THREAD_COUNT}

echo ""
echo "CHIME benchmark complete!"
echo "Results in: $(pwd)/chime_latency.dat"
