#!/bin/bash
###############################################################################
# Experiment A: Cache Size Crossover — Node 0 (Memory Server + Memcached Host)
#
# QUESTION: When is DEX worse than CHIME?
# ANSWER:   Small cache + uniform workload. CHIME caches only metadata (~36 MB).
#           DEX needs ~170 MB for 10M keys before performance becomes viable.
#
# COMPARABLE ARRANGEMENT (both DEX and CHIME):
#   - 10M keys, 30 threads, 100% point reads, uniform distribution, 10M ops
#
# DEX cache sweep (runtime arg):   64, 128, 256, 512 MB
# CHIME cache sweep (compile-time): 32, 64, 100 MB  -- rebuilt once upfront
#
# Node 0 role: memory server (runs all binaries but exits after remote connects)
# Node 1 role: compute client (runs workload, saves .dat files)
#
# HOW TO RUN:
#   Start node0 FIRST. Start node1 within ~60s of each memcached reset.
###############################################################################
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEX_DIR="$SCRIPT_DIR/../../dex"
CHIME_DIR="$SCRIPT_DIR/../../CHIME"
DEX_BUILD="$DEX_DIR/build"
CHIME_BUILD="$CHIME_DIR/build"

# Use CHIME's memcached.conf (both experiments run through same cluster)
MEMC_IP=$(head -1 "$DEX_DIR/memcached.conf")
MEMC_PORT=$(sed -n '2p' "$DEX_DIR/memcached.conf")

echo ">>> Memcached: ${MEMC_IP}:${MEMC_PORT}"

# ── Shared workload config (MUST match node1 exactly) ─────────────────────────
NODE_COUNT=2
THREADS=30
MEM_THREADS=4
BULK_LOAD_M=10
WARMUP_M=1
RUN_M=10        # 10M ops per data point
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
INDEX=0         # 0 = DEX/Sherman
RPC_RATE=0.0
ADMIT_RATE=1.0
AUTO_TUNE=0
MAX_THREAD=30
RANGE_SIZE=1

DEX_CACHES=(64 128 256 512)
CHIME_CACHES=(32 64 100)

ITERATION=0

# ── Helpers ───────────────────────────────────────────────────────────────────
memc_cmd() {
    printf "%s\r\nquit\r\n" "$1" | nc -q1 -w 3 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null || true
}

start_memcached() {
    echo ">>> [memc] Killing old memcached..."
    sudo pkill -9 memcached 2>/dev/null || true
    sleep 3

    echo ">>> [memc] Starting fresh memcached on ${MEMC_IP}:${MEMC_PORT}..."
    sudo memcached -u root -l 0.0.0.0 -p "$MEMC_PORT" -c 10000 -m 256 -d
    sleep 2

    # Wait until memcached is accepting connections (up to 15s)
    local retries=0
    while true; do
        local reply
        reply=$(printf "set ping 0 0 2\r\nok\r\nquit\r\n" | nc -q1 -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | tr -d '\r\n' || true)
        if [[ "$reply" == *"STORED"* ]]; then
            echo ">>> [memc] Memcached is up."
            break
        fi
        retries=$((retries + 1))
        if [ $retries -ge 15 ]; then
            echo "ERROR: memcached did not come up after 15s"
            exit 1
        fi
        echo ">>> [memc] Waiting for memcached... ($retries)"
        sleep 1
    done

    # flush all old state
    memc_cmd "flush_all"
    sleep 1
}

