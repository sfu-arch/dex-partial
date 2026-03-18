#!/bin/bash
###############################################################################
# Experiment C: DART vs CHIME Crossover — Node 0 (Memory Server + Monitor)
#
# QUESTION: When is DART worse than CHIME, and what is DART's best case?
# DART's absolute throughput floor = ~1.67 Mops (fixed RDMA cost, ~4.5 RTTs).
# CHIME degrades at small cache. The crossover: CHIME wins when cache >= ~16 MB.
#
# COMPARABLE ARRANGEMENT:
#   - 2M keys (DART constraint), 30 threads, 100% reads
#   - CHIME cache swept: 4, 16, 32, 64, 100 MB
#   - DART: ~1 MB fixed skip-table cache
#   - Workloads: uniform AND θ=0.99 skew
#
# HOW TO RUN: Start node0 first; node1 within ~60s of each reset.
###############################################################################
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHIME_DIR="$SCRIPT_DIR/../../CHIME"
DART_DIR="$SCRIPT_DIR/../../DART-main"
CHIME_BUILD="$CHIME_DIR/build"
DART_BIN="$DART_DIR/bin"

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
    sudo pkill -9 latency_bench 2>/dev/null || true
    sudo pkill -9 monitor       2>/dev/null || true
    sudo pkill -9 memory        2>/dev/null || true
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

CHIME_COMMON="$CHIME_DIR/include/Common.h"

# ════ PHASE 1: Build ════
echo ""; echo "════ PHASE 1: Build ════"

echo ">>> Building DART..."
cd "$DART_DIR"; mkdir -p build; cd build
cmake .. > /dev/null 2>&1; make -j$(nproc) 2>&1 | tail -3

for CHIME_CACHE in "${CHIME_CACHES[@]}"; do
    echo ">>> Building CHIME cache=${CHIME_CACHE}MB..."
    sed -i "s/constexpr int kIndexCacheSize\s*=\s*[0-9]*/constexpr int kIndexCacheSize  = ${CHIME_CACHE}/" "$CHIME_COMMON"
    cd "$CHIME_DIR"; rm -rf build; mkdir build; cd build
    cmake .. -DSHORT_TEST_EPOCH=ON > /dev/null 2>&1
    make -j$(nproc) latency_bench 2>&1 | tail -2
    cp "$CHIME_BUILD/latency_bench" "/tmp/latency_bench_c${CHIME_CACHE}"
done
sed -i "s/constexpr int kIndexCacheSize\s*=\s*[0-9]*/constexpr int kIndexCacheSize  = 100/" "$CHIME_COMMON"
echo ">>> All builds done."

# ════ PHASE 2: CHIME cache sweep (memory server) ════
echo ""; echo "════ PHASE 2: CHIME cache sweep (memory server) ════"

for CHIME_CACHE in "${CHIME_CACHES[@]}"; do
    for config in "${SKEW_CONFIGS[@]}"; do
        read -r UNIFORM ZIPF_THETA LABEL <<< "$config"
        echo ""; echo "─── CHIME cache=${CHIME_CACHE}MB $LABEL ───"
        cleanup; hugepages; next_iter

        sudo /tmp/latency_bench_c${CHIME_CACHE} \
            $NODE_COUNT $THREADS $READ_RATIO $RANGE_RATIO \
            $((RUN_M * 1000000)) $RANGE_SIZE $ZIPF_THETA $UNIFORM $BULK_LOAD_M \
            2>&1 | tee "/tmp/expC_chime_c${CHIME_CACHE}_${LABEL}_node0.log"

        echo ">>> Done. Sleeping 8s..."; sleep 8
    done
done

# ════ PHASE 3: DART (monitor + memory on node0) ════
echo ""; echo "════ PHASE 3: DART runs (monitor + memory on node0) ════"
echo "NOTE: DART uses its own 3-process architecture."
echo "      node0 runs monitor + memory; node1 runs compute."
echo "      No memcached sync needed for DART (uses its own IPC)."

for config in "${SKEW_CONFIGS[@]}"; do
    read -r UNIFORM ZIPF_THETA LABEL <<< "$config"
    echo ""; echo "─── DART $LABEL ───"

    # For DART, use the exp_iter as a loose sync hint for node1
    ITERATION=$((ITERATION + 1))
    set_iter "$ITERATION"

    cleanup

    cd "$DART_BIN"
    sudo ./monitor &
    MONITOR_PID=$!
    sleep 2
    sudo ./memory &
    MEMORY_PID=$!
    echo ">>> DART monitor(pid=$MONITOR_PID) + memory(pid=$MEMORY_PID) started for $LABEL"
    echo ">>> Waiting for DART to finish..."

    wait $MONITOR_PID 2>/dev/null || true
    wait $MEMORY_PID  2>/dev/null || true

    echo ">>> DART $LABEL done. Sleeping 8s..."; sleep 8
done

echo ""; echo "════ EXP C Node 0 COMPLETE ════"
