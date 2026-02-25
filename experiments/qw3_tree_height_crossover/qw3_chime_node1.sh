#!/bin/bash
###############################################################################
# QW3: CHIME Tree Height / Key Count Sweep — Node 1 (Compute Node)
#
# This is the COMPUTE node that runs workloads and measures latency.
# Must be started AFTER node0 begins.
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHIME_DIR="$SCRIPT_DIR/../../CHIME"
CHIME_BUILD_DIR="$CHIME_DIR/build"
RESULTS_DIR="$SCRIPT_DIR/results/chime"
MEMC_IP=$(head -1 "$CHIME_DIR/memcached.conf")
MEMC_PORT=$(sed -n '2p' "$CHIME_DIR/memcached.conf")
COMMON_H="$CHIME_DIR/include/Common.h"
mkdir -p "$RESULTS_DIR"

# ===================== FIXED CONFIGURATION (must match node0) =====================
NODE_COUNT=2
THREAD_COUNT=30
READ_RATIO=100
RANGE_RATIO=0
TOTAL_OPS=10000000
CACHE_MB=64

# ===================== SWEEP CONFIGURATIONS (must match node0) =====================
KEY_COUNTS=(1 5 10 20 50 100)

DIST_CONFIGS=(
    "1 0.0   uniform"
    "0 0.6   zipf_0.6"
    "0 0.99  zipf_0.99"
)

# ===================== HELPER: Set cache size in Common.h =====================
set_cache_size() {
    local new_size=$1
    echo ">>> [config] Setting kIndexCacheSize = $new_size in Common.h..."
    sed -i "s/^constexpr int kIndexCacheSize.*/constexpr int kIndexCacheSize = ${new_size};/" "$COMMON_H"
    grep "kIndexCacheSize" "$COMMON_H" | head -1
}

# ===================== HELPER: wait for iteration =====================
wait_for_iteration() {
    local target_iter=$1
    echo ">>> Waiting for qw3_iter=$target_iter from node0..."
    while true; do
        local reply
        reply=$(printf "get qw3_iter\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | grep -A1 "VALUE" | tail -1 | tr -d '\r\n')
        if [[ "$reply" == "$target_iter" ]]; then
            echo ">>> Detected qw3_iter=$target_iter — proceeding"
            break
        fi
        sleep 2
    done
}

# ===================== HELPER: cleanup =====================
cleanup_previous_run() {
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
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc) chime_bench
cd "$SCRIPT_DIR"

# ===================== MAIN SWEEP =====================
echo ""
echo "============================================================"
echo "QW3: CHIME Tree Height Crossover — Node 1 (Compute)"
echo "============================================================"
echo ""

ITERATION=0

for KEY_M in "${KEY_COUNTS[@]}"; do
    for DIST_CFG in "${DIST_CONFIGS[@]}"; do
        read -r UNIFORM ZIPF LABEL <<< "$DIST_CFG"
        
        ITERATION=$((ITERATION + 1))
        RUN_LABEL="keys_${KEY_M}M_${LABEL}"
        STDOUT_LOG="$RESULTS_DIR/chime_${RUN_LABEL}_node1_stdout.log"
        LATENCY_FILE="$RESULTS_DIR/chime_${RUN_LABEL}_latency.dat"
        
        # Convert uniform flag to zipfian theta
        if [[ "$UNIFORM" == "1" ]]; then
            ZIPF_ARG="0.0"
        else
            ZIPF_ARG="$ZIPF"
        fi
        
        OPS_M=$((TOTAL_OPS / 1000000))
        
        echo ""
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo ">>> Iteration $ITERATION: ${RUN_LABEL}"
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        
        cleanup_previous_run
        wait_for_iteration "$ITERATION"
        
        sleep 3  # Give node0 time to start
        
        echo ">>> [exec] Launching chime_bench (compute node)..."
        
        "$CHIME_BUILD_DIR/chime_bench" \
            $NODE_COUNT $THREAD_COUNT \
            $READ_RATIO $ZIPF_ARG \
            $KEY_M $OPS_M $RANGE_RATIO \
            2>&1 | tee "$STDOUT_LOG"
        
        # Copy latency file if generated
        if [[ -f "chime_latency.dat" ]]; then
            mv chime_latency.dat "$LATENCY_FILE"
            echo ">>> Saved latency to: $LATENCY_FILE"
        fi
        
        echo ">>> Completed: ${RUN_LABEL}"
        sleep 3
    done
done

echo ""
echo "============================================================"
echo "QW3 CHIME Node 1: ALL RUNS COMPLETE"
echo "Results in: $RESULTS_DIR"
echo "============================================================"
