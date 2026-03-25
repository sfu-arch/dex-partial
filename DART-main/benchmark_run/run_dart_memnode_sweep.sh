#!/bin/bash
# run_dart_memnode_sweep.sh — DART microbenchmark sweep, memory/monitor side
#
# HOW IT WORKS:
#   Runs on 10.30.1.9 (memory + monitor combined).
#   For each experiment the monitor starts in the foreground (blocks until the
#   compute node connects, runs, and reports throughput), then the loop advances.
#   The compute node's run_dart_compute_sweep.sh polls port 9898 as the signal
#   that a new monitor is ready — no memcached needed.
#
# USAGE:
#   On 10.30.1.9:   bash benchmark_run/run_dart_memnode_sweep.sh
#   On 10.30.1.6:   bash benchmark_run/run_dart_compute_sweep.sh
#   (start both within ~60 s of each other; exact timing doesn't matter)
#
# SWEEP DIMENSIONS:
#   • Thread-local cache size  → --th_mb  (analog of DEX CACHES sweep)
#   • Operation type           → point lookups, range queries
#   • Key distribution         → uniform, zipf 0.30 / 0.50 / 0.60 / 0.99
#
# OUTPUT:
#   benchmark_run/results/dart_monitor_<op>_<dist>_<cache>mb.txt  (per run)
#   benchmark_run/results/dart_memnode_sweep_<timestamp>.log       (full log)

set -euo pipefail
cd "$(dirname "$0")/.."

# ── cluster config ────────────────────────────────────────────────────────
MONITOR_IP="10.30.1.9"
MONITOR_PORT=9898
NIC_INDEX=0

# ── monitor / experiment config ───────────────────────────────────────────
LOAD_THREADS=30          # threads used during bulk-load phase
RUN_THREADS=30           # threads used during benchmark phase
CORO_NUM=1
MEM_MB=32768             # remote memory pool (32 GB — sized for 50 M × 64 B keys)
BUCKET=256               # RACE skip-table hash buckets

# Thread-local cache sweep (MB per thread).
# This is the DART analog of DEX's CACHES=(128 256 512).
# Larger values allow more ART nodes to be buffered locally.
TH_MBS=(2 4 8)

# ── distributions (must match compute script exactly — same order) ─────────
# Format: "label  uniform(1/0)  zipf_theta"
DISTRIBUTIONS=(
    "uniform   1  0.99"
    "zipf0.30  0  0.30"
    "zipf0.50  0  0.50"
    "zipf0.60  0  0.60"
    "zipf0.99  0  0.99"
)

# ── output ────────────────────────────────────────────────────────────────
RESULTS_DIR="benchmark_run/results"
mkdir -p "$RESULTS_DIR"
LOGFILE="${RESULTS_DIR}/dart_memnode_sweep_$(date +%Y%m%d_%H%M).log"

# ── helpers ───────────────────────────────────────────────────────────────

# Start memory node in background, run monitor in foreground.
# The monitor blocks until compute connects + experiment finishes.
run_exp() {
    local label="$1"    # e.g. "point/uniform" or "range/zipf0.99"
    local th_mb="$2"    # local cache per thread (MB)
    local safe_label="${label//\//_}"
    local outfile="${RESULTS_DIR}/dart_monitor_${safe_label}_${th_mb}mb.txt"

    echo ""
    echo "──────────────────────────────────────────"
    echo " [MEM][START] ${label}  th_mb=${th_mb} MB"
    echo "──────────────────────────────────────────"

    # Memory node: start in background, exits when monitor signals done
    bin/memory \
        --monitor_addr=${MONITOR_IP}:${MONITOR_PORT} \
        --nic_index=${NIC_INDEX} \
        &
    local mem_pid=$!
    sleep 1   # give memory time to register with monitor

    # Monitor: foreground — blocks until all nodes have reported results
    bin/monitor \
        --test_func=1 \
        --memory_num=1 \
        --compute_num=1 \
        --load_thread_num=${LOAD_THREADS} \
        --run_thread_num=${RUN_THREADS} \
        --coro_num=${CORO_NUM} \
        --mem_mb=${MEM_MB} \
        --th_mb=${th_mb} \
        --bucket=${BUCKET} \
        --workload_load=unused \
        --workload_run=unused \
        2>&1 | tee "$outfile"

    # Reap memory node
    wait "$mem_pid" 2>/dev/null || true

    echo " [MEM][END]   ${label}  th_mb=${th_mb} MB"
    echo " Results → ${outfile}"
    sleep 3
}

# ── main ─────────────────────────────────────────────────────────────────
{
TOTAL=$(( ${#DISTRIBUTIONS[@]} * ${#TH_MBS[@]} * 2 ))
echo "========================================================"
echo "  DART microbench — memory/monitor sweep"
echo "  ${TOTAL} experiments total"
echo "  MONITOR=${MONITOR_IP}:${MONITOR_PORT}"
echo "  Remote memory pool : ${MEM_MB} MB"
echo "  Thread-local cache : ${TH_MBS[*]} MB"
echo "  Distributions      : ${#DISTRIBUTIONS[@]}"
echo "  Operation types    : point-lookup  range-query"
echo "  Log → ${LOGFILE}"
echo "========================================================"
echo ""

# ── POINT LOOKUPS (100 % read) ────────────────────────────────────────────
echo "===== POINT LOOKUPS ====="
for DIST in "${DISTRIBUTIONS[@]}"; do
    read -r dlabel uni theta <<< "$DIST"
    echo ""
    echo "  >> ${dlabel}  (uniform=${uni}  theta=${theta})"
    for TH_MB in "${TH_MBS[@]}"; do
        run_exp "point/${dlabel}" "$TH_MB"
    done
done

# ── RANGE QUERIES (100 % range, scan=100) ─────────────────────────────────
echo ""
echo "===== RANGE QUERIES (scan=100) ====="
for DIST in "${DISTRIBUTIONS[@]}"; do
    read -r dlabel uni theta <<< "$DIST"
    echo ""
    echo "  >> ${dlabel}  (uniform=${uni}  theta=${theta})"
    for TH_MB in "${TH_MBS[@]}"; do
        run_exp "range/${dlabel}" "$TH_MB"
    done
done

echo ""
echo "========================================================"
echo "  Memory/monitor sweep complete."
echo "  ${TOTAL} experiments finished."
echo "  Results in ${RESULTS_DIR}/"
echo ""
echo "  Parsing tips:"
echo "    grep 'throughput'   ${RESULTS_DIR}/dart_monitor_*.txt"
echo "    grep 'latency'      ${RESULTS_DIR}/dart_monitor_*.txt"
echo "========================================================"
} 2>&1 | tee "$LOGFILE"
