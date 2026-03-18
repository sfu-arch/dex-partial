#!/bin/bash
###############################################################################
# Experiment D: Node Size Structural Sweep — Node 1 (Compute Node)
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
READ_RATIO=100; INSERT_RATIO=0; UPDATE_RATIO=0; DELETE_RATIO=0; RANGE_RATIO=0
CACHE_MB=128; CHIME_CACHE=128
UNIFORM=1; ZIPF_THETA=0.0
CHECK=0; TIME_BASED=0; EARLY_STOP=0; INDEX=0
RPC_RATE=0.0; ADMIT_RATE=1.0; AUTO_TUNE=0; MAX_THREAD=30
RANGE_SIZE=1

DEX_LEAF_SIZES=(512 1024 2048)
CHIME_SPAN_SIZES=(32 64 128)

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

DEX_COMMON="$DEX_DIR/include/Common.h"
CHIME_COMMON="$CHIME_DIR/include/Common.h"

# ════ PHASE 1: Build all variants upfront ════
echo ""; echo "════ PHASE 1: Build all variants ════"

for LEAF_SZ in "${DEX_LEAF_SIZES[@]}"; do
    echo ">>> Building DEX leaf=${LEAF_SZ}B..."
    sed -i "s/constexpr uint32_t kLeafPageSize\s*=\s*[0-9]*/constexpr uint32_t kLeafPageSize = ${LEAF_SZ}/" "$DEX_COMMON"
    sed -i "s/constexpr uint32_t kInternalPageSize\s*=\s*[0-9]*/constexpr uint32_t kInternalPageSize = ${LEAF_SZ}/" "$DEX_COMMON"
    cd "$DEX_DIR"; mkdir -p build; cd build
    cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
    make -j$(nproc) newbench_latency 2>&1 | tail -2
    cp "$DEX_BUILD/newbench_latency" "/tmp/newbench_latency_leaf${LEAF_SZ}"
done
sed -i "s/constexpr uint32_t kLeafPageSize\s*=\s*[0-9]*/constexpr uint32_t kLeafPageSize = 1024/" "$DEX_COMMON"
sed -i "s/constexpr uint32_t kInternalPageSize\s*=\s*[0-9]*/constexpr uint32_t kInternalPageSize = 1024/" "$DEX_COMMON"

sed -i "s/constexpr int kIndexCacheSize\s*=\s*[0-9]*/constexpr int kIndexCacheSize  = ${CHIME_CACHE}/" "$CHIME_COMMON"
for SPAN in "${CHIME_SPAN_SIZES[@]}"; do
    echo ">>> Building CHIME span=${SPAN}..."
    sed -i "s/constexpr uint32_t leafSpanSize\s*=\s*[0-9]*/constexpr uint32_t leafSpanSize    = ${SPAN}/" "$CHIME_COMMON"
    sed -i "s/constexpr uint32_t internalSpanSize\s*=\s*[0-9]*/constexpr uint32_t internalSpanSize = ${SPAN}/" "$CHIME_COMMON"
    cd "$CHIME_DIR"; rm -rf build; mkdir build; cd build
    cmake .. -DSHORT_TEST_EPOCH=ON > /dev/null 2>&1
    make -j$(nproc) latency_bench 2>&1 | tail -2
    cp "$CHIME_BUILD/latency_bench" "/tmp/latency_bench_span${SPAN}"
done
sed -i "s/constexpr uint32_t leafSpanSize\s*=\s*[0-9]*/constexpr uint32_t leafSpanSize    = 64/" "$CHIME_COMMON"
sed -i "s/constexpr uint32_t internalSpanSize\s*=\s*[0-9]*/constexpr uint32_t internalSpanSize = 64/" "$CHIME_COMMON"
sed -i "s/constexpr int kIndexCacheSize\s*=\s*[0-9]*/constexpr int kIndexCacheSize  = 100/" "$CHIME_COMMON"
echo ">>> All builds done."

# ════ PHASE 2: DEX node size sweep (compute) ════
echo ""; echo "════ PHASE 2: DEX Node Size Sweep (compute) | cache=${CACHE_MB}MB ════"

for LEAF_SZ in "${DEX_LEAF_SIZES[@]}"; do
    ITERATION=$((ITERATION + 1))
    echo ""; echo "─── DEX leaf=${LEAF_SZ}B (iter=$ITERATION) ───"
    cleanup; hugepages; wait_for_iter "$ITERATION"

    cd "$DEX_BUILD"
    sudo /tmp/newbench_latency_leaf${LEAF_SZ} \
        $NODE_COUNT $READ_RATIO $INSERT_RATIO $UPDATE_RATIO \
        $DELETE_RATIO $RANGE_RATIO $THREADS $MEM_THREADS \
        $CACHE_MB $UNIFORM $ZIPF_THETA $BULK_LOAD_M $WARMUP_M $RUN_M \
        $CHECK $TIME_BASED $EARLY_STOP $INDEX $RPC_RATE $ADMIT_RATE \
        $AUTO_TUNE $MAX_THREAD \
        2>&1 | tee "$RESULTS_DEX/expD_dex_leaf${LEAF_SZ}_stdout.log"

    save_results "$RESULTS_DEX/expD_dex_leaf${LEAF_SZ}" "dex"
    echo ">>> DEX leaf=${LEAF_SZ}B done. Sleeping 8s..."; sleep 8
done

# ════ PHASE 3: CHIME node size sweep (compute) ════
echo ""; echo "════ PHASE 3: CHIME Node Size Sweep (compute) | cache=${CHIME_CACHE}MB ════"

for SPAN in "${CHIME_SPAN_SIZES[@]}"; do
    ITERATION=$((ITERATION + 1))
    echo ""; echo "─── CHIME span=${SPAN} (iter=$ITERATION) ───"
    cleanup; hugepages; wait_for_iter "$ITERATION"

    cd "$CHIME_BUILD"
    sudo /tmp/latency_bench_span${SPAN} \
        $NODE_COUNT $THREADS $READ_RATIO $RANGE_RATIO \
        $((RUN_M * 1000000)) $RANGE_SIZE $ZIPF_THETA $UNIFORM $BULK_LOAD_M \
        2>&1 | tee "$RESULTS_CHIME/expD_chime_span${SPAN}_stdout.log"

    save_results "$RESULTS_CHIME/expD_chime_span${SPAN}" "chime"
    echo ">>> CHIME span=${SPAN} done. Sleeping 8s..."; sleep 8
done

echo ""; echo "════ EXP D Node 1 COMPLETE ════"
ls -la "$RESULTS_DEX" "$RESULTS_CHIME"
