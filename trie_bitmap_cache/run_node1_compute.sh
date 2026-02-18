#!/bin/bash
#############################################################################
# TBC Compute Node Script (Node 1)
#
# Run this AFTER starting the memory node (within 30 seconds).
#
# This script:
#   1. Kills local processes (does NOT touch memcached)
#   2. Builds TBC
#   3. Runs the benchmark
#
# USAGE: ./run_node1_compute.sh [cache_mb] [threads] [zipfian_theta]
#   Examples:
#     ./run_node1_compute.sh               # Use defaults
#     ./run_node1_compute.sh 512           # 512MB cache
#     ./run_node1_compute.sh 256 32 0.99   # 256MB, 32 threads, zipf=0.99
#############################################################################

set -e

# ========================== CONFIGURATION ==========================
# Memory node IP (the OTHER machine running memcached)
MEMORY_NODE_IP="10.30.1.9"
MEMCACHED_PORT="11211"

# Paths
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TBC_DIR="$SCRIPT_DIR"
DEX_DIR="${TBC_DIR}/../dex"

# Parameters (can be overridden by command line)
CACHE_MB=${1:-256}
THREAD_COUNT=${2:-16}
ZIPFIAN_THETA=${3:-0.99}

# Fixed parameters
NODE_COUNT=2
READ_RATIO=100
INSERT_RATIO=0
UPDATE_RATIO=0
DELETE_RATIO=0
RANGE_RATIO=0
UNIFORM_WORKLOAD=0
BULK_LOAD_M=10
WARMUP_M=2
OPS_M=20
TIME_BASED=1
EARLY_STOP=1
MAX_THREADS=32


# ========================== FUNCTIONS ==========================

log() {
    echo -e "\033[1;32m[Node1 $(date '+%H:%M:%S')]\033[0m $1"
}

err() {
    echo -e "\033[1;31m[Node1 ERROR]\033[0m $1" >&2
}

kill_processes() {
    log "Killing local processes..."
    sudo pkill -9 tbc_bench 2>/dev/null || true
    sudo pkill -9 newbench 2>/dev/null || true
    sleep 1
}

setup_hugepages() {
    log "Configuring hugepages..."
    echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
    ulimit -l unlimited 2>/dev/null || true
}

update_config() {
    cat > "$DEX_DIR/memcached.conf" << EOF
$MEMORY_NODE_IP
$MEMCACHED_PORT
EOF
    log "Updated memcached.conf → $MEMORY_NODE_IP:$MEMCACHED_PORT"
}

verify_memcached() {
    log "Verifying memcached connection..."
    local reply
    reply=$(printf "get serverNum\r\nquit\r\n" | nc -w 5 "$MEMORY_NODE_IP" "$MEMCACHED_PORT" 2>/dev/null | head -1)
    if [[ "$reply" == *"VALUE"* ]]; then
        log "✓ Connected to memcached on $MEMORY_NODE_IP"
    else
        err "Cannot connect to memcached on $MEMORY_NODE_IP:$MEMCACHED_PORT"
        err "Make sure memory node is running first!"
        exit 1
    fi
}

build_tbc() {
    log "Building TBC..."
    cd "$TBC_DIR"
    mkdir -p build
    cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -3
    make -j$(nproc) tbc_bench 2>&1 | tail -5
    
    if [ ! -f "tbc_bench" ]; then
        err "Build failed!"
        exit 1
    fi
    log "✓ Build complete"
}


# ========================== MAIN ==========================

echo ""
echo "╔══════════════════════════════════════════════════════════╗"
echo "║    TBC Compute Node (Node 1) - Starting...               ║"
echo "╠══════════════════════════════════════════════════════════╣"
echo "║  Memory Node:  $MEMORY_NODE_IP:$MEMCACHED_PORT"
echo "║  Cache:        ${CACHE_MB}MB"
echo "║  Threads:      $THREAD_COUNT"
echo "║  Zipfian:      $ZIPFIAN_THETA"
echo "║  Workload:     R/I/U/D/S = $READ_RATIO/$INSERT_RATIO/$UPDATE_RATIO/$DELETE_RATIO/$RANGE_RATIO"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""

kill_processes
setup_hugepages
update_config
verify_memcached
build_tbc

cd "$TBC_DIR/build"

echo ""
log "━━━ Starting TBC Benchmark ━━━"
echo ""

./tbc_bench \
    $NODE_COUNT \
    $READ_RATIO $INSERT_RATIO $UPDATE_RATIO $DELETE_RATIO $RANGE_RATIO \
    $THREAD_COUNT 1 \
    $CACHE_MB \
    $UNIFORM_WORKLOAD $ZIPFIAN_THETA \
    $BULK_LOAD_M $WARMUP_M $OPS_M \
    0 $TIME_BASED $EARLY_STOP \
    0 \
    0 1 \
    0 $MAX_THREADS

echo ""
log "━━━ Benchmark Complete ━━━"
