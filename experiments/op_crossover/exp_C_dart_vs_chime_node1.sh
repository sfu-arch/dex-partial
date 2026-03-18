#!/bin/bash
###############################################################################
# Experiment C: DART vs CHIME Crossover — Node 1 (Compute Node)
###############################################################################
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHIME_DIR="$SCRIPT_DIR/../../CHIME"
DART_DIR="$SCRIPT_DIR/../../DART-main"
CHIME_BUILD="$CHIME_DIR/build"
DART_BIN="$DART_DIR/bin"
RESULTS_CHIME="$SCRIPT_DIR/results/chime"
RESULTS_DART="$SCRIPT_DIR/results/dart"
mkdir -p "$RESULTS_CHIME" "$RESULTS_DART"

MEMC_IP=$(head -1 "$CHIME_DIR/memcached.conf")
MEMC_PORT=$(sed -n '2p' "$CHIME_DIR/memcached.conf")
echo ">>> Memcached: ${MEMC_IP}:${MEMC_PORT}"

NODE_COUNT=2; THREADS=30
BULK_LOAD_M=2; RUN_M=5
READ_RATIO=100; RANGE_RATIO=0; RANGE_SIZE=1

SKEW_CONFIGS=(
    "1 0.0  uniform"
    "0 0.99 zipf_0.99"
)
CHIME_CACHES=(4 16 32 64 100)

ITERATION=0

wait_for_iter() {
    local target=$1 elapsed=0
    echo ">>> Waiting for exp_iter=$target ..."
    while true; do
        local raw val
        raw=$(printf "get exp_iter\r\nquit\r\n" | nc -q1 -w 3 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null || true)
        val=$(echo "$raw" | tr -d '\r' | awk '/^[0-9]+$/{print $1}' | head -1)
        [ "$val" = "$target" ] && { echo ">>> Synced — exp_iter=$target"; return 0; }
        elapsed=$((elapsed + 2))
        [ $elapsed -ge 300 ] && { echo "ERROR: timeout waiting for iter=$target"; exit 1; }
        sleep 2
    done
}

cleanup() {
    sudo pkill -9 latency_bench 2>/dev/null || true
    sudo pkill -9 compute       2>/dev/null || true
    sleep 2; sudo rm -f /dev/shm/dsm_* /dev/shm/rdma_* 2>/dev/null || true
}

hugepages() {
    echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
    ulimit -l unlimited 2>/dev/null || true
}

save_results() {
    local prefix=$1 system=$2
    for f in ${system}_read_latency.dat ${system}_range_latency.dat; do
        [ -f "$f" ] && { cp "$f" "${prefix}_${f}"; echo ">>> Saved ${prefix}_${f}"; } || true
    done
}

CHIME_COMMON="$CHIME_DIR/include/Common.h"

# ════ PHASE 1: Build ════
echo ""; echo "════ PHASE 1: Build ════"

for CHIME_CACHE in "${CHIME_CACHES[@]}"; do
    echo ">>> Building CHIME cache=${CHIME_CACHE}MB..."
    sed -i "s/constexpr int kIndexCacheSize\s*=\s*[0-9]*/constexpr int kIndexCacheSize  = ${CHIME_CACHE}/" "$CHIME_COMMON"
    cd "$CHIME_DIR"; rm -rf build; mkdir build; cd build
    cmake .. -DSHORT_TEST_EPOCH=ON > /dev/null 2>&1
    make -j$(nproc) latency_bench 2>&1 | tail -2
    cp "$CHIME_BUILD/latency_bench" "/tmp/latency_bench_c${CHIME_CACHE}"
done
sed -i "s/constexpr int kIndexCacheSize\s*=\s*[0-9]*/constexpr int kIndexCacheSize  = 100/" "$CHIME_COMMON"

echo ">>> Building DART..."
cd "$DART_DIR"; mkdir -p build; cd build
cmake .. > /dev/null 2>&1; make -j$(nproc) 2>&1 | tail -3
echo ">>> All builds done."

# ════ PHASE 2: CHIME cache sweep (compute) ════
echo ""; echo "════ PHASE 2: CHIME cache sweep (compute) ════"

for CHIME_CACHE in "${CHIME_CACHES[@]}"; do
    for config in "${SKEW_CONFIGS[@]}"; do
        read -r UNIFORM ZIPF_THETA LABEL <<< "$config"
        ITERATION=$((ITERATION + 1))
        FULL_LABEL="c${CHIME_CACHE}_${LABEL}"
        echo ""; echo "─── CHIME cache=${CHIME_CACHE}MB $LABEL (iter=$ITERATION) ───"
        cleanup; hugepages; wait_for_iter "$ITERATION"

        cd "$CHIME_BUILD"
        sudo /tmp/latency_bench_c${CHIME_CACHE} \
            $NODE_COUNT $THREADS $READ_RATIO $RANGE_RATIO \
            $((RUN_M * 1000000)) $RANGE_SIZE $ZIPF_THETA $UNIFORM $BULK_LOAD_M \
            2>&1 | tee "$RESULTS_CHIME/expC_chime_${FULL_LABEL}_stdout.log"

        save_results "$RESULTS_CHIME/expC_chime_${FULL_LABEL}" "chime"
        echo ">>> Done. Sleeping 8s..."; sleep 8
    done
done

# ════ PHASE 3: DART compute ════
echo ""; echo "════ PHASE 3: DART compute ════"

if [ ! -d "$DART_DIR/workload/split" ]; then
    echo "WARNING: DART workload not found at $DART_DIR/workload/split"
    echo "  Run workload generation first on this node:"
    echo "  cd $DART_DIR && python3 benchmark_script/gen_workload.py"
fi

for config in "${SKEW_CONFIGS[@]}"; do
    read -r UNIFORM ZIPF_THETA LABEL <<< "$config"
    ITERATION=$((ITERATION + 1))
    echo ""; echo "─── DART $LABEL (iter=$ITERATION) ───"
    cleanup; hugepages; wait_for_iter "$ITERATION"

    cd "$DART_BIN"
    if [ -d "$DART_DIR/workload/split" ]; then
        sudo ./compute \
            2>&1 | tee "$RESULTS_DART/expC_dart_${LABEL}_stdout.log"
        echo ">>> DART $LABEL done."
    else
        echo ">>> SKIPPING DART (no workload files)"
    fi
    sleep 8
done

echo ""; echo "════ EXP C Node 1 COMPLETE ════"
ls -la "$RESULTS_CHIME" "$RESULTS_DART"
