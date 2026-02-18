#!/bin/bash
###############################################################################
# QW2: COHERENT TRIE CHIME Zipfian Skew Sweep — Node 1 (Compute Node)
#
# This runs the NEW Coherent Trie CHIME with:
#   - ART-style trie cache (replacing SkipList)
#   - Compute-side bitmap caching
#   - MESI-like cache coherence protocol
#
# MUST run the SAME skew points in the SAME order as node0.
# Start this AFTER node0 is waiting for connection.
#
# NOTE: CHIME node 1 is the COMPUTE node. Latency results saved here.
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHIME_DIR="$SCRIPT_DIR/../../CHIME_Cache_Coherent_trie"
RESULTS_DIR="$SCRIPT_DIR/results/chime_coherent"
MEMC_IP=$(head -1 "$CHIME_DIR/memcached.conf")
MEMC_PORT=$(sed -n '2p' "$CHIME_DIR/memcached.conf")
mkdir -p "$RESULTS_DIR"

# ===================== SHARED CONFIGURATION (must match node0!) =====================
NODE_COUNT=2
THREAD_COUNT=30
READ_RATIO=100
RANGE_RATIO=0
TOTAL_OPS=30000000    # 30M ops
RANGE_SIZE=100

SKEW_CONFIGS=(
    "1 0.0   uniform"
    "0 0.6   zipf_0.6"
    "0 0.8   zipf_0.8"
    "0 0.9   zipf_0.9"
    "0 0.99  zipf_0.99"
)

# Build options (must match node0)
USE_COHERENT_TREE="ON"
USE_TRIE_CACHE="ON"
USE_BITMAP_CACHE="ON"
USE_LAZY_COHERENCE="ON"

ITERATION=0

# ===================== HELPER: wait for node0 =====================
wait_for_iteration() {
    local expected_iter="$1"
    echo ">>> [sync] Waiting for node0 to signal iteration $expected_iter..."
    local attempts=0
    local max_attempts=120
    while [ $attempts -lt $max_attempts ]; do
        if nc -z -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null; then
            local val
            val=$(printf "get qw2_coh_iter\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | grep -A1 "^VALUE" | tail -1 | tr -d '\r')
            if [ "$val" = "$expected_iter" ]; then
                echo ">>> [sync] OK — qw2_coh_iter=$expected_iter"
                echo ">>> [sync] Waiting for node0 binary to register..."
                local reg_attempts=0
                while [ $reg_attempts -lt 30 ]; do
                    local sn
                    sn=$(printf "get serverNum\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | grep -A1 "^VALUE" | tail -1 | tr -d '\r')
                    if [ -n "$sn" ] && [ "$sn" -ge 1 ] 2>/dev/null; then
                        echo ">>> [sync] node0 registered (serverNum=$sn)"
                        sleep 1
                        return 0
                    fi
                    reg_attempts=$((reg_attempts + 1))
                    sleep 1
                done
                echo ">>> [sync] WARNING: serverNum never reached 1, starting anyway."
                return 0
            fi
            [ -n "$val" ] && echo ">>> [sync] qw2_coh_iter='$val' (waiting for $expected_iter)..."
        fi
        attempts=$((attempts + 1))
        [ $((attempts % 10)) -eq 0 ] && echo ">>> [sync] Still waiting... ($attempts/$max_attempts)"
        sleep 2
    done
    echo ">>> [sync] ERROR: Timed out!"
    return 1
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

    cd "$CHIME_DIR/build" 2>/dev/null || true
    rm -f chime_read_latency.dat chime_range_latency.dat 2>/dev/null || true
    rm -f coherent_read_latency.dat coherent_range_latency.dat 2>/dev/null || true
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
echo "  QW2: COHERENT TRIE CHIME Zipfian Sweep — Node 1 (Compute)"
echo "  Features: Trie Cache, Bitmap Cache, Lazy Coherence"
echo "  Configs: uniform, zipf_0.6, zipf_0.8, zipf_0.9, zipf_0.99"
echo "============================================================"

for config in "${SKEW_CONFIGS[@]}"; do
    read -r UNIFORM ZIPF_THETA LABEL <<< "$config"
    
    echo ""
    echo "============================================================"
    echo "  COHERENT TRIE Compute — $LABEL (uniform=$UNIFORM, theta=$ZIPF_THETA)"
    echo "============================================================"
    
    cleanup_previous_run

    echo ">>> [hugepages] Setting 36864 pages..."
    echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
    ulimit -l unlimited 2>/dev/null || true
    HP_FREE=$(grep HugePages_Free /proc/meminfo | awk '{print $2}')
    echo ">>> [hugepages] Free: $HP_FREE"
    
    ITERATION=$((ITERATION + 1))
    wait_for_iteration "$ITERATION"
    
    cd "$CHIME_DIR/build"
    echo ">>> Running COHERENT TRIE compute for $LABEL..."
    echo ">>> Config: ${READ_RATIO}% read + ${RANGE_RATIO}% range, ${TOTAL_OPS} ops"
    
    sudo ./latency_bench \
        ${NODE_COUNT} ${THREAD_COUNT} ${READ_RATIO} ${RANGE_RATIO} \
        ${TOTAL_OPS} ${RANGE_SIZE} ${ZIPF_THETA} ${UNIFORM} \
        2>&1 | tee "$RESULTS_DIR/coherent_${LABEL}_stdout.log"
    
    # Collect latency results (may have different prefixes)
    for f in chime_read_latency.dat chime_range_latency.dat \
             coherent_read_latency.dat coherent_range_latency.dat; do
        if [ -f "$f" ]; then
            cp "$f" "$RESULTS_DIR/coherent_${LABEL}_${f}"
            echo ">>> Saved $RESULTS_DIR/coherent_${LABEL}_${f}"
        fi
    done
    
    echo ">>> $LABEL compute complete. Sleeping 5s..."
    sleep 5
done

echo ""
echo "============================================================"
echo "  QW2: COHERENT TRIE CHIME SWEEP COMPLETE — Node 1"
echo "  Results in: $RESULTS_DIR"
echo "============================================================"
ls -la "$RESULTS_DIR"
