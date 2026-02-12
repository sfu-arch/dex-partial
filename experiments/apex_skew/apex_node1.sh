#!/bin/bash
###############################################################################
# APEX Zipfian Skew Sweep — Node 1 (Compute Node)
#
# This script runs on the COMPUTE NODE (10.30.1.6).
# It performs the actual benchmark workload and saves latency results.
#
# MUST run the SAME skew points in the SAME order as node0.
# Start this AFTER node0 is waiting for connection.
#
# USAGE:
#   1. First start node0: ./apex_node0.sh  (on 10.30.1.9)
#   2. Then start this:   ./apex_node1.sh  (on 10.30.1.6)
#
# OUTPUT:
#   results/apex/
#     apex_{label}_apex_read_latency.dat
#     apex_{label}_apex_range_latency.dat
#     apex_{label}_stdout.log
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APEX_DIR="$SCRIPT_DIR/../../apex"
RESULTS_DIR="$SCRIPT_DIR/results/apex"
MEMC_IP=$(head -1 "$APEX_DIR/memcached.conf")
MEMC_PORT=$(sed -n '2p' "$APEX_DIR/memcached.conf")
mkdir -p "$RESULTS_DIR"

# ===================== SHARED CONFIGURATION (must match node0!) =====================
NODE_COUNT=2
THREAD_COUNT=30         # Worker threads on compute node
READ_RATIO=100          # 100% point reads
RANGE_RATIO=0           # 0% range scans
TOTAL_OPS=30000000      # 30M ops (matches DEX RUN_M=30)
RANGE_SIZE=100          # Range scan size

SKEW_CONFIGS=(
    "1 0.0   uniform"
    "0 0.6   zipf_0.6"
    "0 0.8   zipf_0.8"
    "0 0.9   zipf_0.9"
    "0 0.99  zipf_0.99"
)

# Iteration counter — must stay in sync with node0
ITERATION=0

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [APEX-N1] $1"
}

# ===================== Wait for node0 to signal new iteration =====================
wait_for_iteration() {
    local expected_iter="$1"
    log "Waiting for node0 to signal iteration $expected_iter (apex_iter key)..."
    local attempts=0
    local max_attempts=120   # 4 min max

    while [ $attempts -lt $max_attempts ]; do
        if nc -z -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null; then
            local val
            val=$(printf "get apex_iter\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | grep -A1 "^VALUE" | tail -1 | tr -d '\r')
            if [ "$val" = "$expected_iter" ]; then
                log "OK — apex_iter=$expected_iter (node0 ready)"

                # Wait for node0's binary to register (serverNum >= 1)
                log "Waiting for node0 binary to register (serverNum >= 1)..."
                local reg_attempts=0
                while [ $reg_attempts -lt 60 ]; do
                    local sn
                    sn=$(printf "get serverNum\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | grep -A1 "^VALUE" | tail -1 | tr -d '\r')
                    if [ -n "$sn" ] && [ "$sn" -ge 1 ] 2>/dev/null; then
                        log "node0 registered (serverNum=$sn). Starting in 1s..."
                        sleep 1
                        return 0
                    fi
                    reg_attempts=$((reg_attempts + 1))
                    sleep 1
                done
                log "WARNING: serverNum never reached 1. Starting anyway."
                return 0
            fi
            [ -n "$val" ] && log "apex_iter='$val' (waiting for $expected_iter)..."
        fi
        attempts=$((attempts + 1))
        [ $((attempts % 10)) -eq 0 ] && log "Still waiting... ($attempts/$max_attempts)"
        sleep 2
    done
    log "ERROR: Timed out waiting for iteration $expected_iter!"
    return 1
}

# ===================== Cleanup =====================
cleanup_previous_run() {
    log "Killing stale APEX processes..."
    sudo pkill -9 latency_bench 2>/dev/null || true
    sudo pkill -9 ycsb_bench 2>/dev/null || true
    sleep 1

    log "Clearing /dev/shm RDMA artifacts..."
    sudo rm -f /dev/shm/dsm_* /dev/shm/rdma_* 2>/dev/null || true

    # Remove stale latency files
    cd "$APEX_DIR/build" 2>/dev/null || true
    rm -f apex_read_latency.dat apex_range_latency.dat 2>/dev/null || true
}

# ===================== Hugepages =====================
setup_hugepages() {
    log "Setting up hugepages (36864 × 2MB = 72 GB)..."
    echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
    ulimit -l unlimited 2>/dev/null || true
    local hp_free=$(grep HugePages_Free /proc/meminfo | awk '{print $2}')
    log "Hugepages available: $hp_free"
}

# ===================== BUILD =====================
log "Building APEX..."
cd "$APEX_DIR"
rm -rf build && mkdir build && cd build
cmake .. > /dev/null 2>&1
make -j$(nproc) latency_bench
log "Build complete."

# ===================== SWEEP =====================
echo ""
echo "============================================================"
echo "  APEX Skew Sweep — Node 1 (Compute)"
echo "  Configs: uniform, zipf_0.6, zipf_0.8, zipf_0.9, zipf_0.99"
echo "  Results: $RESULTS_DIR"
echo "============================================================"

for config in "${SKEW_CONFIGS[@]}"; do
    read -r UNIFORM ZIPF_THETA LABEL <<< "$config"
    
    echo ""
    echo "============================================================"
    echo "  APEX Compute — $LABEL (uniform=$UNIFORM, theta=$ZIPF_THETA)"
    echo "============================================================"
    
    cleanup_previous_run
    setup_hugepages

    # Wait for node0 to signal this iteration
    ITERATION=$((ITERATION + 1))
    wait_for_iteration "$ITERATION"
    
    # Run benchmark
    cd "$APEX_DIR/build"
    log "Running APEX compute for $LABEL ..."
    log "Config: ${READ_RATIO}% read + ${RANGE_RATIO}% range, ${TOTAL_OPS} ops, theta=${ZIPF_THETA}"
    echo ""
    
    sudo ./latency_bench \
        ${NODE_COUNT} ${THREAD_COUNT} ${READ_RATIO} ${RANGE_RATIO} \
        ${TOTAL_OPS} ${RANGE_SIZE} ${ZIPF_THETA} ${UNIFORM} \
        2>&1 | tee "$RESULTS_DIR/apex_${LABEL}_stdout.log"
    
    # Collect result files
    for f in apex_read_latency.dat apex_range_latency.dat; do
        if [ -f "$f" ]; then
            cp "$f" "$RESULTS_DIR/apex_${LABEL}_${f}"
            log "Saved $RESULTS_DIR/apex_${LABEL}_${f}"
        else
            log "WARNING: $f not found after $LABEL run!"
        fi
    done

    for f in apex_results_*.txt; do
        if [ -f "$f" ]; then
            cp "$f" "$RESULTS_DIR/apex_${LABEL}_${f}"
            log "Saved $RESULTS_DIR/apex_${LABEL}_${f}"
        fi
    done
    
    log "$LABEL compute complete. Sleeping 5s before next..."
    sleep 5
done

echo ""
echo "============================================================"
echo "  APEX SWEEP COMPLETE — Node 1 (Compute)"
echo "  Results in: $RESULTS_DIR"
echo "============================================================"
ls -la "$RESULTS_DIR"
