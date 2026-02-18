#!/bin/bash
###############################################################################
# QW2: Original CHIME Zipfian Skew Sweep — Node 0 (Memory Node + Memcached Host)
#
# This runs the ORIGINAL CHIME (SkipList cache, memory-side bitmap) as baseline.
# Compare against qw2_coherent_chime_* scripts which run the new Coherent Trie version.
#
# Sweeps: Uniform, Zipfian θ = 0.6, 0.8, 0.9, 0.99
# Workload: 100% Lookup (stress test cache performance)
# Latency: 500ns buckets, per-op
#
# USAGE: Run this FIRST on Node 0, then start node1 script on Node 1.
#        This script runs ALL skew points sequentially. Node 1 must also
#        run all points in the same order.
#
# NOTE: CHIME node 0 is the MEMORY node (MEMORY_NODE_NUM=1 in Common.h).
#       It does NOT run the workload; only provides memory and coordination.
#       Latency files are saved by Node 1 (the compute node).
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHIME_DIR="$SCRIPT_DIR/../../CHIME"
MEMC_IP=$(head -1 "$CHIME_DIR/memcached.conf")
MEMC_PORT=$(sed -n '2p' "$CHIME_DIR/memcached.conf")

# ===================== SHARED CONFIGURATION (must match node1!) =====================
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

# Iteration counter for cross-node synchronization
ITERATION=0

# ===================== HELPER: flush & reset memcached =====================
flush_and_reset_memcached() {
    echo ">>> [memcached] Killing any existing memcached..."
    sudo pkill -9 memcached 2>/dev/null || true
    sleep 2

    echo ">>> [memcached] Starting fresh memcached on 0.0.0.0:${MEMC_PORT}..."
    sudo memcached -u root -l 0.0.0.0 -p "$MEMC_PORT" -c 10000 -d
    sleep 2

    echo ">>> [memcached] Flushing all keys (flush_all)..."
    printf "flush_all\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null || true
    sleep 1

    echo ">>> [memcached] Setting serverNum=0, clientNum=0..."
    printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
    printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
    sleep 1

    # Verify memcached
    local reply
    reply=$(printf "get serverNum\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | head -1)
    if [[ "$reply" == *"VALUE"* ]]; then
        echo ">>> [memcached] Verified OK — serverNum key present"
    else
        echo ">>> [memcached] WARNING: could not verify serverNum (reply: $reply)"
        echo ">>> Retrying memcached start..."
        sudo pkill -9 memcached 2>/dev/null || true
        sleep 2
        sudo memcached -u root -l 0.0.0.0 -p "$MEMC_PORT" -c 10000 -d
        sleep 3
        printf "flush_all\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null || true
        printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
        printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
        sleep 1
    fi

    # Set iteration sentinel
    ITERATION=$((ITERATION + 1))
    local iter_len=${#ITERATION}
    printf "set qw2_orig_iter 0 0 %d\r\n%s\r\nquit\r\n" "$iter_len" "$ITERATION" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
    echo ">>> [memcached] Set qw2_orig_iter=$ITERATION (node1 sync sentinel)"
}

# ===================== HELPER: clean previous run state =====================
cleanup_previous_run() {
    echo ">>> [cleanup] Killing stale processes..."
    sudo pkill -9 latency_bench 2>/dev/null || true
    sudo pkill -9 chime_bench 2>/dev/null || true
    sleep 1

    echo ">>> [cleanup] Clearing /dev/shm RDMA artifacts..."
    sudo rm -f /dev/shm/dsm_* /dev/shm/rdma_* 2>/dev/null || true
}

# ===================== BUILD (Original CHIME without coherent tree) =====================
echo ">>> Building ORIGINAL CHIME latency_bench..."
cd "$CHIME_DIR"
rm -rf build && mkdir build && cd build
# NO coherent tree flags — this is the baseline
cmake .. -DSHORT_TEST_EPOCH=ON > /dev/null 2>&1
make -j$(nproc) latency_bench
echo ">>> Build complete (ORIGINAL CHIME)."

# ===================== SWEEP =====================
echo ""
echo "============================================================"
echo "  QW2: ORIGINAL CHIME Zipfian Skew Sweep — Node 0 (Memory)"
echo "  Configs: uniform, zipf_0.6, zipf_0.8, zipf_0.9, zipf_0.99"
echo "  NOTE: This node provides memory only. Latency saved on Node 1."
echo "============================================================"

for config in "${SKEW_CONFIGS[@]}"; do
    read -r UNIFORM ZIPF_THETA LABEL <<< "$config"
    
    echo ""
    echo "============================================================"
    echo "  ORIGINAL CHIME Memory — $LABEL (uniform=$UNIFORM, theta=$ZIPF_THETA)"
    echo "============================================================"
    
    cleanup_previous_run

    # Hugepages
    echo ">>> [hugepages] Setting 36864 pages..."
    echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
    ulimit -l unlimited 2>/dev/null || true
    HP_FREE=$(grep HugePages_Free /proc/meminfo | awk '{print $2}')
    echo ">>> [hugepages] Free: $HP_FREE"

    flush_and_reset_memcached
    
    # Run memory server
    cd "$CHIME_DIR/build"
    echo ">>> Starting ORIGINAL CHIME memory server for $LABEL ... waiting for Node 1."
    
    sudo ./latency_bench \
        ${NODE_COUNT} ${THREAD_COUNT} ${READ_RATIO} ${RANGE_RATIO} \
        ${TOTAL_OPS} ${RANGE_SIZE} ${ZIPF_THETA} ${UNIFORM} \
        2>&1 | tee "/tmp/qw2_orig_node0_${LABEL}_stdout.log"
    
    echo ">>> $LABEL memory server done. Sleeping 5s before next..."
    sleep 5
done

echo ""
echo "============================================================"
echo "  QW2: ORIGINAL CHIME SWEEP COMPLETE — Node 0 (Memory)"
echo "  Latency results are on Node 1."
echo "============================================================"
