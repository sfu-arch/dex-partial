#!/bin/bash
###############################################################################
# QW1: CHIME Zipfian Skew Sweep — Node 1 (Compute Node)
#
# MUST run the SAME skew points in the SAME order as node0.
# Start this AFTER node0 is waiting for connection.
#
# NOTE: CHIME node 1 is the COMPUTE node (my_node >= MEMORY_NODE_NUM).
#       It runs the full benchmark and saves latency histograms.
#       Results are collected on THIS node.
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHIME_DIR="$SCRIPT_DIR/../../CHIME"
RESULTS_DIR="$SCRIPT_DIR/results/chime"
MEMC_IP=$(head -1 "$CHIME_DIR/memcached.conf")
MEMC_PORT=$(sed -n '2p' "$CHIME_DIR/memcached.conf")
mkdir -p "$RESULTS_DIR"

# ===================== SHARED CONFIGURATION (must match node0 & DEX!) =====================
NODE_COUNT=2
THREAD_COUNT=16
READ_RATIO=70
RANGE_RATIO=30
TOTAL_OPS=5000000     # 5M ops = matches DEX's RUN_M=5
RANGE_SIZE=100        # matches DEX count-based scan(100)

SKEW_CONFIGS=(
    "1 0.0   uniform"
    "0 0.6   zipf_0.6"
    "0 0.8   zipf_0.8"
    "0 0.9   zipf_0.9"
    "0 0.99  zipf_0.99"
)

# ===================== HELPER: wait for memcached to be fresh =====================
wait_for_fresh_memcached() {
    echo ">>> [memcached] Waiting for fresh memcached at $MEMC_IP:$MEMC_PORT..."
    local attempts=0
    local max_attempts=60
    while [ $attempts -lt $max_attempts ]; do
        if nc -z -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null; then
            # Connection OK — verify serverNum is 0 (freshly reset by node0)
            local val
            val=$(printf "get serverNum\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | grep -A1 "^VALUE" | tail -1 | tr -d '\r')
            if [ "$val" = "0" ]; then
                echo ">>> [memcached] OK — connected and serverNum=0 (fresh state)"
                return 0
            fi
            echo ">>> [memcached] Connected but serverNum='$val' (waiting for node0 to reset)..."
        fi
        attempts=$((attempts + 1))
        echo ">>> [memcached] Waiting... ($attempts/$max_attempts)"
        sleep 2
    done
    echo ">>> [memcached] WARNING: Timed out waiting for fresh memcached. Proceeding anyway."
    return 1
}

# ===================== HELPER: cleanup =====================
cleanup_previous_run() {
    echo ">>> [cleanup] Killing stale processes..."
    sudo pkill -9 latency_bench 2>/dev/null || true
    sleep 1

    echo ">>> [cleanup] Clearing /dev/shm RDMA artifacts..."
    sudo rm -f /dev/shm/dsm_* /dev/shm/rdma_* 2>/dev/null || true

    # Remove stale latency files from previous iteration
    cd "$CHIME_DIR/build" 2>/dev/null || true
    rm -f chime_read_latency.dat chime_range_latency.dat 2>/dev/null || true
}

# ===================== BUILD =====================
echo ">>> Building CHIME latency_bench..."
cd "$CHIME_DIR"
rm -rf build && mkdir build && cd build
cmake .. -DSHORT_TEST_EPOCH=ON > /dev/null 2>&1
make -j$(nproc) latency_bench
echo ">>> Build complete."

# ===================== SWEEP =====================
echo ""
echo "============================================================"
echo "  CHIME QW1 Zipfian Skew Sweep — Node 1 (Compute)"
echo "  Configs: uniform, zipf_0.6, zipf_0.8, zipf_0.9, zipf_0.99"
echo "============================================================"

for config in "${SKEW_CONFIGS[@]}"; do
    read -r UNIFORM ZIPF_THETA LABEL <<< "$config"
    
    echo ""
    echo "============================================================"
    echo "  CHIME QW1 Compute — $LABEL (uniform=$UNIFORM, theta=$ZIPF_THETA)"
    echo "============================================================"
    
    # --- Full cleanup ---
    cleanup_previous_run

    # --- Hugepages ---
    echo ">>> [hugepages] Setting 36864 pages..."
    echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
    ulimit -l unlimited 2>/dev/null || true
    HP_FREE=$(grep HugePages_Free /proc/meminfo | awk '{print $2}')
    echo ">>> [hugepages] Free: $HP_FREE"
    
    # --- Wait for node0 to reset memcached ---
    wait_for_fresh_memcached
    
    # --- Run benchmark (this node SAVES latency files) ---
    cd "$CHIME_DIR/build"
    echo ">>> Running CHIME compute for $LABEL ..."
    echo ">>> Config: ${READ_RATIO}% read + ${RANGE_RATIO}% range, ${TOTAL_OPS} ops, range_size=${RANGE_SIZE}"
    echo ""
    
    ./latency_bench \
        ${NODE_COUNT} ${THREAD_COUNT} ${READ_RATIO} ${RANGE_RATIO} \
        ${TOTAL_OPS} ${RANGE_SIZE} ${ZIPF_THETA} ${UNIFORM} \
        2>&1 | tee "$RESULTS_DIR/chime_${LABEL}_stdout.log"
    
    # --- Collect results (latency files saved by this compute node) ---
    for f in chime_read_latency.dat chime_range_latency.dat; do
        if [ -f "$f" ]; then
            cp "$f" "$RESULTS_DIR/chime_${LABEL}_${f}"
            echo ">>> Saved $RESULTS_DIR/chime_${LABEL}_${f}"
        else
            echo ">>> WARNING: $f not found after $LABEL run!"
        fi
    done
    
    echo ">>> $LABEL compute complete. Sleeping 5s before next..."
    sleep 5
done

echo ""
echo "============================================================"
echo "  CHIME QW1 SWEEP COMPLETE — Node 1 (Compute)"
echo "  Results in: $RESULTS_DIR"
echo "============================================================"
ls -la "$RESULTS_DIR"
