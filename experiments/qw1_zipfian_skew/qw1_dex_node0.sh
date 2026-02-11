#!/bin/bash
###############################################################################
# QW1: DEX Zipfian Skew Sweep — Node 0 (Primary Compute Node)
#
# Sweeps: Uniform, Zipfian θ = 0.6, 0.8, 0.9, 0.99
# Workload: 70% Lookup + 30% Range Scan (identical to CHIME experiment)
# Latency: 500ns buckets, per-op (read + range separate)
#
# USAGE: Run this FIRST on Node 0, then start node1 script on Node 1.
#        This script runs ALL skew points sequentially. Node 1 must also
#        run all points in the same order.
#
# MATCHED TO CHIME:
#   - Same key space: 10,001,000 (BULK_LOAD_M=10 + 1000 padding)
#   - Same key derivation: CityHash64
#   - Same operation mix: 70% read + 30% range
#   - Same thread count: 16
#   - Same cache size: 256MB
#   - Same warmup: 1M ops full-mix
#   - Same measurement: 5M ops
#   - Same latency granularity: 500ns buckets
#   - Same Zipfian library: mehcached zipf.h
#   - No offloading (RPC_RATE=0)
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEX_DIR="$SCRIPT_DIR/../../dex"
DEX_BUILD_DIR="$DEX_DIR/build"
RESULTS_DIR="$SCRIPT_DIR/results/dex"
MEMC_IP=$(head -1 "$DEX_DIR/memcached.conf")
MEMC_PORT=$(sed -n '2p' "$DEX_DIR/memcached.conf")
mkdir -p "$RESULTS_DIR"

# ===================== SHARED CONFIGURATION (must match node1 & CHIME!) =====================
NODE_COUNT=2
READ_RATIO=70
INSERT_RATIO=0
UPDATE_RATIO=0
DELETE_RATIO=0
RANGE_RATIO=30
TOTAL_THREADS=30
MEM_THREADS=8
CACHE_MB=256
BULK_LOAD_M=10       # → kKeySpace = 10M + 0 + 1000 = 10,001,000
WARMUP_M=1
RUN_M=30             # 30M ops total = CHIME's TOTAL_OPS=30000000
CHECK=0
TIME_BASED=0
EARLY_STOP=0
INDEX=0              # 0=DEX
RPC_RATE=0.0         # No offloading — match CHIME (no offload equiv.)
ADMIT_RATE=1.0
AUTO_TUNE=0
MAX_THREAD=30

# Skew sweep: uniform first, then increasing zipfian skew
SKEW_CONFIGS=(
    "1 0.0   uniform"
    "0 0.6   zipf_0.6"
    "0 0.8   zipf_0.8"
    "0 0.9   zipf_0.9"
    "0 0.99  zipf_0.99"
)

# Iteration counter for cross-node synchronization
ITERATION=0

