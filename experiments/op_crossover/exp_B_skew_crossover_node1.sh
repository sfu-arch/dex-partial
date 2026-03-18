#!/bin/bash
###############################################################################
# Experiment B: Zipfian Skew Crossover — Node 1 (Compute Node)
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

NODE_COUNT=2; THREADS=30; MEM_THREADS=4
BULK_LOAD_M=10; WARMUP_M=1; RUN_M=10
READ_RATIO=70; INSERT_RATIO=0; UPDATE_RATIO=0; DELETE_RATIO=0; RANGE_RATIO=30
RANGE_SIZE=100; CACHE_MB=256; CHIME_CACHE=256
CHECK=0; TIME_BASED=0; EARLY_STOP=0; INDEX=0
RPC_RATE=0.0; ADMIT_RATE=1.0; AUTO_TUNE=0; MAX_THREAD=30

SKEW_CONFIGS=(
    "1  0.0   uniform"
    "0  0.6   zipf_0.6"
    "0  0.8   zipf_0.8"
    "0  0.9   zipf_0.9"
    "0  0.99  zipf_0.99"
)

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
    sudo pkill -9 newbench_latency 2>/dev/null || true
    sudo pkill -9 latency_bench    2>/dev/null || true
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

# ════ PHASE 1: Build ════
echo ""; echo "════ PHASE 1: Build ════"

echo ">>> Building DEX..."
cd "$DEX_DIR"; mkdir -p build; cd build
cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
make -j$(nproc) newbench_latency 2>&1 | tail -2

CHIME_COMMON="$CHIME_DIR/include/Common.h"
echo ">>> Building CHIME with kIndexCacheSize=${CHIME_CACHE}..."
sed -i "s/constexpr int kIndexCacheSize\s*=\s*[0-9]*/constexpr int kIndexCacheSize  = ${CHIME_CACHE}/" "$CHIME_COMMON"
cd "$CHIME_DIR"; rm -rf build; mkdir build; cd build
cmake .. -DSHORT_TEST_EPOCH=ON > /dev/null 2>&1
make -j$(nproc) latency_bench 2>&1 | tail -2
cp "$CHIME_BUILD/latency_bench" "/tmp/latency_bench_b256"
sed -i "s/constexpr int kIndexCacheSize\s*=\s*[0-9]*/constexpr int kIndexCacheSize  = 100/" "$CHIME_COMMON"
echo ">>> Both systems built."

# ════ PHASE 2: DEX skew sweep (compute) ════
echo ""; echo "════ PHASE 2: DEX Skew Sweep (compute) | cache=${CACHE_MB}MB ════"

for config in "${SKEW_CONFIGS[@]}"; do
    read -r UNIFORM ZIPF_THETA LABEL <<< "$config"
    ITERATION=$((ITERATION + 1))
    echo ""; echo "─── DEX $LABEL (iter=$ITERATION) ───"
    cleanup; hugepages; wait_for_iter "$ITERATION"

    cd "$DEX_BUILD"
    sudo ./newbench_latency \
        $NODE_COUNT $READ_RATIO $INSERT_RATIO $UPDATE_RATIO \
        $DELETE_RATIO $RANGE_RATIO $THREADS $MEM_THREADS \
        $CACHE_MB $UNIFORM $ZIPF_THETA $BULK_LOAD_M $WARMUP_M $RUN_M \
        $CHECK $TIME_BASED $EARLY_STOP $INDEX $RPC_RATE $ADMIT_RATE \
        $AUTO_TUNE $MAX_THREAD \
        2>&1 | tee "$RESULTS_DEX/expB_dex_${LABEL}_stdout.log"

    save_results "$RESULTS_DEX/expB_dex_${LABEL}" "dex"
    echo ">>> DEX $LABEL done. Sleeping 8s..."; sleep 8
done

# ════ PHASE 3: CHIME skew sweep (compute) ════
echo ""; echo "════ PHASE 3: CHIME Skew Sweep (compute) | cache=${CHIME_CACHE}MB ════"

for config in "${SKEW_CONFIGS[@]}"; do
    read -r UNIFORM ZIPF_THETA LABEL <<< "$config"
    ITERATION=$((ITERATION + 1))
    echo ""; echo "─── CHIME $LABEL (iter=$ITERATION) ───"
    cleanup; hugepages; wait_for_iter "$ITERATION"

    cd "$CHIME_BUILD"
    sudo /tmp/latency_bench_b256 \
        $NODE_COUNT $THREADS $READ_RATIO $RANGE_RATIO \
        $((RUN_M * 1000000)) $RANGE_SIZE $ZIPF_THETA $UNIFORM $BULK_LOAD_M \
        2>&1 | tee "$RESULTS_CHIME/expB_chime_${LABEL}_stdout.log"

    save_results "$RESULTS_CHIME/expB_chime_${LABEL}" "chime"
    echo ">>> CHIME $LABEL done. Sleeping 8s..."; sleep 8
done

echo ""; echo "════ EXP B Node 1 COMPLETE ════"
ls -la "$RESULTS_DEX" "$RESULTS_CHIME"
