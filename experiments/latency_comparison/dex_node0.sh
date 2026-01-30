#!/bin/bash
# DEX Latency Benchmark - Node 0 (Primary)
# Run this on the primary compute node first
# 
# This script runs 100% reads workload and captures latency histogram

set -e

# Parameters
NODE_COUNT=2          # Total nodes (compute + memory)
READ_RATIO=100        # 100% reads
INSERT_RATIO=0
UPDATE_RATIO=0
DELETE_RATIO=0
RANGE_RATIO=0
TOTAL_THREADS=16      # Total threads across all compute nodes
MEM_THREADS=4         # Memory threads per node
CACHE_MB=256          # Cache size in MB
UNIFORM=0             # 0=Zipfian, 1=Uniform
ZIPF_THETA=0.99       # Zipfian skew
BULK_LOAD_M=10        # Bulk load (millions)
WARMUP_M=1            # Warmup ops (millions)
RUN_M=5               # Run ops (millions)
CHECK=0               # Check correctness
TIME_BASED=0          # 0=op-based, 1=time-based
EARLY_STOP=0          # Disable early stop for latency measurement
INDEX=0               # 0=DEX
RPC_RATE=1.0          # RPC rate
ADMIT_RATE=1.0        # Admission rate
AUTO_TUNE=0           # Auto-tune disabled
MAX_THREAD=16         # Max threads per node

echo "=========================================="
echo "DEX Latency Benchmark - Node 0"
echo "100% Reads Workload"
echo "=========================================="

# Start memcached
echo "Starting memcached..."
memcached -u root -l 0.0.0.0 -p 11211 -c 10000 -d

sleep 2

# Run the benchmark
echo "Running DEX latency benchmark..."
./newbench_latency ${NODE_COUNT} ${READ_RATIO} ${INSERT_RATIO} ${UPDATE_RATIO} \
    ${DELETE_RATIO} ${RANGE_RATIO} ${TOTAL_THREADS} ${MEM_THREADS} \
    ${CACHE_MB} ${UNIFORM} ${ZIPF_THETA} ${BULK_LOAD_M} ${WARMUP_M} ${RUN_M} \
    ${CHECK} ${TIME_BASED} ${EARLY_STOP} ${INDEX} ${RPC_RATE} ${ADMIT_RATE} \
    ${AUTO_TUNE} ${MAX_THREAD}

echo "=========================================="
echo "Benchmark complete!"
echo "Latency data saved to: dex_latency.dat"
echo "=========================================="
