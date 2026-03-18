#!/bin/bash
###############################################################################
# Experiment A: Cache Size Crossover — Node 1 (Compute Node)
#
# Runs the COMPUTE role for both DEX and CHIME cache sweeps.
# Saves latency .dat files to results/dex/ and results/chime/.
#
# HOW TO RUN:
#   Start node0 first, then start this within ~60s.
#   This script will block waiting for each exp_iter sentinel from node0.
###############################################################################
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEX_DIR="$SCRIPT_DIR/../../dex"
CHIME_DIR="$SCRIPT_DIR/../../CHIME"
DEX_BUILD="$DEX_DIR/build"
CHIME_BUILD="$CHIME_DIR/build"
RESULTS_DEX="$SCRIPT_DIR/results/dex"
RESULTS_CHIME="$SCRIPT_DIR/results/chime"
mkdir -p "$RESULTS_DEX" "$RESULTS_CHIME"

MEMC_IP=$(head -1 "$DEX_DIR/memcached.conf")
MEMC_PORT=$(sed -n '2p' "$DEX_DIR/memcached.conf")
echo ">>> Memcached: ${MEMC_IP}:${MEMC_PORT}"

# ── Config (must match node0 exactly) ────────────────────────────────────────
NODE_COUNT=2
THREADS=30
MEM_THREADS=4
BULK_LOAD_M=10
WARMUP_M=1
RUN_M=10
READ_RATIO=100
INSERT_RATIO=0
UPDATE_RATIO=0
DELETE_RATIO=0
RANGE_RATIO=0
UNIFORM=1
ZIPF_THETA=0.0
CHECK=0
TIME_BASED=0
EARLY_STOP=0
INDEX=0
RPC_RATE=0.0
ADMIT_RATE=1.0
AUTO_TUNE=0
MAX_THREAD=30
RANGE_SIZE=1

DEX_CACHES=(64 128 256 512)
CHIME_CACHES=(32 64 100)

ITERATION=0

# ── Helpers ───────────────────────────────────────────────────────────────────
wait_for_iter() {
    local target=$1
    local elapsed=0
    echo ">>> Waiting for exp_iter=$target from node0..."
    while true; do
        local raw val
        raw=$(printf "get exp_iter\r\nquit\r\n" | timeout 3 nc -w 3 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null || true)
        # strip carriage returns, extract the value line (a bare integer after VALUE header)
        val=$(echo "$raw" | tr -d '\r' | awk '/^[0-9]+$/{print $1}' | head -1)
        if [ "$val" = "$target" ]; then
            echo ">>> Synced — exp_iter=$target. Starting compute..."
            return 0
        fi
        elapsed=$((elapsed + 2))
        if [ $elapsed -ge 300 ]; then
            echo "ERROR: Timed out waiting for exp_iter=$target after 300s (node0 may have hung)"
            exit 1
        fi
        sleep 2
    done
}

cleanup() {
    sudo pkill -9 newbench_latency 2>/dev/null || true
    sudo pkill -9 latency_bench    2>/dev/null || true
    sleep 2
    sudo rm -f /dev/shm/dsm_* /dev/shm/rdma_* 2>/dev/null || true
}

hugepages() {
    echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
    ulimit -l unlimited 2>/dev/null || true
}

save_results() {
    local prefix=$1 system=$2
    local saved=0
    for f in ${system}_read_latency.dat ${system}_range_latency.dat; do
        if [ -f "$f" ]; then
            cp "$f" "${prefix}_${f}" && echo ">>> Saved: ${prefix}_${f}" && saved=1 || true
        fi
    done
    [ $saved -eq 0 ] && echo ">>> WARNING: no .dat files found for ${system} in $(pwd)"
}

# ════════════════════════════════
# PHASE 1: Build everything upfront
# ════════════════════════════════
echo ""; echo "════════════════════════════════"
echo "  PHASE 1: Building all binaries"
echo "════════════════════════════════"

echo ">>> Building DEX newbench_latency..."
cd "$DEX_DIR"; mkdir -p build; cd build
cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
make -j$(nproc) newbench_latency 2>&1 | tail -3