set_iter() {
    local val=$1
    local len=${#val}
    printf "set exp_iter 0 0 %d\r\n%s\r\nquit\r\n" "$len" "$val" \
        | nc -q1 -w 3 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | tr -d '\r\n' | grep -q "STORED" \
        && echo ">>> [memc] exp_iter=$val set." \
        || echo ">>> [memc] WARNING: failed to set exp_iter=$val"
}

reset_coord_keys() {
    # DEX and CHIME DSM both look for serverNum and clientNum to coordinate node roles
    printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -q1 -w 3 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | tr -d '\r\n' | grep -q "STORED" || true
    printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -q1 -w 3 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | tr -d '\r\n' | grep -q "STORED" || true
    echo ">>> [memc] serverNum=0, clientNum=0 reset."
}

cleanup() {
    echo ">>> [cleanup] Killing stale benchmark processes..."
    sudo pkill -9 newbench_latency 2>/dev/null || true
    sudo pkill -9 latency_bench    2>/dev/null || true
    sleep 2
    echo ">>> [cleanup] Removing /dev/shm RDMA artifacts..."
    sudo rm -f /dev/shm/dsm_* /dev/shm/rdma_* 2>/dev/null || true
    sleep 1
}

hugepages() {
    echo ">>> [hugepages] Allocating 36864 huge pages..."
    echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
    ulimit -l unlimited 2>/dev/null || true
    local free
    free=$(grep HugePages_Free /proc/meminfo | awk '{print $2}')
    echo ">>> [hugepages] Free: $free"
    if [ "$free" -lt 1000 ]; then
        echo "WARNING: Only $free huge pages free — may be insufficient"
    fi
}

next_iter() {
    ITERATION=$((ITERATION + 1))
    # Full memcached reset before each run (prevents stale keys hanging DSM init)
    start_memcached
    reset_coord_keys
    set_iter "$ITERATION"
}

# ═══════════════════════════════════════════════════════════════
# PHASE 1: Build everything upfront (before any sync with node1)
# ═══════════════════════════════════════════════════════════════
echo ""; echo "════════════════════════════════"
echo "  PHASE 1: Building all binaries"
echo "════════════════════════════════"

# Build DEX (one binary, cache is a runtime arg)
echo ">>> Building DEX newbench_latency..."
cd "$DEX_DIR"; mkdir -p build; cd build
cmake .. -DCMAKE_BUILD_TYPE=Release > /dev/null 2>&1
make -j$(nproc) newbench_latency 2>&1 | tail -3
echo ">>> DEX built: $DEX_BUILD/newbench_latency"

# Build CHIME for each cache size (cache is compile-time: kIndexCacheSize)
CHIME_COMMON="$CHIME_DIR/include/Common.h"
for CHIME_CACHE in "${CHIME_CACHES[@]}"; do
    echo ">>> Building CHIME with kIndexCacheSize=${CHIME_CACHE}..."
    sed -i "s/constexpr int kIndexCacheSize\s*=\s*[0-9]*/constexpr int kIndexCacheSize  = ${CHIME_CACHE}/" "$CHIME_COMMON"
    cd "$CHIME_DIR"; rm -rf build; mkdir build; cd build
    cmake .. -DSHORT_TEST_EPOCH=ON > /dev/null 2>&1
    make -j$(nproc) latency_bench 2>&1 | tail -2
    # Save binary with a name encoding the cache size
    cp "$CHIME_BUILD/latency_bench" "/tmp/latency_bench_cache${CHIME_CACHE}"
    echo ">>> Saved /tmp/latency_bench_cache${CHIME_CACHE}"
done
# Restore CHIME default
sed -i "s/constexpr int kIndexCacheSize\s*=\s*[0-9]*/constexpr int kIndexCacheSize  = 100/" "$CHIME_COMMON"
echo ">>> All CHIME variants built. kIndexCacheSize restored to 100."

# ═══════════════════════════════════════════════════════════════
# PHASE 2: DEX cache sweep (memory server role)
# ═══════════════════════════════════════════════════════════════
echo ""; echo "════════════════════════════════════════════════════════"
echo "  PHASE 2: DEX Cache Sweep (memory server)"
echo "  Cache: ${DEX_CACHES[*]} MB | uniform | 100% reads | 10M keys"
echo "════════════════════════════════════════════════════════"

for CACHE_MB in "${DEX_CACHES[@]}"; do
    echo ""; echo "─── DEX cache=${CACHE_MB}MB ───"
    cleanup; hugepages; next_iter

    echo ">>> Running DEX memory server (iter=$ITERATION, cache=${CACHE_MB}MB)..."
    cd "$DEX_BUILD"
    sudo ./newbench_latency \
        $NODE_COUNT $READ_RATIO $INSERT_RATIO $UPDATE_RATIO \
        $DELETE_RATIO $RANGE_RATIO $THREADS $MEM_THREADS \
        $CACHE_MB $UNIFORM $ZIPF_THETA $BULK_LOAD_M $WARMUP_M $RUN_M \
        $CHECK $TIME_BASED $EARLY_STOP $INDEX $RPC_RATE $ADMIT_RATE \
        $AUTO_TUNE $MAX_THREAD \
        2>&1 | tee "/tmp/expA_dex_cache${CACHE_MB}mb_node0.log"

    echo ">>> DEX cache=${CACHE_MB}MB memory done. Sleeping 8s..."; sleep 8
done

# ═══════════════════════════════════════════════════════════════
# PHASE 3: CHIME cache sweep (memory server role)
# ═══════════════════════════════════════════════════════════════
echo ""; echo "════════════════════════════════════════════════════════"
echo "  PHASE 3: CHIME Cache Sweep (memory server)"
echo "  Cache: ${CHIME_CACHES[*]} MB | uniform | 100% reads | 10M keys"
echo "════════════════════════════════════════════════════════"

for CHIME_CACHE in "${CHIME_CACHES[@]}"; do
    echo ""; echo "─── CHIME cache=${CHIME_CACHE}MB ───"
    cleanup; hugepages; next_iter

    echo ">>> Running CHIME memory server (iter=$ITERATION, cache=${CHIME_CACHE}MB)..."
    sudo /tmp/latency_bench_cache${CHIME_CACHE} \
        $NODE_COUNT $THREADS $READ_RATIO $RANGE_RATIO \
        $((RUN_M * 1000000)) $RANGE_SIZE $ZIPF_THETA $UNIFORM $BULK_LOAD_M \
        2>&1 | tee "/tmp/expA_chime_cache${CHIME_CACHE}mb_node0.log"

    echo ">>> CHIME cache=${CHIME_CACHE}MB memory done. Sleeping 8s..."; sleep 8
done

echo ""; echo "════════════════════════════════"
echo "  EXP A Node 0 COMPLETE"
echo "  Compute results are on Node 1."
echo "════════════════════════════════"
