#!/bin/bash
###############################################################################
# QW2: Feature Ablation Study — Node 1 (Compute Node)
#
# Tests individual features of Coherent Trie CHIME.
# MUST run same configs in same order as node0.
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHIME_DIR="$SCRIPT_DIR/../../CHIME_Cache_Coherent_trie"
RESULTS_DIR="$SCRIPT_DIR/results/ablation"
MEMC_IP=$(head -1 "$CHIME_DIR/memcached.conf")
MEMC_PORT=$(sed -n '2p' "$CHIME_DIR/memcached.conf")
mkdir -p "$RESULTS_DIR"

# Fixed workload: zipf 0.99
NODE_COUNT=2
THREAD_COUNT=30
READ_RATIO=100
RANGE_RATIO=0
TOTAL_OPS=30000000
RANGE_SIZE=100
ZIPF_THETA=0.99
UNIFORM=0

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
wait_for_iteration() {
    local expected_iter="$1"
    echo ">>> [sync] Waiting for iteration $expected_iter..."
    local attempts=0
    while [ $attempts -lt 120 ]; do
        if nc -z -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null; then
            local val
            val=$(printf "get qw2_abl_iter\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | grep -A1 "^VALUE" | tail -1 | tr -d '\r')
            if [ "$val" = "$expected_iter" ]; then
                echo ">>> [sync] OK — qw2_abl_iter=$expected_iter"
                local reg_attempts=0
                while [ $reg_attempts -lt 30 ]; do
                    local sn
                    sn=$(printf "get serverNum\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | grep -A1 "^VALUE" | tail -1 | tr -d '\r')
                    if [ -n "$sn" ] && [ "$sn" -ge 1 ] 2>/dev/null; then
                        sleep 1
                        return 0
                    fi
                    reg_attempts=$((reg_attempts + 1))
                    sleep 1
                done
                return 0
            fi
        fi
        attempts=$((attempts + 1))
        [ $((attempts % 10)) -eq 0 ] && echo ">>> [sync] Still waiting... ($attempts/120)"
        sleep 2
    done
    echo ">>> [sync] ERROR: Timed out!"
    return 1
}

cleanup_previous_run() {
    sudo pkill -9 latency_bench 2>/dev/null || true
    sudo pkill -9 chime_bench 2>/dev/null || true
    sleep 1
    sudo rm -f /dev/shm/dsm_* /dev/shm/rdma_* 2>/dev/null || true
    cd "$CHIME_DIR/build" 2>/dev/null || true
    rm -f *_latency.dat 2>/dev/null || true
}

# ===================== SWEEP =====================
echo ""
echo "============================================================"
echo "  QW2 Ablation Study — Node 1 (Compute)"
echo "============================================================"

for config in "${FEATURE_CONFIGS[@]}"; do
    read -r LABEL COHERENT TRIE BITMAP LAZY <<< "$config"
    
    echo ""
    echo "============================================================"
    echo "  Ablation: $LABEL"
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
    
    echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
    ulimit -l unlimited 2>/dev/null || true
    
    ITERATION=$((ITERATION + 1))
    wait_for_iteration "$ITERATION"
    
    echo ">>> Running compute for $LABEL..."
    
    sudo ./latency_bench \
        ${NODE_COUNT} ${THREAD_COUNT} ${READ_RATIO} ${RANGE_RATIO} \
        ${TOTAL_OPS} ${RANGE_SIZE} ${ZIPF_THETA} ${UNIFORM} \
        2>&1 | tee "$RESULTS_DIR/ablation_${LABEL}_stdout.log"
    
    # Collect results
    for f in *_latency.dat; do
        if [ -f "$f" ]; then
            cp "$f" "$RESULTS_DIR/ablation_${LABEL}_${f}"
            echo ">>> Saved $RESULTS_DIR/ablation_${LABEL}_${f}"
        fi
    done
    
    echo ">>> $LABEL done. Sleeping 5s..."
    sleep 5
done

echo ""
echo "============================================================"
echo "  QW2 Ablation Study COMPLETE — Node 1"
echo "  Results in: $RESULTS_DIR"
echo "============================================================"
ls -la "$RESULTS_DIR"
