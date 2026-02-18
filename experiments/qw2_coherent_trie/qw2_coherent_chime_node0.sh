#!/bin/bash
###############################################################################
# QW2: COHERENT TRIE CHIME Zipfian Skew Sweep — Node 0 (Memory Node)
#
# This runs the NEW Coherent Trie CHIME with:
#   - ART-style trie cache (replacing SkipList)
#   - Compute-side bitmap caching
#   - MESI-like cache coherence protocol
#
# Sweeps: Uniform, Zipfian θ = 0.6, 0.8, 0.9, 0.99
# Workload: 100% Lookup (stress test cache performance)
#
# USAGE: Run this FIRST on Node 0, then start node1 script on Node 1.
#        After running both Original and Coherent Trie experiments,
#        use plot_qw2_comparison.py to generate comparison graphs.
#
# NOTE: CHIME node 0 is the MEMORY node (MEMORY_NODE_NUM=1 in Common.h).
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHIME_DIR="$SCRIPT_DIR/../../CHIME_Cache_Coherent_trie"
MEMC_IP=$(head -1 "$CHIME_DIR/memcached.conf")
MEMC_PORT=$(sed -n '2p' "$CHIME_DIR/memcached.conf")

# ===================== SHARED CONFIGURATION (must match node1!) =====================
NODE_COUNT=2
THREAD_COUNT=30
READ_RATIO=100
RANGE_RATIO=0
TOTAL_OPS=30000000    # 30M ops
RANGE_SIZE=100

# Experiment variants: test different coherence modes
# Format: "UNIFORM ZIPF_THETA LABEL COHERENCE_MODE BITMAP_CACHE"
# COHERENCE_MODE: lazy, eager, none
# BITMAP_CACHE: on, off

SKEW_CONFIGS=(
    "1 0.0   uniform"
    "0 0.6   zipf_0.6"
    "0 0.8   zipf_0.8"
    "0 0.9   zipf_0.9"
    "0 0.99  zipf_0.99"
)

# Build options: which features to enable
USE_COHERENT_TREE="ON"
USE_TRIE_CACHE="ON"
USE_BITMAP_CACHE="ON"
USE_LAZY_COHERENCE="ON"

ITERATION=0

# ===================== HELPER: flush & reset memcached =====================
flush_and_reset_memcached() {
    echo ">>> [memcached] Killing any existing memcached..."
    sudo pkill -9 memcached 2>/dev/null || true
    sleep 2

    echo ">>> [memcached] Starting fresh memcached on 0.0.0.0:${MEMC_PORT}..."
    sudo memcached -u root -l 0.0.0.0 -p "$MEMC_PORT" -c 10000 -d
    sleep 2

    echo ">>> [memcached] Flushing all keys..."
    printf "flush_all\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null || true
    sleep 1

    echo ">>> [memcached] Setting serverNum=0, clientNum=0..."
    printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
    printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
    sleep 1

    local reply
    reply=$(printf "get serverNum\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | head -1)
    if [[ "$reply" == *"VALUE"* ]]; then
        echo ">>> [memcached] Verified OK"
    else
        echo ">>> [memcached] WARNING: verification failed, retrying..."
        sudo pkill -9 memcached 2>/dev/null || true
        sleep 2
        sudo memcached -u root -l 0.0.0.0 -p "$MEMC_PORT" -c 10000 -d
        sleep 3
        printf "flush_all\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null || true
        printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
        printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
        sleep 1
    fi

    ITERATION=$((ITERATION + 1))
    local iter_len=${#ITERATION}
    printf "set qw2_coh_iter 0 0 %d\r\n%s\r\nquit\r\n" "$iter_len" "$ITERATION" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
    echo ">>> [memcached] Set qw2_coh_iter=$ITERATION"
}

# ===================== HELPER: cleanup =====================
cleanup_previous_run() {
    echo ">>> [cleanup] Killing stale processes..."
    sudo pkill -9 latency_bench 2>/dev/null || true
    sudo pkill -9 chime_bench 2>/dev/null || true
    sudo pkill -9 coherent_bench 2>/dev/null || true
    sleep 1

    echo ">>> [cleanup] Clearing /dev/shm RDMA artifacts..."
    sudo rm -f /dev/shm/dsm_* /dev/shm/rdma_* 2>/dev/null || true
}

# ===================== BUILD (Coherent Trie CHIME) =====================
echo ">>> Building COHERENT TRIE CHIME..."
echo ">>> Build options:"
echo "    USE_COHERENT_TREE=${USE_COHERENT_TREE}"
echo "    USE_TRIE_CACHE=${USE_TRIE_CACHE}"
echo "    USE_BITMAP_CACHE=${USE_BITMAP_CACHE}"
echo "    USE_LAZY_COHERENCE=${USE_LAZY_COHERENCE}"

cd "$CHIME_DIR"
rm -rf build && mkdir build && cd build
cmake .. \
    -DSHORT_TEST_EPOCH=ON \
    -DUSE_COHERENT_TREE=${USE_COHERENT_TREE} \
    -DUSE_TRIE_CACHE=${USE_TRIE_CACHE} \
    -DUSE_BITMAP_CACHE=${USE_BITMAP_CACHE} \
    -DUSE_LAZY_COHERENCE=${USE_LAZY_COHERENCE} \
    > /dev/null 2>&1
make -j$(nproc) latency_bench
echo ">>> Build complete (COHERENT TRIE CHIME)."

# ===================== SWEEP =====================
echo ""
echo "============================================================"
echo "  QW2: COHERENT TRIE CHIME Zipfian Sweep — Node 0 (Memory)"
echo "  Features: Trie Cache, Bitmap Cache, Lazy Coherence"
echo "  Configs: uniform, zipf_0.6, zipf_0.8, zipf_0.9, zipf_0.99"
echo "============================================================"

for config in "${SKEW_CONFIGS[@]}"; do
    read -r UNIFORM ZIPF_THETA LABEL <<< "$config"
    
    echo ""
    echo "============================================================"
    echo "  COHERENT TRIE Memory — $LABEL (uniform=$UNIFORM, theta=$ZIPF_THETA)"
    echo "============================================================"
    
    cleanup_previous_run

    echo ">>> [hugepages] Setting 36864 pages..."
    echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
    ulimit -l unlimited 2>/dev/null || true
    HP_FREE=$(grep HugePages_Free /proc/meminfo | awk '{print $2}')
    echo ">>> [hugepages] Free: $HP_FREE"

    flush_and_reset_memcached
    
    cd "$CHIME_DIR/build"
    echo ">>> Starting COHERENT TRIE memory server for $LABEL..."
    
    sudo ./latency_bench \
        ${NODE_COUNT} ${THREAD_COUNT} ${READ_RATIO} ${RANGE_RATIO} \
        ${TOTAL_OPS} ${RANGE_SIZE} ${ZIPF_THETA} ${UNIFORM} \
        2>&1 | tee "/tmp/qw2_coh_node0_${LABEL}_stdout.log"
    
    echo ">>> $LABEL memory server done. Sleeping 5s..."
    sleep 5
done

echo ""
echo "============================================================"
echo "  QW2: COHERENT TRIE CHIME SWEEP COMPLETE — Node 0"
echo "============================================================"
