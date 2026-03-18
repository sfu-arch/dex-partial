#!/bin/bash
###############################################################################
# Experiment B: Zipfian Skew Crossover — Node 0 (Memory Server)
#
# QUESTION: When is CHIME worse than DEX?
# ANSWER:   Large cache (256 MB) + high skew. DEX page cache absorbs hot pages
#           entirely in local memory. CHIME always needs at least 1 RDMA/lookup.
#           Range scans: DEX sequential layout gives 8× lower P50.
#
# COMPARABLE ARRANGEMENT:
#   - 256 MB cache for BOTH systems (DEX runtime, CHIME compile-time)
#   - Same key space: 10M, threads: 30, mix: 70% read + 30% range, ops: 10M
#
# HOW TO RUN: Start node0 FIRST; node1 within ~60s of each memcached reset.
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
READ_RATIO=70; INSERT_RATIO=0; UPDATE_RATIO=0; DELETE_RATIO=0; RANGE_RATIO=30
RANGE_SIZE=100
CACHE_MB=256; CHIME_CACHE=256
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

memc_cmd() { printf "%s\r\nquit\r\n" "$1" | timeout 3 nc -w 3 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null || true; }

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
    memc_cmd "flush_all"; sleep 1
    printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | timeout 3 nc -w 3 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | tr -d '\r\n' | grep -q "STORED" || true
    printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | timeout 3 nc -w 3 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | tr -d '\r\n' | grep -q "STORED" || true
    echo ">>> [memc] serverNum=0, clientNum=0 set."
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
    local free; free=$(grep HugePages_Free /proc/meminfo | awk '{print $2}')
    echo ">>> [hugepages] Free: $free"
}

next_iter() {
    ITERATION=$((ITERATION + 1))
    start_memcached; set_iter "$ITERATION"
}

# ════════════════════════════════
# PHASE 1: Build both systems once
# ════════════════════════════════
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

# ════════════════════════════════
# PHASE 2: DEX skew sweep (memory server)
# ════════════════════════════════
echo ""; echo "════ PHASE 2: DEX Skew Sweep | cache=${CACHE_MB}MB | 70/30 mix ════"

for config in "${SKEW_CONFIGS[@]}"; do
    read -r UNIFORM ZIPF_THETA LABEL <<< "$config"
    echo ""; echo "─── DEX $LABEL ───"
    cleanup; hugepages; next_iter

    echo ">>> Running DEX memory server (iter=$ITERATION)..."
    cd "$DEX_BUILD"
    sudo ./newbench_latency \
        $NODE_COUNT $READ_RATIO $INSERT_RATIO $UPDATE_RATIO \
        $DELETE_RATIO $RANGE_RATIO $THREADS $MEM_THREADS \
        $CACHE_MB $UNIFORM $ZIPF_THETA $BULK_LOAD_M $WARMUP_M $RUN_M \
        $CHECK $TIME_BASED $EARLY_STOP $INDEX $RPC_RATE $ADMIT_RATE \
        $AUTO_TUNE $MAX_THREAD \
        2>&1 | tee "/tmp/expB_dex_${LABEL}_node0.log"

    echo ">>> DEX $LABEL memory done. Sleeping 8s..."; sleep 8
done

# ════════════════════════════════
# PHASE 3: CHIME skew sweep (memory server)
# ════════════════════════════════
echo ""; echo "════ PHASE 3: CHIME Skew Sweep | cache=${CHIME_CACHE}MB | 70/30 mix ════"

for config in "${SKEW_CONFIGS[@]}"; do
    read -r UNIFORM ZIPF_THETA LABEL <<< "$config"
    echo ""; echo "─── CHIME $LABEL ───"
    cleanup; hugepages; next_iter

    echo ">>> Running CHIME memory server (iter=$ITERATION)..."
    sudo /tmp/latency_bench_b256 \
        $NODE_COUNT $THREADS $READ_RATIO $RANGE_RATIO \
        $((RUN_M * 1000000)) $RANGE_SIZE $ZIPF_THETA $UNIFORM $BULK_LOAD_M \
        2>&1 | tee "/tmp/expB_chime_${LABEL}_node0.log"

    echo ">>> CHIME $LABEL memory done. Sleeping 8s..."; sleep 8
done

echo ""; echo "════ EXP B Node 0 COMPLETE ════"