# ===================== HELPER: flush & reset memcached =====================
flush_and_reset_memcached() {
    echo ">>> [memcached] Killing any existing memcached..."
    sudo pkill -9 memcached 2>/dev/null || true
    sleep 2

    echo ">>> [memcached] Starting fresh memcached on 0.0.0.0:${MEMC_PORT}..."
    sudo memcached -u root -l 0.0.0.0 -p "$MEMC_PORT" -c 10000 -d
    sleep 2

    # flush_all clears all stored data (safety net even though we just restarted)
    echo ">>> [memcached] Flushing all keys (flush_all)..."
    printf "flush_all\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null || true
    sleep 1

    # Seed coordination keys (both DEX and CHIME expect these to start at 0)
    echo ">>> [memcached] Setting serverNum=0, clientNum=0..."
    printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
    printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
    sleep 1

    # Verify: read back serverNum to make sure memcached is live and clean
    local reply
    reply=$(printf "get serverNum\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | head -1)
    if [[ "$reply" == *"VALUE"* ]]; then
        echo ">>> [memcached] Verified OK — serverNum key present"
    else
        echo ">>> [memcached] WARNING: could not verify serverNum (reply: $reply)"
        echo ">>> Retrying memcached start..."
        sudo pkill -9 memcached 2>/dev/null || true
        sleep 2
        sudo memcached -u root -l 0.0.0.0 -p "$MEMC_PORT" -c 10000 -d
        sleep 3
        printf "flush_all\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null || true
        printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
        printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
        sleep 1
    fi

    # Set iteration sentinel (node1 watches for this — binaries never touch it)
    ITERATION=$((ITERATION + 1))
    local iter_len=${#ITERATION}
    printf "set qw1_iter 0 0 %d\r\n%s\r\nquit\r\n" "$iter_len" "$ITERATION" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
    echo ">>> [memcached] Set qw1_iter=$ITERATION (node1 sync sentinel)"
}

# ===================== HELPER: clean previous run state =====================
cleanup_previous_run() {
    echo ">>> [cleanup] Killing stale processes..."
    sudo pkill -9 newbench_latency 2>/dev/null || true
    sleep 1

    # Remove leftover POSIX shared memory segments (avoid RDMA registration conflicts)
    echo ">>> [cleanup] Clearing /dev/shm RDMA artifacts..."
    sudo rm -f /dev/shm/dsm_* /dev/shm/rdma_* 2>/dev/null || true

    # Remove stale latency files from previous iteration
    cd "$DEX_BUILD_DIR" 2>/dev/null || true
    rm -f dex_read_latency.dat dex_range_latency.dat 2>/dev/null || true
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
echo "  DEX QW1 Zipfian Skew Sweep — Node 0 (Primary)"
echo "  Configs: uniform, zipf_0.6, zipf_0.8, zipf_0.9, zipf_0.99"
echo "============================================================"

for config in "${SKEW_CONFIGS[@]}"; do
    read -r UNIFORM ZIPF_THETA LABEL <<< "$config"
    
    echo ""
    echo "============================================================"
    echo "  DEX QW1 — $LABEL (uniform=$UNIFORM, theta=$ZIPF_THETA)"
    echo "============================================================"
    
    # --- Full cleanup ---
    cleanup_previous_run

    # --- Hugepages ---
    echo ">>> [hugepages] Setting 36864 pages..."
    echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
    ulimit -l unlimited 2>/dev/null || true
    HP_FREE=$(grep HugePages_Free /proc/meminfo | awk '{print $2}')
    echo ">>> [hugepages] Free: $HP_FREE"

    # --- Memcached flush & reset ---
    flush_and_reset_memcached
    
    # --- Run benchmark ---
    cd "$DEX_BUILD_DIR"
    echo ">>> Running DEX with $LABEL ... waiting for Node 1 to connect."
    echo ">>> Config: ${READ_RATIO}% read + ${RANGE_RATIO}% range, ${RUN_M}M ops, cache=${CACHE_MB}MB"
    echo ""
    
    sudo ./newbench_latency \
        ${NODE_COUNT} ${READ_RATIO} ${INSERT_RATIO} ${UPDATE_RATIO} \
        ${DELETE_RATIO} ${RANGE_RATIO} ${TOTAL_THREADS} ${MEM_THREADS} \
        ${CACHE_MB} ${UNIFORM} ${ZIPF_THETA} ${BULK_LOAD_M} ${WARMUP_M} ${RUN_M} \
        ${CHECK} ${TIME_BASED} ${EARLY_STOP} ${INDEX} ${RPC_RATE} ${ADMIT_RATE} \
        ${AUTO_TUNE} ${MAX_THREAD} \
        2>&1 | tee "$RESULTS_DIR/dex_${LABEL}_stdout.log"
    
    # --- Collect results ---
    for f in dex_read_latency.dat dex_range_latency.dat; do
        if [ -f "$f" ]; then
            cp "$f" "$RESULTS_DIR/dex_${LABEL}_${f}"
            echo ">>> Saved $RESULTS_DIR/dex_${LABEL}_${f}"
        else
            echo ">>> WARNING: $f not found after $LABEL run!"
        fi
    done
    
    echo ">>> $LABEL complete. Sleeping 5s before next..."
    sleep 5
done

echo ""
echo "============================================================"
echo "  DEX QW1 SWEEP COMPLETE — Node 0"
echo "  Results in: $RESULTS_DIR"
echo "============================================================"
ls -la "$RESULTS_DIR"
