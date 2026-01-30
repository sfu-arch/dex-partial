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
sleep 2

# Setup hugepages
echo "Setting up hugepages..."
sudo bash -c "echo 36864 > /proc/sys/vm/nr_hugepages"
sleep 1
echo "Hugepages allocated: $(cat /proc/sys/vm/nr_hugepages)"

# Set unlimited memlock
ulimit -l unlimited 2>/dev/null || true

# Initialize memcached keys
echo "Initializing memcached keys..."
(printf "set serverNum 0 0 1\r\n0\r\n"; printf "set clientNum 0 0 1\r\n0\r\n"; sleep 0.5) | nc -q1 127.0.0.1 11211 2>/dev/null || \
(printf "set serverNum 0 0 1\r\n0\r\n"; printf "set clientNum 0 0 1\r\n0\r\n"; sleep 0.5) | telnet 127.0.0.1 11211 2>/dev/null || \
echo "Warning: Could not initialize memcached keys"

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
echo "Running CHIME simple_bench (compute node)..."
echo "  Nodes: ${NODE_COUNT}"
echo "  Threads: ${THREAD_COUNT}"
echo ""

# Run benchmark
sudo ./simple_bench ${NODE_COUNT} ${THREAD_COUNT}

echo ""
echo "CHIME benchmark complete!"
echo "Results in: $(pwd)/chime_latency.dat"
