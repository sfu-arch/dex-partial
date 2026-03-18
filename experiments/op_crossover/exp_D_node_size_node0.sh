#!/bin/bash
###############################################################################
# Experiment D: Node Size Structural Sweep — Node 0 (Memory Server)
#
# QUESTION: How does node size shift operating points?
# Node size is compile-time in both systems:
#   DEX:   kLeafPageSize / kInternalPageSize  in dex/include/Common.h  (bytes)
#   CHIME: leafSpanSize / internalSpanSize    in CHIME/include/Common.h (entries)
#
# CHIME entry→bytes: leafEntrySize ≈ 20 bytes → span32≈640B, span64≈1280B, span128≈2560B
#
# COMPARABLE ARRANGEMENT:
#   - 128 MB cache for both, uniform, 100% reads, 10M keys, 30 threads
#   - DEX leaf sizes: 512, 1024, 2048 bytes
#   - CHIME span sizes: 32, 64, 128 entries
#
# HOW TO RUN: Start node0 first; node1 within ~60s of each reset.
###############################################################################
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEX_DIR="$SCRIPT_DIR/../../dex"
CHIME_DIR="$SCRIPT_DIR/../../CHIME"
DEX_BUILD="$DEX_DIR/build"
CHIME_BUILD="$CHIME_DIR/build"

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

start_memcached() {
    echo ">>> [memc] Killing old memcached..."
    sudo pkill -9 memcached 2>/dev/null || true; sleep 3
    sudo memcached -u root -l 0.0.0.0 -p "$MEMC_PORT" -c 10000 -m 256 -d; sleep 2
    local retries=0
    while true; do
        local reply
        reply=$(printf "set ping 0 0 2\r\nok\r\nquit\r\n" | timeout 2 nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | tr -d '\r\n' || true)
        [[ "$reply" == *"STORED"* ]] && { echo ">>> [memc] Up."; break; }
        retries=$((retries + 1)); [ $retries -ge 15 ] && { echo "ERROR: memcached not up"; exit 1; }
        echo ">>> [memc] Waiting... ($retries)"; sleep 1
    done
    printf "flush_all\r\nquit\r\n" | timeout 3 nc -w 3 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | tr -d '\r\n' || true
    sleep 1
    printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | timeout 3 nc -w 3 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | tr -d '\r\n' | grep -q "STORED" || true
    printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | timeout 3 nc -w 3 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | tr -d '\r\n' | grep -q "STORED" || true
    echo ">>> [memc] serverNum=0 clientNum=0 set."
}

set_iter() {
    local val=$1 len=${#1}
    printf "set exp_iter 0 0 %d\r\n%s\r\nquit\r\n" "$len" "$val" \
        | timeout 3 nc -w 3 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | tr -d '\r\n' | grep -q "STORED" \
        && echo ">>> [memc] exp_iter=$val" || echo ">>> WARNING: set exp_iter=$val failed"
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

next_iter() {
    ITERATION=$((ITERATION + 1))
    start_memcached; set_iter "$ITERATION"
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
    echo ">>> Saved /tmp/newbench_latency_leaf${LEAF_SZ}"
done
# Restore DEX defaults
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
    echo ">>> Saved /tmp/latency_bench_span${SPAN}"
done
# Restore CHIME defaults
sed -i "s/constexpr uint32_t leafSpanSize\s*=\s*[0-9]*/constexpr uint32_t leafSpanSize    = 64/" "$CHIME_COMMON"
sed -i "s/constexpr uint32_t internalSpanSize\s*=\s*[0-9]*/constexpr uint32_t internalSpanSize = 64/" "$CHIME_COMMON"
sed -i "s/constexpr int kIndexCacheSize\s*=\s*[0-9]*/constexpr int kIndexCacheSize  = 100/" "$CHIME_COMMON"
echo ">>> All variants built. Defaults restored."

# ════ PHASE 2: DEX node size sweep (memory server) ════
echo ""; echo "════ PHASE 2: DEX Node Size Sweep (memory server) ════"
echo "  cache=${CACHE_MB}MB | uniform | 100% reads | 10M keys"

for LEAF_SZ in "${DEX_LEAF_SIZES[@]}"; do
    echo ""; echo "─── DEX leaf=${LEAF_SZ}B ───"
    cleanup; hugepages; next_iter

    echo ">>> Running DEX memory server (iter=$ITERATION, leaf=${LEAF_SZ}B)..."
    cd "$DEX_BUILD"  # cwd for .dat output
    sudo /tmp/newbench_latency_leaf${LEAF_SZ} \
        $NODE_COUNT $READ_RATIO $INSERT_RATIO $UPDATE_RATIO \
        $DELETE_RATIO $RANGE_RATIO $THREADS $MEM_THREADS \
        $CACHE_MB $UNIFORM $ZIPF_THETA $BULK_LOAD_M $WARMUP_M $RUN_M \
        $CHECK $TIME_BASED $EARLY_STOP $INDEX $RPC_RATE $ADMIT_RATE \
        $AUTO_TUNE $MAX_THREAD \
        2>&1 | tee "/tmp/expD_dex_leaf${LEAF_SZ}_node0.log" || true

    echo ">>> Done. Sleeping 8s..."; sleep 8
done

# ════ PHASE 3: CHIME node size sweep (memory server) ════
echo ""; echo "════ PHASE 3: CHIME Node Size Sweep (memory server) ════"
echo "  cache=${CHIME_CACHE}MB | uniform | 100% reads | 10M keys"

for SPAN in "${CHIME_SPAN_SIZES[@]}"; do
    echo ""; echo "─── CHIME span=${SPAN} ───"
    cleanup; hugepages; next_iter

    echo ">>> Running CHIME memory server (iter=$ITERATION, span=${SPAN})..."
    sudo /tmp/latency_bench_span${SPAN} \
        $NODE_COUNT $THREADS $READ_RATIO $RANGE_RATIO \
        $((RUN_M * 1000000)) $RANGE_SIZE $ZIPF_THETA $UNIFORM $BULK_LOAD_M \
        2>&1 | tee "/tmp/expD_chime_span${SPAN}_node0.log" || true

    echo ">>> Done. Sleeping 8s..."; sleep 8
done

echo ""; echo "════ EXP D Node 0 COMPLETE ════"
