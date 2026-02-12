#!/bin/bash
#############################################################################
# APEX Experiment Script
#
# This script runs APEX benchmarks on a 2-node RDMA cluster.
#
# Cluster Configuration:
#   - Node 0 (Memory Node): runs memcached + memory server
#   - Node 1 (Compute Node): runs the benchmark
#
# USAGE:
#   On Node 0: ./run_experiment.sh node0
#   On Node 1: ./run_experiment.sh node1
#############################################################################

set -e

# ─── Configuration ──────────────────────────────────────────────────
MEMORY_NODE_IP="10.30.1.9"       # [CONFIG] Set to your memory node IP
MEMCACHED_PORT="11211"
APEX_DIR="$(cd "$(dirname "$0")" && pwd)"

# Benchmark parameters
NODE_COUNT=2           # 1 memory + 1 compute
THREAD_COUNT=16        # Threads per compute node
READ_RATIO=70          # 70% point reads
RANGE_RATIO=30         # 30% range scans
TOTAL_OPS=5000000      # 5 million operations
RANGE_SIZE=100         # Range scan size

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [APEX] $1"
}

cleanup_all() {
    log "=== FULL CLEANUP ==="

    log "Killing APEX processes..."
    sudo pkill -9 latency_bench 2>/dev/null || true
    sudo pkill -9 ycsb_bench 2>/dev/null || true

    log "Killing memcached..."
    sudo pkill -9 memcached 2>/dev/null || true
    sleep 3

    log "Starting fresh memcached..."
    sudo memcached -u root -l "$MEMORY_NODE_IP" -p "$MEMCACHED_PORT" -c 10000 -d -P /tmp/memcached.pid
    sleep 3

    log "Initializing memcached keys..."
    echo -e "set serverNum 0 0 1\r\n0\r\nquit\r" | nc "$MEMORY_NODE_IP" "$MEMCACHED_PORT"
    echo -e "set clientNum 0 0 1\r\n0\r\nquit\r" | nc "$MEMORY_NODE_IP" "$MEMCACHED_PORT"
    sleep 1

    log "Verifying serverNum key..."
    echo -e "get serverNum\r\nquit\r" | nc "$MEMORY_NODE_IP" "$MEMCACHED_PORT"

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

build_apex() {
    log "Building APEX..."
    cd "$APEX_DIR"

    rm -rf build
    mkdir build
    cd build

    cmake ..
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
    build_apex

    cd "$APEX_DIR/build"

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

    log "Killing local APEX processes..."
    sudo pkill -9 latency_bench 2>/dev/null || true
    sudo pkill -9 ycsb_bench 2>/dev/null || true
    sleep 2

    setup_hugepages
    build_apex

    cd "$APEX_DIR/build"

    log "memcached.conf:"
    cat ../memcached.conf

    log ""
    log "Starting APEX benchmark..."
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
    log "  - apex_read_latency.csv"
    log "  - apex_range_latency.csv"
}

run_ycsb() {
    log "=========================================="
    log "Running APEX YCSB Benchmark"
    log "=========================================="

    local workload="${2:-a}"
    local threads="${3:-$THREAD_COUNT}"

    case "$workload" in
        a) READ=50; INSERT=0;  UPDATE=50 ;;
        b) READ=95; INSERT=0;  UPDATE=5  ;;
        c) READ=100; INSERT=0; UPDATE=0  ;;
        d) READ=95; INSERT=5;  UPDATE=0  ;;
        e) READ=0;  INSERT=5;  UPDATE=0  ;;  # range-heavy not modeled here
        f) READ=50; INSERT=0;  UPDATE=50 ;;
        *) log "Unknown workload: $workload"; exit 1 ;;
    esac

    cd "$APEX_DIR/build"
    log "YCSB Workload $workload: R=$READ% I=$INSERT% U=$UPDATE%"
    ./ycsb_bench $NODE_COUNT $threads $READ $INSERT $UPDATE $TOTAL_OPS 0.99
}

case "$1" in
    node0)
        run_node0
        ;;
    node1)
        run_node1
        ;;
    ycsb)
        run_ycsb "$@"
        ;;
    clean)
        cleanup_all
        log "Ready for fresh start"
        ;;
    build)
        build_apex
        ;;
    *)
        echo "APEX Experiment Runner"
        echo ""
        echo "Usage: $0 <command> [args]"
        echo ""
        echo "Commands:"
        echo "  node0          - Run on memory node"
        echo "  node1          - Run on compute node"
        echo "  ycsb [a-f] [t] - Run YCSB workload with t threads"
        echo "  clean          - Kill all processes and flush memcached"
        echo "  build          - Just build without running"
        echo ""
        echo "Order of execution:"
        echo "  1. On Node 0: $0 node0"
        echo "  2. On Node 1: $0 node1 (within 30 seconds)"
        exit 1
        ;;
esac
