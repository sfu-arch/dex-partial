#!/bin/bash
###############################################################################
# QW3: CHIME Tree Height / Key Count Sweep — Node 0 (Memory Node)
#
# Sweeps: Key counts 1M, 5M, 10M, 20M, 50M, 100M
# Distributions: Uniform, Zipfian θ = 0.6, 0.99
# Cache: 64MB (fixed - stress test caching)
# Workload: 100% Lookup (point queries only)
#
# USAGE: Run this FIRST on Node 0, then start node1 script on Node 1.
#
# NOTE: CHIME node 0 is the MEMORY node. It provides memory + coordination.
#       Latency files are saved by Node 1 (the compute node).
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHIME_DIR="$(cd "$SCRIPT_DIR/../../CHIME" && pwd)"
CHIME_BUILD_DIR="$CHIME_DIR/build"
RESULTS_DIR="$SCRIPT_DIR/results/chime"
MEMC_IP=$(head -1 "$CHIME_DIR/memcached.conf")
MEMC_PORT=$(sed -n '2p' "$CHIME_DIR/memcached.conf")
COMMON_H="$CHIME_DIR/include/Common.h"
mkdir -p "$RESULTS_DIR"

# ===================== FIXED CONFIGURATION =====================
NODE_COUNT=2
THREAD_COUNT=30
READ_RATIO=100        # 100% reads
RANGE_RATIO=0         # No range scans
TOTAL_OPS=10000000    # 10M ops
RANGE_SIZE=100        # Range scan size (unused with 0% range)
CACHE_MB=64           # 64MB cache - stress test

# ===================== SWEEP CONFIGURATIONS =====================
KEY_COUNTS=(1 5 10 20 50 100)

DIST_CONFIGS=(
    "1 0.0   uniform"
    "0 0.6   zipf_0.6"
    "0 0.99  zipf_0.99"
)

ITERATION=0

# ===================== HELPER: Set cache size in Common.h =====================
set_cache_size() {
    local new_size=$1
    echo ">>> [config] Setting kIndexCacheSize = $new_size in Common.h..."
    sed -i "s/^constexpr int kIndexCacheSize.*/constexpr int kIndexCacheSize = ${new_size};/" "$COMMON_H"
    grep "kIndexCacheSize" "$COMMON_H" | head -1
}

# ===================== HELPER: flush & reset memcached =====================
flush_and_reset_memcached() {
    echo ">>> [memcached] Killing any existing memcached..."
    sudo pkill -9 memcached 2>/dev/null || true
    sleep 2

    echo ">>> [memcached] Starting fresh memcached on 0.0.0.0:${MEMC_PORT}..."
    sudo memcached -u root -l 0.0.0.0 -p "$MEMC_PORT" -c 10000 -d
    sleep 2

    printf "flush_all\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null || true
    sleep 1

    printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
    printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
    sleep 1

    ITERATION=$((ITERATION + 1))
    local iter_len=${#ITERATION}
    printf "set qw3_iter 0 0 %d\r\n%s\r\nquit\r\n" "$iter_len" "$ITERATION" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
    echo ">>> [memcached] Set qw3_iter=$ITERATION"
}

# ===================== HELPER: cleanup =====================
cleanup_previous_run() {
    echo ">>> [cleanup] Killing stale processes..."
    sudo pkill -9 chime_bench 2>/dev/null || true
    sudo pkill -9 latency_bench 2>/dev/null || true
    sleep 1
    sudo rm -f /dev/shm/dsm_* /dev/shm/rdma_* 2>/dev/null || true
}

# ===================== SET CACHE SIZE & BUILD =====================
echo ">>> [config] Setting cache size to ${CACHE_MB}MB..."
set_cache_size $CACHE_MB

echo ">>> [build] Building CHIME..."
mkdir -p "$CHIME_BUILD_DIR"
cd "$CHIME_BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release -DSHORT_TEST_EPOCH=ON
make -j$(nproc) latency_bench
cd "$SCRIPT_DIR"

# ===================== MAIN SWEEP =====================
echo ""
echo "============================================================"
echo "QW3: CHIME Tree Height Crossover — Node 0 (Memory)"
echo "Cache: ${CACHE_MB}MB | Threads: ${THREAD_COUNT}"
echo "Key counts: ${KEY_COUNTS[*]} (millions)"
echo "Distributions: uniform, zipf_0.6, zipf_0.99"
echo "============================================================"
echo ""

for KEY_M in "${KEY_COUNTS[@]}"; do
    for DIST_CFG in "${DIST_CONFIGS[@]}"; do
        read -r UNIFORM ZIPF LABEL <<< "$DIST_CFG"
        
        RUN_LABEL="keys_${KEY_M}M_${LABEL}"
        STDOUT_LOG="$RESULTS_DIR/chime_${RUN_LABEL}_stdout.log"
        
        echo ""
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo ">>> Running: ${RUN_LABEL}"
        echo ">>>   Keys: ${KEY_M}M, Distribution: ${LABEL}"
        echo ">>>   Cache: ${CACHE_MB}MB"
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        
        cleanup_previous_run
        
        # --- Hugepages ---
        echo ">>> [hugepages] Setting 36864 pages..."
        echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
        ulimit -l unlimited 2>/dev/null || true
        HP_FREE=$(grep HugePages_Free /proc/meminfo | awk '{print $2}')
        echo ">>> [hugepages] Free: $HP_FREE"
        
        flush_and_reset_memcached
        
        echo ">>> [exec] Launching latency_bench (memory node)..."
        
        # Change to build directory so ../memcached.conf is found
        cd "$CHIME_BUILD_DIR"
        
        # CHIME latency_bench args:
        # <kNodeCount> <kThreadCount> <read_ratio> <range_ratio>
        # <total_ops> <range_size> <zipfian_theta> <uniform> <bulk_load_M>
        sudo "$CHIME_BUILD_DIR/latency_bench" \
            $NODE_COUNT $THREAD_COUNT \
            $READ_RATIO $RANGE_RATIO \
            $TOTAL_OPS $RANGE_SIZE \
            $ZIPF $UNIFORM \
            $KEY_M \
            2>&1 | tee "$STDOUT_LOG"
        
        # Return to script directory
        cd "$SCRIPT_DIR"
        
        echo ">>> Completed: ${RUN_LABEL}"
        sleep 5
    done
done

echo ""
echo "============================================================"
echo "QW3 CHIME Node 0: ALL RUNS COMPLETE"
echo "Results in: $RESULTS_DIR"
echo "============================================================"
