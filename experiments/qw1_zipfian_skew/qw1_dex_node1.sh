#!/bin/bash
###############################################################################
# QW1: DEX Zipfian Skew Sweep — Node 1 (Worker/Memory Node)
#
# MUST run the SAME skew points in the SAME order as node0.
# Start this AFTER node0 is waiting for connection.
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEX_DIR="$SCRIPT_DIR/../../dex"
DEX_BUILD_DIR="$DEX_DIR/build"
MEMC_IP=$(head -1 "$DEX_DIR/memcached.conf")
MEMC_PORT=$(sed -n '2p' "$DEX_DIR/memcached.conf")

# ===================== SHARED CONFIGURATION (must match node0!) =====================
NODE_COUNT=2
READ_RATIO=70
INSERT_RATIO=0
UPDATE_RATIO=0
DELETE_RATIO=0
RANGE_RATIO=30
TOTAL_THREADS=16
MEM_THREADS=4
CACHE_MB=256
BULK_LOAD_M=10
WARMUP_M=1
RUN_M=5
CHECK=0
TIME_BASED=0
EARLY_STOP=0
INDEX=0
RPC_RATE=0.0
ADMIT_RATE=1.0
AUTO_TUNE=0
MAX_THREAD=16

SKEW_CONFIGS=(
    "1 0.0   uniform"
    "0 0.6   zipf_0.6"
    "0 0.8   zipf_0.8"
    "0 0.9   zipf_0.9"
    "0 0.99  zipf_0.99"
)

# Iteration counter — must stay in sync with node0
ITERATION=0

# ===================== HELPER: wait for node0 to signal new iteration =====================
wait_for_iteration() {
    local expected_iter="$1"
    echo ">>> [sync] Waiting for node0 to signal iteration $expected_iter (qw1_iter key)..."
    local attempts=0
    local max_attempts=120   # 4 min max (iteration may take time)
    while [ $attempts -lt $max_attempts ]; do
        if nc -z -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null; then
            local val
            val=$(printf "get qw1_iter\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | grep -A1 "^VALUE" | tail -1 | tr -d '\r')
            if [ "$val" = "$expected_iter" ]; then
                echo ">>> [sync] OK — qw1_iter=$expected_iter (node0 ready for this iteration)"
                # Now wait for node0's BINARY to register (serverNum >= 1)
                # This ensures node0 gets ID 0 (memory/primary role)
                echo ">>> [sync] Waiting for node0 binary to register (serverNum >= 1)..."
                local reg_attempts=0
                while [ $reg_attempts -lt 30 ]; do
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
                return 0
            fi
            [ -n "$val" ] && echo ">>> [sync] qw1_iter='$val' (waiting for $expected_iter)..."
        fi
        attempts=$((attempts + 1))
        [ $((attempts % 10)) -eq 0 ] && echo ">>> [sync] Still waiting... ($attempts/$max_attempts)"
        sleep 2
    done
    echo ">>> [sync] ERROR: Timed out waiting for iteration $expected_iter!"
    return 1
}

# ===================== HELPER: cleanup =====================
cleanup_previous_run() {
    echo ">>> [cleanup] Killing stale processes..."
    sudo pkill -9 newbench_latency 2>/dev/null || true
    sleep 1

    echo ">>> [cleanup] Clearing /dev/shm RDMA artifacts..."
    sudo rm -f /dev/shm/dsm_* /dev/shm/rdma_* 2>/dev/null || true
}

# ===================== BUILD =====================
echo ">>> Building DEX newbench_latency..."
cd "$DEX_DIR"
mkdir -p build && cd build
cmake .. > /dev/null 2>&1
make -j$(nproc) newbench_latency
echo ">>> Build complete."

# ===================== SWEEP =====================
echo ""
echo "============================================================"
echo "  DEX QW1 Zipfian Skew Sweep — Node 1 (Worker)"
echo "  Configs: uniform, zipf_0.6, zipf_0.8, zipf_0.9, zipf_0.99"
echo "============================================================"

for config in "${SKEW_CONFIGS[@]}"; do
    read -r UNIFORM ZIPF_THETA LABEL <<< "$config"
    
    echo ""
    echo "============================================================"
    echo "  DEX QW1 Worker — $LABEL (uniform=$UNIFORM, theta=$ZIPF_THETA)"
    echo "============================================================"
    
    # --- Full cleanup ---
    cleanup_previous_run
    
    # --- Hugepages ---
    echo ">>> [hugepages] Setting 36864 pages..."
    echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
    ulimit -l unlimited 2>/dev/null || true
    HP_FREE=$(grep HugePages_Free /proc/meminfo | awk '{print $2}')
    echo ">>> [hugepages] Free: $HP_FREE"
    
    # --- Wait for node0 to signal this iteration ---
    ITERATION=$((ITERATION + 1))
    wait_for_iteration "$ITERATION"
    
    # --- Run ---
    cd "$DEX_BUILD_DIR"
    echo ">>> Joining cluster for $LABEL..."
    
    sudo ./newbench_latency \
        ${NODE_COUNT} ${READ_RATIO} ${INSERT_RATIO} ${UPDATE_RATIO} \
        ${DELETE_RATIO} ${RANGE_RATIO} ${TOTAL_THREADS} ${MEM_THREADS} \
        ${CACHE_MB} ${UNIFORM} ${ZIPF_THETA} ${BULK_LOAD_M} ${WARMUP_M} ${RUN_M} \
        ${CHECK} ${TIME_BASED} ${EARLY_STOP} ${INDEX} ${RPC_RATE} ${ADMIT_RATE} \
        ${AUTO_TUNE} ${MAX_THREAD}
    
    echo ">>> $LABEL worker complete. Sleeping 5s..."
    sleep 5
done

echo ""
echo "============================================================"
echo "  DEX QW1 Worker — ALL POINTS COMPLETE"
echo "============================================================"
