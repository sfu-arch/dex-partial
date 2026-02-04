#!/bin/bash
#############################################################################
# CHIME Latency Experiment Script
# 
# This script runs CHIME with 500ns latency buckets for reads and range scans.
# 
# Cluster Configuration:
#   - Node 0 (10.30.1.9): Memory Node - runs memcached + memory server
#   - Node 1 (10.30.1.6): Compute Node - runs actual benchmark
#
# USAGE:
#   On Node 0: ./run_experiment.sh node0
#   On Node 1: ./run_experiment.sh node1
#############################################################################

set -e

# Configuration
MEMORY_NODE_IP="10.30.1.9"
MEMCACHED_PORT="11211"
CHIME_DIR="/home/apa222/dex-partial/CHIME"

# Benchmark parameters
NODE_COUNT=2           # 1 memory + 1 compute
THREAD_COUNT=1         # Threads per compute node
READ_RATIO=70          # 70% point reads
RANGE_RATIO=30         # 30% range scans  
TOTAL_OPS=1000000      # 1 million operations
RANGE_SIZE=50          # Range scan size

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1"
}

cleanup_all() {
    log "=== FULL CLEANUP ==="
    
    # Kill any running CHIME processes (with sudo)
    log "Killing CHIME processes..."
    sudo pkill -9 latency_bench 2>/dev/null || true
    sudo pkill -9 ycsb_test 2>/dev/null || true
    sudo pkill -9 simple_bench 2>/dev/null || true
    
    # Kill memcached (with sudo)
    log "Killing memcached..."
    sudo pkill -9 memcached 2>/dev/null || true
    
    # Wait for processes to die
    sleep 3
    
    # Start fresh memcached (with sudo)
    log "Starting fresh memcached..."
    sudo memcached -u root -l "$MEMORY_NODE_IP" -p "$MEMCACHED_PORT" -c 10000 -d
    sleep 2
    
    # Delete the serverNum key (this is what assigns node IDs)
    log "Deleting serverNum key..."
    echo "delete serverNum" | nc -q 1 "$MEMORY_NODE_IP" "$MEMCACHED_PORT" 2>/dev/null || true
    
    # Flush all memcached data
    log "Flushing memcached data..."
    echo "flush_all" | nc -q 1 "$MEMORY_NODE_IP" "$MEMCACHED_PORT" 2>/dev/null || true
    sleep 1
    
    # Verify memcached is running
    if pgrep memcached > /dev/null; then
        log "memcached is running and flushed"
    else
        log "WARNING: memcached may not be running"
    fi
    
    log "=== CLEANUP COMPLETE ==="
}

setup_hugepages() {
    log "Setting up hugepages..."
    echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
    ulimit -l unlimited 2>/dev/null || true
    log "Hugepages: $(cat /proc/sys/vm/nr_hugepages)"
}

build_chime() {
    log "Building CHIME..."
    cd "$CHIME_DIR"
    
    # Clean build directory
    rm -rf build
    mkdir build
    cd build
    
    cmake .. -DSHORT_TEST_EPOCH=ON
    make -j$(nproc)
    
    if [ -f "latency_bench" ]; then
        log "Build successful: latency_bench exists"
    else
        log "ERROR: latency_bench not found!"
        ls -la
        exit 1
    fi
    
    cd ..
}

run_node0() {
    log "=========================================="
    log "Running on Node 0 (Memory Node)"
    log "=========================================="
    
    cleanup_all
    setup_hugepages
    build_chime
    
    cd "$CHIME_DIR/build"
    
    log "memcached.conf:"
    cat ../memcached.conf
    
    log ""
    log "Starting memory server..."
    log "Waiting for Node 1 to connect..."
    log ""
    
    ./latency_bench $NODE_COUNT $THREAD_COUNT $READ_RATIO $RANGE_RATIO $TOTAL_OPS $RANGE_SIZE
}

run_node1() {
    log "=========================================="
    log "Running on Node 1 (Compute Node)"
    log "=========================================="
    
    # Just kill local processes, don't touch memcached (with sudo)
    log "Killing local CHIME processes..."
    sudo pkill -9 latency_bench 2>/dev/null || true
    sudo pkill -9 ycsb_test 2>/dev/null || true
    sudo pkill -9 simple_bench 2>/dev/null || true
    sleep 2
    
    setup_hugepages
    build_chime
    
    cd "$CHIME_DIR/build"
    
    log "memcached.conf:"
    cat ../memcached.conf
    
    log ""
    log "Starting benchmark..."
    log "Parameters:"
    log "  Nodes:       $NODE_COUNT"
    log "  Threads:     $THREAD_COUNT"
    log "  Read ratio:  $READ_RATIO%"
    log "  Range ratio: $RANGE_RATIO%"
    log "  Total ops:   $TOTAL_OPS"
    log "  Range size:  $RANGE_SIZE"
    log ""
    
    ./latency_bench $NODE_COUNT $THREAD_COUNT $READ_RATIO $RANGE_RATIO $TOTAL_OPS $RANGE_SIZE
    
    log ""
    log "Results saved to:"
    log "  - chime_read_latency.csv"
    log "  - chime_range_latency.csv"
}

case "$1" in
    node0)
        run_node0
        ;;
    node1)
        run_node1
        ;;
    clean)
        cleanup_all
        log "Ready for fresh start"
        ;;
    build)
        build_chime
        ;;
    *)
        echo "CHIME Latency Experiment"
        echo ""
        echo "Usage: $0 <command>"
        echo ""
        echo "Commands:"
        echo "  node0  - Run on memory node (10.30.1.9)"
        echo "  node1  - Run on compute node (10.30.1.6)"
        echo "  clean  - Kill all processes and flush memcached"
        echo "  build  - Just build without running"
        echo ""
        echo "Order of execution:"
        echo "  1. On Node 0: $0 node0"
        echo "  2. On Node 1: $0 node1 (within 30 seconds)"
        exit 1
        ;;
esac
