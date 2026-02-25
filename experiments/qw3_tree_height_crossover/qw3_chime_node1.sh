#!/bin/bash
###############################################################################
# QW3: CHIME Tree Height / Key Count Sweep — Node 1 (Compute Node)
#
# This is the COMPUTE node that runs workloads and measures latency.
# Must be started AFTER node0 begins.
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

# ===================== FIXED CONFIGURATION (must match node0) =====================
NODE_COUNT=2
THREAD_COUNT=30
READ_RATIO=100
RANGE_RATIO=0
TOTAL_OPS=10000000
RANGE_SIZE=100
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
    
    # Wait for node0's binary to register (serverNum >= 1)
    echo ">>> [sync] Waiting for node0 binary to register (serverNum >= 1)..."
    local reg_attempts=0
    while [ $reg_attempts -lt 60 ]; do
        local sn
        sn=$(printf "get serverNum\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | grep -A1 "^VALUE" | tail -1 | tr -d '\r')
        if [ -n "$sn" ] && [ "$sn" -ge 1 ] 2>/dev/null; then
            echo ">>> [sync] node0 registered (serverNum=$sn). Starting in 1s..."
            sleep 1
            return 0
        fi
        reg_attempts=$((reg_attempts + 1))
        sleep 1
    done
    echo ">>> [sync] WARNING: serverNum never reached 1, starting anyway."
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
cmake .. -DCMAKE_BUILD_TYPE=Release -DSHORT_TEST_EPOCH=ON
make -j$(nproc) latency_bench
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
        
        echo ""
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo ">>> Iteration $ITERATION: ${RUN_LABEL}"
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        
        cleanup_previous_run
        
        # --- Hugepages ---
        echo ">>> [hugepages] Setting 36864 pages..."
        echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
        ulimit -l unlimited 2>/dev/null || true
        HP_FREE=$(grep HugePages_Free /proc/meminfo | awk '{print $2}')
        echo ">>> [hugepages] Free: $HP_FREE"
        
        wait_for_iteration "$ITERATION"
        
        echo ">>> [exec] Launching latency_bench (compute node)..."
        
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
        
        # Copy latency file if generated
        if [[ -f "$CHIME_BUILD_DIR/chime_read_latency.dat" ]]; then
            mv "$CHIME_BUILD_DIR/chime_read_latency.dat" "$LATENCY_FILE"
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
