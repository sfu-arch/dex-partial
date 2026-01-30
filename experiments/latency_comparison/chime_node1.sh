#!/bin/bash
# CHIME Latency Benchmark - Node 1+ (Worker)
# Run this on worker compute nodes AFTER node 0 is started
# Wait ~5 seconds after starting node 0

set -e

# Parameters - MUST MATCH node 0
NODE_COUNT=2          # Number of nodes
THREAD_COUNT=16       # Threads per node  
CORO_COUNT=2          # Coroutines per thread
KEY_TYPE="randint"    # Key type
WORKLOAD="c"          # Workload C = 100% reads

echo "=========================================="
echo "CHIME Latency Benchmark - Worker Node"
echo "100% Reads Workload (Workload C)"
echo "=========================================="

# Run the benchmark
echo "Running CHIME latency benchmark (worker)..."
./ycsb_test_latency ${NODE_COUNT} ${THREAD_COUNT} ${CORO_COUNT} ${KEY_TYPE} ${WORKLOAD}

echo "=========================================="
echo "Worker node complete!"
echo "=========================================="
