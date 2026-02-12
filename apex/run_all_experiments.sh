#!/bin/bash
#############################################################################
# APEX vs DEX vs CHIME Comparison Script
#
# Runs the same workloads across all three systems and collects results.
# Must be run from the DEX-CHIME root directory on the COMPUTE node
# (the memory node should already be running).
#############################################################################

set -e

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RESULTS_DIR="$ROOT_DIR/experiments/apex_results"
TIMESTAMP=$(date '+%Y%m%d_%H%M%S')

NODE_COUNT=2
THREAD_COUNT=16
TOTAL_OPS=5000000
RANGE_SIZE=100

mkdir -p "$RESULTS_DIR"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] $1"
}

run_apex_latency() {
    local read_pct=$1
    local range_pct=$2
    local label=$3

    log "=== APEX: $label (read=$read_pct%, range=$range_pct%) ==="

    cd "$ROOT_DIR/apex/build"
    ./latency_bench $NODE_COUNT $THREAD_COUNT $read_pct $range_pct $TOTAL_OPS $RANGE_SIZE \
        2>&1 | tee "$RESULTS_DIR/apex_${label}_${TIMESTAMP}.log"

    # Collect CSV outputs
    for f in apex_*.csv; do
        [ -f "$f" ] && mv "$f" "$RESULTS_DIR/${f%.csv}_${label}_${TIMESTAMP}.csv"
    done
}

run_apex_ycsb() {
    local read_pct=$1
    local insert_pct=$2
    local update_pct=$3
    local theta=$4
    local label=$5

    log "=== APEX YCSB: $label (R=$read_pct I=$insert_pct U=$update_pct θ=$theta) ==="

    cd "$ROOT_DIR/apex/build"
    ./ycsb_bench $NODE_COUNT $THREAD_COUNT $read_pct $insert_pct $update_pct $TOTAL_OPS $theta \
        2>&1 | tee "$RESULTS_DIR/apex_ycsb_${label}_${TIMESTAMP}.log"

    for f in apex_ycsb_*.txt; do
        [ -f "$f" ] && mv "$f" "$RESULTS_DIR/"
    done
}

# ─── Build APEX ─────────────────────────────────────────────────────
log "Building APEX..."
cd "$ROOT_DIR/apex"
mkdir -p build && cd build
cmake .. && make -j$(nproc)
cd "$ROOT_DIR"

# ─── Latency Experiments ────────────────────────────────────────────
log "╔══════════════════════════════════════════╗"
log "║  APEX Latency Experiments                ║"
log "╚══════════════════════════════════════════╝"

run_apex_latency 100 0  "read_only"
run_apex_latency 70  30 "mixed_rw_range"
run_apex_latency 0   100 "range_only"
run_apex_latency 50  50 "balanced"

# ─── YCSB Experiments ───────────────────────────────────────────────
log "╔══════════════════════════════════════════╗"
log "║  APEX YCSB Throughput Experiments        ║"
log "╚══════════════════════════════════════════╝"

run_apex_ycsb 50  0  50 0.99 "ycsb_a"
run_apex_ycsb 95  0  5  0.99 "ycsb_b"
run_apex_ycsb 100 0  0  0.99 "ycsb_c"
run_apex_ycsb 95  5  0  0.99 "ycsb_d"

# ─── Skew Sensitivity ───────────────────────────────────────────────
log "╔══════════════════════════════════════════╗"
log "║  APEX Skew Sensitivity (θ sweep)         ║"
log "╚══════════════════════════════════════════╝"

for theta in 0.0 0.5 0.9 0.99 0.999; do
    run_apex_ycsb 100 0 0 $theta "skew_${theta}"
done

# ─── Thread Scalability ─────────────────────────────────────────────
log "╔══════════════════════════════════════════╗"
log "║  APEX Thread Scalability                 ║"
log "╚══════════════════════════════════════════╝"

for threads in 1 2 4 8 16 24 32; do
    log "--- Threads: $threads ---"
    cd "$ROOT_DIR/apex/build"
    ./ycsb_bench $NODE_COUNT $threads 50 0 50 $TOTAL_OPS 0.99 \
        2>&1 | tee "$RESULTS_DIR/apex_scale_t${threads}_${TIMESTAMP}.log"
    for f in apex_ycsb_*.txt; do
        [ -f "$f" ] && mv "$f" "$RESULTS_DIR/"
    done
done

# ─── Summary ────────────────────────────────────────────────────────
log ""
log "╔══════════════════════════════════════════╗"
log "║  All experiments complete!               ║"
log "╚══════════════════════════════════════════╝"
log ""
log "Results directory: $RESULTS_DIR"
log "Files:"
ls -la "$RESULTS_DIR/"
