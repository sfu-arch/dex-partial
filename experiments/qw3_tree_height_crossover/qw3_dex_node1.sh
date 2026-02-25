#!/bin/bash
###############################################################################
# QW3: DEX Tree Height / Key Count Sweep — Node 1 (Secondary Compute Node)
#
# This node waits for each iteration from node0 and runs the matching config.
# Must be started AFTER node0 begins.
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEX_DIR="$(cd "$SCRIPT_DIR/../../dex" && pwd)"
DEX_BUILD_DIR="$DEX_DIR/build"
MEMC_IP=$(head -1 "$DEX_DIR/memcached.conf")
MEMC_PORT=$(sed -n '2p' "$DEX_DIR/memcached.conf")

# ===================== FIXED CONFIGURATION (must match node0) =====================
NODE_COUNT=2
READ_RATIO=100
INSERT_RATIO=0
UPDATE_RATIO=0
DELETE_RATIO=0
RANGE_RATIO=0
TOTAL_THREADS=30
MEM_THREADS=4
CACHE_MB=64
WARMUP_M=1
RUN_M=10
CHECK=0
TIME_BASED=0
EARLY_STOP=0
INDEX=0
RPC_RATE=0.0
ADMIT_RATE=1.0
AUTO_TUNE=0
MAX_THREAD=30

# ===================== SWEEP CONFIGURATIONS (must match node0) =====================
KEY_COUNTS=(1 5 10 20 50 100)

DIST_CONFIGS=(
    "1 0.0   uniform"
    "0 0.6   zipf_0.6"
    "0 0.99  zipf_0.99"
)

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
    # This ensures node0 gets ID 0 (memory/primary role)
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
    sudo pkill -9 newbench_latency 2>/dev/null || true
    sleep 1
    sudo rm -f /dev/shm/dsm_* /dev/shm/rdma_* 2>/dev/null || true
}

# ===================== BUILD =====================
echo ">>> [build] Building DEX..."
cd "$DEX_BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc) newbench_latency
cd "$SCRIPT_DIR"

# ===================== MAIN SWEEP =====================
echo ""
echo "============================================================"
echo "QW3: DEX Tree Height Crossover — Node 1 (Secondary)"
echo "============================================================"
echo ""

ITERATION=0

for KEY_M in "${KEY_COUNTS[@]}"; do
    for DIST_CFG in "${DIST_CONFIGS[@]}"; do
        read -r UNIFORM ZIPF LABEL <<< "$DIST_CFG"
        
        ITERATION=$((ITERATION + 1))
        RUN_LABEL="keys_${KEY_M}M_${LABEL}"
        
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
        
        echo ">>> [exec] Launching newbench_latency..."
        
        # Change to build directory so ../memcached.conf is found
        cd "$DEX_BUILD_DIR"
        
        sudo "$DEX_BUILD_DIR/newbench_latency" \
            $NODE_COUNT $READ_RATIO $INSERT_RATIO $UPDATE_RATIO $DELETE_RATIO $RANGE_RATIO \
            $TOTAL_THREADS $MEM_THREADS $CACHE_MB \
            $UNIFORM $ZIPF \
            $KEY_M $WARMUP_M $RUN_M \
            $CHECK $TIME_BASED $EARLY_STOP \
            $INDEX $RPC_RATE $ADMIT_RATE $AUTO_TUNE $MAX_THREAD
        
        # Return to script directory
        cd "$SCRIPT_DIR"
        
        echo ">>> Completed: ${RUN_LABEL}"
        sleep 3
    done
done

echo ""
echo "============================================================"
echo "QW3 DEX Node 1: ALL RUNS COMPLETE"
echo "============================================================"
