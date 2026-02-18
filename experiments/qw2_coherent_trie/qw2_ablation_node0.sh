#!/bin/bash
###############################################################################
# QW2: Feature Ablation Study — Node 0 (Memory Node)
#
# Tests individual features of Coherent Trie CHIME:
#   1. Original CHIME (baseline)
#   2. Trie Cache only (no bitmap cache, no coherence)
#   3. Bitmap Cache only (no trie cache, no coherence)
#   4. Trie + Bitmap (no coherence)
#   5. Full Coherent Trie (trie + bitmap + lazy coherence)
#   6. Full Coherent Trie with Eager Coherence
#
# This helps understand the contribution of each component.
#
# USAGE: Run on Node 0 first, then Node 1.
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHIME_DIR="$SCRIPT_DIR/../../CHIME_Cache_Coherent_trie"
RESULTS_DIR="$SCRIPT_DIR/results/ablation"
MEMC_IP=$(head -1 "$CHIME_DIR/memcached.conf")
MEMC_PORT=$(sed -n '2p' "$CHIME_DIR/memcached.conf")
mkdir -p "$RESULTS_DIR"

# Fixed workload: zipf 0.99 (high skew to stress cache)
NODE_COUNT=2
THREAD_COUNT=30
READ_RATIO=100
RANGE_RATIO=0
TOTAL_OPS=30000000
RANGE_SIZE=100
ZIPF_THETA=0.99
UNIFORM=0

# Feature configurations to test
# Format: "LABEL USE_COHERENT_TREE USE_TRIE_CACHE USE_BITMAP_CACHE USE_LAZY_COHERENCE"
FEATURE_CONFIGS=(
    "baseline          OFF OFF OFF OFF"
    "trie_only         ON  ON  OFF OFF"
    "bitmap_only       ON  OFF ON  OFF"
    "trie_bitmap       ON  ON  ON  OFF"
    "full_lazy         ON  ON  ON  ON"
    "full_eager        ON  ON  ON  OFF"
)

ITERATION=0

# ===================== HELPERS =====================
flush_and_reset_memcached() {
    sudo pkill -9 memcached 2>/dev/null || true
    sleep 2
    sudo memcached -u root -l 0.0.0.0 -p "$MEMC_PORT" -c 10000 -d
    sleep 2
    printf "flush_all\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null || true
    printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
    printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
    sleep 1
    
    ITERATION=$((ITERATION + 1))
    local iter_len=${#ITERATION}
    printf "set qw2_abl_iter 0 0 %d\r\n%s\r\nquit\r\n" "$iter_len" "$ITERATION" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
    echo ">>> [memcached] Set qw2_abl_iter=$ITERATION"
}

cleanup_previous_run() {
    sudo pkill -9 latency_bench 2>/dev/null || true
    sudo pkill -9 chime_bench 2>/dev/null || true
    sleep 1
    sudo rm -f /dev/shm/dsm_* /dev/shm/rdma_* 2>/dev/null || true
}

# ===================== SWEEP =====================
echo ""
echo "============================================================"
echo "  QW2 Ablation Study — Node 0"
echo "  Testing: baseline, trie_only, bitmap_only, trie_bitmap,"
echo "           full_lazy, full_eager"
echo "  Workload: Zipf θ=0.99"
echo "============================================================"

for config in "${FEATURE_CONFIGS[@]}"; do
    read -r LABEL COHERENT TRIE BITMAP LAZY <<< "$config"
    
    echo ""
    echo "============================================================"
    echo "  Ablation: $LABEL"
    echo "  USE_COHERENT_TREE=$COHERENT, USE_TRIE_CACHE=$TRIE"
    echo "  USE_BITMAP_CACHE=$BITMAP, USE_LAZY_COHERENCE=$LAZY"
    echo "============================================================"
    
    cleanup_previous_run
    
    # Build with specific features
    echo ">>> Building with features: $LABEL..."
    cd "$CHIME_DIR"
    rm -rf build && mkdir build && cd build
    
    cmake .. \
        -DSHORT_TEST_EPOCH=ON \
        -DUSE_COHERENT_TREE=${COHERENT} \
        -DUSE_TRIE_CACHE=${TRIE} \
        -DUSE_BITMAP_CACHE=${BITMAP} \
        -DUSE_LAZY_COHERENCE=${LAZY} \
        > /dev/null 2>&1
    make -j$(nproc) latency_bench
    echo ">>> Build complete."
    
    # Hugepages
    echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
    ulimit -l unlimited 2>/dev/null || true
    
    flush_and_reset_memcached
    
    echo ">>> Starting memory server for $LABEL..."
    
    sudo ./latency_bench \
        ${NODE_COUNT} ${THREAD_COUNT} ${READ_RATIO} ${RANGE_RATIO} \
        ${TOTAL_OPS} ${RANGE_SIZE} ${ZIPF_THETA} ${UNIFORM} \
        2>&1 | tee "/tmp/qw2_abl_node0_${LABEL}_stdout.log"
    
    echo ">>> $LABEL done. Sleeping 5s..."
    sleep 5
done

echo ""
echo "============================================================"
echo "  QW2 Ablation Study COMPLETE — Node 0"
echo "============================================================"