CHIME_COMMON="$CHIME_DIR/include/Common.h"
for CHIME_CACHE in "${CHIME_CACHES[@]}"; do
    echo ">>> Building CHIME with kIndexCacheSize=${CHIME_CACHE}..."
    sed -i "s/constexpr int kIndexCacheSize\s*=\s*[0-9]*/constexpr int kIndexCacheSize  = ${CHIME_CACHE}/" "$CHIME_COMMON"
    cd "$CHIME_DIR"; rm -rf build; mkdir build; cd build
    cmake .. -DSHORT_TEST_EPOCH=ON > /dev/null 2>&1
    make -j$(nproc) latency_bench 2>&1 | tail -2
    cp "$CHIME_BUILD/latency_bench" "/tmp/latency_bench_cache${CHIME_CACHE}"
    echo ">>> Saved /tmp/latency_bench_cache${CHIME_CACHE}"
done
sed -i "s/constexpr int kIndexCacheSize\s*=\s*[0-9]*/constexpr int kIndexCacheSize  = 100/" "$CHIME_COMMON"
echo ">>> All builds done."

# ════════════════════════════════
# PHASE 2: DEX cache sweep (compute)
# ════════════════════════════════
echo ""; echo "════════════════════════════════════════════════════════"
echo "  PHASE 2: DEX Cache Sweep (compute node)"
echo "  Cache: ${DEX_CACHES[*]} MB | uniform | 100% reads"
echo "════════════════════════════════════════════════════════"

for CACHE_MB in "${DEX_CACHES[@]}"; do
    ITERATION=$((ITERATION + 1))
    echo ""; echo "─── DEX cache=${CACHE_MB}MB (iter=$ITERATION) ───"
    cleanup; hugepages
    wait_for_iter "$ITERATION"

    cd "$DEX_BUILD"
    sudo ./newbench_latency \
        $NODE_COUNT $READ_RATIO $INSERT_RATIO $UPDATE_RATIO \
        $DELETE_RATIO $RANGE_RATIO $THREADS $MEM_THREADS \
        $CACHE_MB $UNIFORM $ZIPF_THETA $BULK_LOAD_M $WARMUP_M $RUN_M \
        $CHECK $TIME_BASED $EARLY_STOP $INDEX $RPC_RATE $ADMIT_RATE \
        $AUTO_TUNE $MAX_THREAD \
        2>&1 | tee "$RESULTS_DEX/expA_dex_cache${CACHE_MB}mb_stdout.log" || true

    save_results "$RESULTS_DEX/expA_dex_cache${CACHE_MB}mb" "dex"
    echo ">>> DEX cache=${CACHE_MB}MB done. Sleeping 8s..."; sleep 8
done

# ════════════════════════════════
# PHASE 3: CHIME cache sweep (compute)
# ════════════════════════════════
echo ""; echo "════════════════════════════════════════════════════════"
echo "  PHASE 3: CHIME Cache Sweep (compute node)"
echo "  Cache: ${CHIME_CACHES[*]} MB | uniform | 100% reads"
echo "════════════════════════════════════════════════════════"

for CHIME_CACHE in "${CHIME_CACHES[@]}"; do
    ITERATION=$((ITERATION + 1))
    echo ""; echo "─── CHIME cache=${CHIME_CACHE}MB (iter=$ITERATION) ───"
    cleanup; hugepages
    wait_for_iter "$ITERATION"

    cd "$CHIME_BUILD"  # cwd for .dat file output
    sudo /tmp/latency_bench_cache${CHIME_CACHE} \
        $NODE_COUNT $THREADS $READ_RATIO $RANGE_RATIO \
        $((RUN_M * 1000000)) $RANGE_SIZE $ZIPF_THETA $UNIFORM $BULK_LOAD_M \
        2>&1 | tee "$RESULTS_CHIME/expA_chime_cache${CHIME_CACHE}mb_stdout.log" || true

    save_results "$RESULTS_CHIME/expA_chime_cache${CHIME_CACHE}mb" "chime"
    echo ">>> CHIME cache=${CHIME_CACHE}MB done. Sleeping 8s..."; sleep 8
done

echo ""; echo "════════════════════════════════"
echo "  EXP A Node 1 COMPLETE"
echo "  DEX results:   $RESULTS_DEX"
echo "  CHIME results: $RESULTS_CHIME"
echo "════════════════════════════════"
ls -la "$RESULTS_DEX" "$RESULTS_CHIME"
