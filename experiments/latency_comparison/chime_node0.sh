#!/bin/bash
# CHIME Latency Benchmark - Node 0 (Primary)
# Run this on the primary compute node first
#
# This script runs 100% reads workload (workload C) and captures latency histogram

set -e

# Parameters
NODE_COUNT=2          # Number of nodes
THREAD_COUNT=16       # Threads per node  
CORO_COUNT=2          # Coroutines per thread
KEY_TYPE="randint"    # Key type
WORKLOAD="c"          # Workload C = 100% reads

echo "=========================================="
echo "CHIME Latency Benchmark - Node 0"
echo "100% Reads Workload (Workload C)"
echo "=========================================="

# Start memcached
echo "Starting memcached..."
memcached -u root -l 0.0.0.0 -p 11211 -c 10000 -d

sleep 2

# Run the benchmark
echo "Running CHIME latency benchmark..."
./ycsb_test_latency ${NODE_COUNT} ${THREAD_COUNT} ${CORO_COUNT} ${KEY_TYPE} ${WORKLOAD}

echo "=========================================="
echo "Benchmark complete!"
echo "Latency data saved to: chime_latency.dat"
echo "=========================================="
