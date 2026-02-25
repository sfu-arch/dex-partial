#!/bin/bash
###############################################################################
# QW3: DEX Tree Height / Key Count Sweep — Node 0 (Primary Compute Node)
#
# Sweeps: Key counts 1M, 5M, 10M, 20M, 50M, 100M
# Distributions: Uniform, Zipfian θ = 0.6, 0.99
# Cache: 64MB (fixed - stress test caching)
# Workload: 100% Lookup (point queries only)
#
# USAGE: Run this FIRST on Node 0, then start node1 script on Node 1.
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEX_DIR="$SCRIPT_DIR/../../dex"
DEX_BUILD_DIR="$DEX_DIR/build"
RESULTS_DIR="$SCRIPT_DIR/results/dex"
MEMC_IP=$(head -1 "$DEX_DIR/memcached.conf")
MEMC_PORT=$(sed -n '2p' "$DEX_DIR/memcached.conf")
mkdir -p "$RESULTS_DIR"

# ===================== FIXED CONFIGURATION =====================
NODE_COUNT=2
READ_RATIO=100        # 100% reads - isolate point lookup
INSERT_RATIO=0
UPDATE_RATIO=0
DELETE_RATIO=0
RANGE_RATIO=0         # No range scans
TOTAL_THREADS=30
MEM_THREADS=4
CACHE_MB=64           # 64MB cache - stress test
WARMUP_M=1
RUN_M=10              # 10M ops for measurement
CHECK=0
TIME_BASED=0
EARLY_STOP=0
INDEX=0               # 0=DEX
RPC_RATE=0.0          # No offloading
ADMIT_RATE=1.0
AUTO_TUNE=0
MAX_THREAD=30

# ===================== SWEEP CONFIGURATIONS =====================
# Key counts in millions
KEY_COUNTS=(1 5 10 20 50 100)

# Distribution configs: "uniform zipfian_theta label"
DIST_CONFIGS=(
    "1 0.0   uniform"
    "0 0.6   zipf_0.6"
    "0 0.99  zipf_0.99"
)

ITERATION=0

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
echo "QW3: DEX Tree Height Crossover — Node 0"
echo "Cache: ${CACHE_MB}MB | Threads: ${TOTAL_THREADS}"
echo "Key counts: ${KEY_COUNTS[*]} (millions)"
echo "Distributions: uniform, zipf_0.6, zipf_0.99"
echo "============================================================"
echo ""

for KEY_M in "${KEY_COUNTS[@]}"; do
    for DIST_CFG in "${DIST_CONFIGS[@]}"; do
        read -r UNIFORM ZIPF LABEL <<< "$DIST_CFG"
        
        RUN_LABEL="keys_${KEY_M}M_${LABEL}"
        STDOUT_LOG="$RESULTS_DIR/dex_${RUN_LABEL}_stdout.log"
        LATENCY_FILE="$RESULTS_DIR/dex_${RUN_LABEL}_latency.dat"
        
        echo ""
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        echo ">>> Running: ${RUN_LABEL}"
        echo ">>>   Keys: ${KEY_M}M, Distribution: ${LABEL}"
        echo ">>>   Cache: ${CACHE_MB}MB"
        echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
        
        cleanup_previous_run
        flush_and_reset_memcached
        
        echo ">>> [exec] Launching newbench_latency..."
        
        # DEX arguments (23 total):
        # 1:kNodeCount 2:kReadRatio 3:kInsertRatio 4:kUpdateRatio 5:kDeleteRatio 6:kRangeRatio
        # 7:totalThreadCount 8:memThreadCount 9:cacheSize(MB) 10:uniform_workload 11:zipfian_theta
        # 12:bulk_load_num(M) 13:warmup_num(M) 14:op_num(M)
        # 15:check_correctness 16:time_based 17:early_stop
        # 18:index(0=DEX) 19:rpc_rate 20:admission_rate 21:auto_tune 22:kMaxThread
        
        "$DEX_BUILD_DIR/newbench_latency" \
            $NODE_COUNT $READ_RATIO $INSERT_RATIO $UPDATE_RATIO $DELETE_RATIO $RANGE_RATIO \
            $TOTAL_THREADS $MEM_THREADS $CACHE_MB \
            $UNIFORM $ZIPF \
            $KEY_M $WARMUP_M $RUN_M \
            $CHECK $TIME_BASED $EARLY_STOP \
            $INDEX $RPC_RATE $ADMIT_RATE $AUTO_TUNE $MAX_THREAD \
            2>&1 | tee "$STDOUT_LOG"
        
        # Copy latency file if generated
        if [[ -f "latency_read.dat" ]]; then
            mv latency_read.dat "$LATENCY_FILE"
            echo ">>> Saved latency to: $LATENCY_FILE"
        fi
        
        echo ">>> Completed: ${RUN_LABEL}"
        sleep 5
    done
done

echo ""
echo "============================================================"
echo "QW3 DEX Node 0: ALL RUNS COMPLETE"
echo "Results in: $RESULTS_DIR"
echo "============================================================"
