#!/bin/bash
#############################################################################
# TBC (Trie+Bitmap Cache) Starter Scripts
#
# Cluster Configuration:
#   - Node 0: Memory Node - hosts memcached + memory server
#   - Node 1: Compute Node - runs TBC benchmark
#
# USAGE:
#   On Memory Node: ./run_tbc.sh node0
#   On Compute Node: ./run_tbc.sh node1
#   Reset only:      ./run_tbc.sh reset
#
# Order of execution:
#   1. Run on Node 0 first (starts memcached + memory server)
#   2. Run on Node 1 within 30 seconds (connects to memory node)
#############################################################################

set -e

# ========================== CONFIGURATION ==========================
# Edit these to match your cluster setup

# Memory node IP (where memcached runs)
MEMORY_NODE_IP="10.30.1.9"
MEMCACHED_PORT="11211"

# Path to TBC directory (same on both nodes, or edit per node)
TBC_DIR="$(cd "$(dirname "$0")" && pwd)"
DEX_DIR="${TBC_DIR}/../dex"

# Benchmark parameters
NODE_COUNT=2            # 1 memory + 1 compute
THREAD_COUNT=16         # Threads per compute node
CACHE_MB=256            # Cache size in MB
READ_RATIO=100          # % point reads
INSERT_RATIO=0          # % inserts
UPDATE_RATIO=0          # % updates
DELETE_RATIO=0          # % deletes
RANGE_RATIO=0           # % range scans
UNIFORM_WORKLOAD=0      # 0=zipfian, 1=uniform
ZIPFIAN_THETA=0.99      # Zipfian skew
BULK_LOAD_M=10          # Millions of keys to bulk load
WARMUP_M=2              # Millions of warmup ops
OPS_M=20                # Millions of benchmark ops
RANGE_SIZE=100          # Range scan size
TIME_BASED=1            # 1=time-based, 0=op-count based
EARLY_STOP=1            # Stop when first thread finishes
MAX_THREADS=32          # Max threads per node


# ========================== HELPER FUNCTIONS ==========================

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1"
}

# Kill all related processes
kill_processes() {
    log "Killing any running TBC/DEX processes..."
    sudo pkill -9 tbc_bench 2>/dev/null || true
    sudo pkill -9 newbench 2>/dev/null || true
    sudo pkill -9 newbench_latency 2>/dev/null || true
    sleep 1
}

# Complete memcached reset
flush_and_reset_memcached() {
    log "=== Resetting memcached ==="
    
    # Kill existing memcached
    log "Killing existing memcached..."
    sudo pkill -9 memcached 2>/dev/null || true
    sleep 2
    
    # Start fresh memcached
    log "Starting fresh memcached on 0.0.0.0:${MEMCACHED_PORT}..."
    sudo memcached -u root -l 0.0.0.0 -p "$MEMCACHED_PORT" -c 10000 -d
    sleep 2
    
    # Flush all keys (safety net)
    log "Flushing all keys..."
    printf "flush_all\r\nquit\r\n" | nc -w 2 "$MEMORY_NODE_IP" "$MEMCACHED_PORT" 2>/dev/null || true
    sleep 1
    
    # Initialize coordination keys (DEX/CHIME/TBC all use these)
    log "Setting serverNum=0, clientNum=0..."
    printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMORY_NODE_IP" "$MEMCACHED_PORT" || true
    printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMORY_NODE_IP" "$MEMCACHED_PORT" || true
    sleep 1
    
    # Verify memcached is working
    local reply
    reply=$(printf "get serverNum\r\nquit\r\n" | nc -w 2 "$MEMORY_NODE_IP" "$MEMCACHED_PORT" 2>/dev/null | head -1)
    if [[ "$reply" == *"VALUE"* ]]; then
        log "OK: memcached verified - serverNum key present"
    else
        log "ERROR: memcached verification failed (reply: $reply)"
        log "Retrying..."
        sudo pkill -9 memcached 2>/dev/null || true
        sleep 2
        sudo memcached -u root -l 0.0.0.0 -p "$MEMCACHED_PORT" -c 10000 -d
        sleep 3
        printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMORY_NODE_IP" "$MEMCACHED_PORT" || true
        printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMORY_NODE_IP" "$MEMCACHED_PORT" || true
    fi
    
    log "=== memcached reset complete ==="
}

