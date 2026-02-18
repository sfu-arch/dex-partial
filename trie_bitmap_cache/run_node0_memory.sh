#!/bin/bash
#############################################################################
# TBC Memory Node Script (Node 0)
#
# Run this FIRST on the memory node before starting the compute node.
#
# This script:
#   1. Kills all previous processes
#   2. Flushes and resets memcached completely
#   3. Builds TBC
#   4. Starts the memory server
#
# USAGE: ./run_node0_memory.sh [cache_mb] [threads]
#   Examples:
#     ./run_node0_memory.sh           # Use defaults
#     ./run_node0_memory.sh 512       # 512MB cache
#     ./run_node0_memory.sh 256 32    # 256MB cache, 32 threads
#############################################################################

set -e

# ========================== CONFIGURATION ==========================
# Memory node IP (THIS machine's IP)
MEMORY_NODE_IP="10.30.1.9"
MEMCACHED_PORT="11211"

# Paths
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TBC_DIR="$SCRIPT_DIR"
DEX_DIR="${TBC_DIR}/../dex"

# Parameters (can be overridden by command line)
CACHE_MB=${1:-256}
THREAD_COUNT=${2:-16}

# Fixed parameters
NODE_COUNT=2
READ_RATIO=100
INSERT_RATIO=0
UPDATE_RATIO=0
DELETE_RATIO=0
RANGE_RATIO=0
UNIFORM_WORKLOAD=0
ZIPFIAN_THETA=0.99
BULK_LOAD_M=10
WARMUP_M=2
OPS_M=20
TIME_BASED=1
EARLY_STOP=1
MAX_THREADS=32


# ========================== FUNCTIONS ==========================

log() {
    echo -e "\033[1;36m[Node0 $(date '+%H:%M:%S')]\033[0m $1"
}

err() {
    echo -e "\033[1;31m[Node0 ERROR]\033[0m $1" >&2
}

# Complete memcached reset
reset_memcached() {
    log "━━━ Resetting memcached ━━━"
    
    # Kill existing
    log "Killing existing memcached..."
    sudo pkill -9 memcached 2>/dev/null || true
    sleep 2
    
    # Start fresh
    log "Starting memcached on 0.0.0.0:$MEMCACHED_PORT..."
    sudo memcached -u root -l 0.0.0.0 -p "$MEMCACHED_PORT" -c 10000 -d
    sleep 2
    
    # Flush all
    log "Flushing all keys..."
    printf "flush_all\r\nquit\r\n" | nc -w 2 "$MEMORY_NODE_IP" "$MEMCACHED_PORT" 2>/dev/null || true
    sleep 1
    
    # Initialize coordination keys
    log "Initializing serverNum=0, clientNum=0..."
    printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMORY_NODE_IP" "$MEMCACHED_PORT"
    printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMORY_NODE_IP" "$MEMCACHED_PORT"
    sleep 1
    
    # Verify
    local reply
    reply=$(printf "get serverNum\r\nquit\r\n" | nc -w 2 "$MEMORY_NODE_IP" "$MEMCACHED_PORT" 2>/dev/null | head -1)
    if [[ "$reply" == *"VALUE"* ]]; then
        log "✓ memcached verified"
    else
        err "memcached verification failed!"
        exit 1
    fi
}

kill_processes() {
    log "Killing stale processes..."
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
echo "║    TBC Memory Node (Node 0) - Starting...                ║"
echo "╠══════════════════════════════════════════════════════════╣"
echo "║  Memory IP:    $MEMORY_NODE_IP:$MEMCACHED_PORT"
echo "║  Cache:        ${CACHE_MB}MB"
echo "║  Threads:      $THREAD_COUNT"
echo "╚══════════════════════════════════════════════════════════╝"
echo ""

kill_processes
reset_memcached
setup_hugepages
update_config
build_tbc

cd "$TBC_DIR/build"

echo ""
log "━━━ Starting TBC Memory Server ━━━"
log "Waiting for compute node to connect..."
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
