#!/bin/bash
#
# CHIME Benchmark - Memory Node (10.30.1.6)
# Run this FIRST before the compute node
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(dirname "$(dirname "$SCRIPT_DIR")")"
CHIME_DIR="${REPO_DIR}/CHIME"
MEMCACHED_IP="10.30.1.9"  # Compute node where memcached runs

# Pull latest code
echo "Pulling latest code..."
cd ${REPO_DIR}
git fetch origin && git reset --hard origin/main
echo "Code updated."

# ============================================
# BENCHMARK PARAMETERS - Must match node0!
# ============================================
NODE_COUNT=2
THREAD_COUNT=1
READ_RATIO=100
ZIPFIAN=0.99
BULK_LOAD_M=1         # Must match node0
OP_NUM_M=1            # Must match node0
# ============================================

echo "=========================================="
echo " CHIME Benchmark - Memory Node"
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

# Configure memcached IP
echo "Configuring memcached IP: ${MEMCACHED_IP}"
if [ -f "${CHIME_DIR}/memcached.conf" ]; then
    sed -i "s/--SERVER=.*/--SERVER=${MEMCACHED_IP}/" ${CHIME_DIR}/memcached.conf
    cat ${CHIME_DIR}/memcached.conf
fi

# Build (always rebuild to pick up changes)
echo "Building chime_bench..."
mkdir -p ${CHIME_DIR}/build
cd ${CHIME_DIR}/build
cmake -DCMAKE_BUILD_TYPE=Release ..
make chime_bench -j$(nproc)

# Run from build directory
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
echo "Memory node complete!"
