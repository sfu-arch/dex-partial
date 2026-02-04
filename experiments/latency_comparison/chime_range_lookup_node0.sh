#!/bin/bash
# CHIME Range + Lookup Latency Benchmark - Node 0 (MEMORY Server)
# In CHIME: Node 0 = Memory Server, Node 1 = Compute Node
# Run this on the MEMORY node FIRST!
#
# This script runs range scan + lookup combination workload with:
# - 500ns latency buckets
# - Same operation count as DEX

set -e

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHIME_BUILD_DIR="$SCRIPT_DIR/../../CHIME/build"

# Check if benchmark exists
if [ ! -f "$CHIME_BUILD_DIR/microbench_latency" ]; then
    echo "ERROR: microbench_latency not found at $CHIME_BUILD_DIR/microbench_latency"
    echo "Please build CHIME first: cd CHIME/build && cmake .. && make -j microbench_latency"
    exit 1
fi

cd "$CHIME_BUILD_DIR"

# ============================================
# CONFIGURATION - Range + Lookup Mix (SAME AS DEX)
# ============================================
NODE_COUNT=2          # Total nodes (compute + memory)
READ_RATIO=70         # 70% reads/lookups
INSERT_RATIO=0
UPDATE_RATIO=0
DELETE_RATIO=0
RANGE_RATIO=30        # 30% range scans
TOTAL_THREADS=1       # Single thread for simplicity
MEM_THREADS=1         # Memory threads per node
CACHE_MB=256          # Cache size in MB
UNIFORM=0             # 0=Zipfian, 1=Uniform
ZIPF_THETA=0.99       # Zipfian skew
BULK_LOAD_M=1         # Bulk load (millions) - reduced for single thread
WARMUP_M=0            # Skip warmup for faster testing
RUN_M=1               # Run ops (millions) - reduced for single thread
CHECK=0               # Check correctness
TIME_BASED=0          # 0=op-based, 1=time-based
EARLY_STOP=0          # Disable early stop for latency measurement
INDEX=0               # Not used for CHIME
RPC_RATE=0.0          # Not applicable for CHIME
ADMIT_RATE=1.0        # Not applicable for CHIME
AUTO_TUNE=0           # Auto-tune disabled
MAX_THREAD=1          # Max threads per node - single thread

echo "=========================================="
echo "CHIME Range+Lookup Benchmark - Node 0 (MEMORY SERVER)"
echo "In CHIME: Node 0 = Memory, Node 1 = Compute"
echo "70% Lookups + 30% Range Scans"
echo "500ns Latency Buckets"
echo "=========================================="

echo ""
echo "Configuration:"
echo "  Nodes: $NODE_COUNT"
echo "  Threads: $TOTAL_THREADS"
echo "  Cache: ${CACHE_MB}MB"
echo "  Workload: ${READ_RATIO}% lookup + ${RANGE_RATIO}% range"
echo "  Distribution: $([ $UNIFORM -eq 0 ] && echo "Zipfian (theta=$ZIPF_THETA)" || echo "Uniform")"
echo "  Operations: ${RUN_M}M (same as DEX)"
echo ""

# ============================================
# CLEANUP AND SETUP
# ============================================

# Kill any existing processes
echo ">>> Cleaning up previous processes..."
pkill -9 microbench_latency 2>/dev/null || true
pkill -9 ycsb_test 2>/dev/null || true
pkill -9 ycsb_test_latency 2>/dev/null || true
sudo pkill -9 microbench_latency 2>/dev/null || true
sleep 1

# Kill and restart memcached to clear all state
echo ">>> Stopping old memcached..."
pkill -9 memcached 2>/dev/null || true
sudo pkill -9 memcached 2>/dev/null || true
sleep 2

# Setup hugepages
echo ">>> Setting up hugepages..."
echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
ulimit -l unlimited 2>/dev/null || true

# Verify hugepages
HP_FREE=$(cat /proc/meminfo | grep HugePages_Free | awk '{print $2}')
echo ">>> HugePages_Free: $HP_FREE"

# Start fresh memcached
echo ">>> Starting fresh memcached..."
memcached -u root -l 0.0.0.0 -p 11211 -c 10000 -d
sleep 2

# Flush all memcached data and reinitialize keys
echo ">>> Flushing memcached and initializing keys..."
printf "flush_all\r\nquit\r\n" | nc localhost 11211
sleep 1
printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc localhost 11211
printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc localhost 11211
sleep 1

# Verify memcached state
echo ">>> Verifying memcached state..."
echo "serverNum: $(printf 'get serverNum\r\nquit\r\n' | nc localhost 11211 | grep -A1 VALUE | tail -1)"
echo "clientNum: $(printf 'get clientNum\r\nquit\r\n' | nc localhost 11211 | grep -A1 VALUE | tail -1)"

# Configure memcached.conf for CHIME (memory server IP)
echo ">>> Configuring memcached.conf..."
MEMC_SERVER_IP="10.30.1.9"
MEMC_PORT="11211"
echo -e "${MEMC_SERVER_IP}\n${MEMC_PORT}" > ../memcached.conf
echo ">>> memcached.conf set to ${MEMC_SERVER_IP}:${MEMC_PORT}"

# ============================================
# RUN BENCHMARK
# ============================================
echo ""
echo ">>> Running CHIME range+lookup benchmark..."
echo ">>> Waiting for worker node to connect..."
echo ""

sudo ./microbench_latency ${NODE_COUNT} ${READ_RATIO} ${INSERT_RATIO} ${UPDATE_RATIO} \
    ${DELETE_RATIO} ${RANGE_RATIO} ${TOTAL_THREADS} ${MEM_THREADS} \
    ${CACHE_MB} ${UNIFORM} ${ZIPF_THETA} ${BULK_LOAD_M} ${WARMUP_M} ${RUN_M} \
    ${CHECK} ${TIME_BASED} ${EARLY_STOP} ${INDEX} ${RPC_RATE} ${ADMIT_RATE} \
    ${AUTO_TUNE} ${MAX_THREAD}

echo ""
echo "=========================================="
echo "Benchmark complete!"
echo "Latency data saved to: $(pwd)/chime_latency.dat"
echo "=========================================="