# Setup hugepages for RDMA
setup_hugepages() {
    log "Configuring hugepages..."
    echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
    ulimit -l unlimited 2>/dev/null || true
    log "Hugepages: $(cat /proc/sys/vm/nr_hugepages)"
}

# Build TBC
build_tbc() {
    log "Building TBC..."
    cd "$TBC_DIR"
    
    mkdir -p build
    cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release
    make -j$(nproc) tbc_bench
    
    if [ -f "tbc_bench" ]; then
        log "Build successful: tbc_bench ready"
    else
        log "ERROR: tbc_bench not found!"
        ls -la
        exit 1
    fi
}

# Update memcached.conf
update_memcached_conf() {
    log "Updating memcached.conf to point to $MEMORY_NODE_IP:$MEMCACHED_PORT"
    
    cat > "$TBC_DIR/../dex/memcached.conf" << EOF
$MEMORY_NODE_IP
$MEMCACHED_PORT
EOF
}


# ========================== NODE EXECUTION ==========================

run_memory_node() {
    log "=========================================="
    log "  TBC: Memory Node (Node 0)"
    log "=========================================="
    
    kill_processes
    flush_and_reset_memcached
    setup_hugepages
    update_memcached_conf
    build_tbc
    
    cd "$TBC_DIR/build"
    
    log ""
    log "memcached.conf:"
    cat "$DEX_DIR/memcached.conf"
    log ""
    
    log "Starting TBC memory server..."
    log "Waiting for compute node to connect (within 30s)..."
    log ""
    
    # TBC uses same 23-argument format as DEX
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
}

run_compute_node() {
    log "=========================================="
    log "  TBC: Compute Node (Node 1)"
    log "=========================================="
    
    kill_processes
    # Don't reset memcached on compute node - memory node handles that
    setup_hugepages
    update_memcached_conf
    build_tbc
    
    cd "$TBC_DIR/build"
    
    log ""
    log "memcached.conf:"
    cat "$DEX_DIR/memcached.conf"
    log ""
    
    log "Starting TBC benchmark..."
    log "Parameters:"
    log "  Nodes:         $NODE_COUNT"
    log "  Threads:       $THREAD_COUNT"
    log "  Cache:         ${CACHE_MB}MB"
    log "  Workload:      R/I/U/D/S = $READ_RATIO/$INSERT_RATIO/$UPDATE_RATIO/$DELETE_RATIO/$RANGE_RATIO"
    log "  Zipfian theta: $ZIPFIAN_THETA"
    log "  Bulk load:     ${BULK_LOAD_M}M keys"
    log "  Operations:    ${OPS_M}M"
    log ""
    
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
    
    log ""
    log "=== TBC benchmark complete ==="
}


# ========================== MAIN ENTRY ==========================

case "${1:-help}" in
    node0|memory)
        run_memory_node
        ;;
    node1|compute)
        run_compute_node
        ;;
    reset|clean|flush)
        kill_processes
        flush_and_reset_memcached
        log "Ready for fresh start"
        ;;
    build)
        build_tbc
        ;;
    *)
        echo ""
        echo "TBC (Trie+Bitmap Cache) Experiment Runner"
        echo ""
        echo "Usage: $0 <command>"
        echo ""
        echo "Commands:"
        echo "  node0 | memory   Run on memory node (starts memcached + server)"
        echo "  node1 | compute  Run on compute node (benchmark client)"
        echo "  reset | clean    Kill all processes, flush & reset memcached"
        echo "  build            Just build TBC without running"
        echo ""
        echo "Execution order:"
        echo "  1. On memory node:  $0 node0"
        echo "  2. On compute node: $0 node1   (within 30 seconds)"
        echo ""
        echo "Configuration (edit in script):"
        echo "  MEMORY_NODE_IP = $MEMORY_NODE_IP"
        echo "  MEMCACHED_PORT = $MEMCACHED_PORT"
        echo "  NODE_COUNT     = $NODE_COUNT"
        echo "  THREAD_COUNT   = $THREAD_COUNT"
        echo "  CACHE_MB       = $CACHE_MB"
        echo ""
        exit 1
        ;;
esac
