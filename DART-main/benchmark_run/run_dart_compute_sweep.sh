#!/bin/bash
# run_dart_compute_sweep.sh — DART microbenchmark sweep, compute side
#
# HOW IT WORKS:
#   Runs on 10.30.1.6 (compute node).
#   For each experiment this script polls port 9898 on MONITOR_IP until the
#   monitor (on 10.30.1.9) opens it, then connects and runs.  When the
#   experiment finishes the compute binary exits, the loop advances, and we
#   poll again for the next monitor instance.
#
# USAGE:
#   On 10.30.1.9:  bash benchmark_run/run_dart_memnode_sweep.sh   ← start first
#   On 10.30.1.6:  bash benchmark_run/run_dart_compute_sweep.sh
#
# IMPORTANT — loop order must match run_dart_memnode_sweep.sh exactly:
#   outer: POINT_LOOKUPS then RANGE_QUERIES
#   inner: DISTRIBUTIONS × TH_MBS   (same arrays, same order)
#
# WORKLOAD PARAMETERS (set per-compute-node here, not in monitor):
#   --mb_bulk_load_num  keys preloaded into the tree before benchmarking
#   --mb_warmup_num     ops run untimed to warm up caches
#   --mb_op_num         timed benchmark ops
#   --mb_read_ratio /   share of Lookup / Range ops  (sum must be 100)
#   --mb_range_ratio
#   --mb_uniform        true = uniform, false = zipfian
#   --mb_zipfian        zipfian theta (ignored when uniform=true)
#   --mb_scan_num       keys fetched per Range op
#
# OUTPUT:
#   benchmark_run/results/dart_compute_<op>_<dist>_<cache>mb.txt  (per run)
#   benchmark_run/results/dart_compute_sweep_<timestamp>.log       (full log)

set -euo pipefail
cd "$(dirname "$0")/.."

# ── cluster config ────────────────────────────────────────────────────────
MONITOR_IP="10.30.1.9"
MONITOR_PORT=9898
NIC_INDEX=0
NODE_ID=0            # this compute node's id (for workload seed partitioning)

# ── workload config ───────────────────────────────────────────────────────
BULK_M=50            # million keys to bulk-load (50 M → ~3 GB dataset)
WARMUP_M=10          # million untimed warmup ops
RUN_M=50             # million timed benchmark ops
SCAN_NUM=100         # keys fetched per Range op

# ── sweep axes ────────────────────────────────────────────────────────────
# TH_MBS must match run_dart_memnode_sweep.sh (same order — drives sync)
TH_MBS=(2 4 8)

# Distributions (same array, same order as memory node script)
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
LOGFILE="${RESULTS_DIR}/dart_compute_sweep_$(date +%Y%m%d_%H%M).log"

# ── helpers ───────────────────────────────────────────────────────────────

# Poll until monitor port opens, then sleep briefly for monitor+memory to settle
wait_for_monitor() {
    echo "  [COM] waiting for monitor on ${MONITOR_IP}:${MONITOR_PORT}..."
    local waited=0
    while ! bash -c "echo > /dev/tcp/${MONITOR_IP}/${MONITOR_PORT}" 2>/dev/null; do
        sleep 2
        (( waited += 2 ))
        if (( waited % 20 == 0 )); then
            echo "  [COM] still waiting... ${waited}s"
        fi
    done
    sleep 2   # let memory node finish registering with monitor
    echo "  [COM] monitor ready (waited ${waited}s)"
}

# Convert shell 0/1 → gflag boolean string
bool_flag() { [ "$1" = "1" ] && echo "true" || echo "false"; }

run_exp() {
    local label="$1"    # e.g. "point/uniform" or "range/zipf0.99"
    local read_r="$2"   # Lookup ratio  (0–100)
    local range_r="$3"  # Range ratio   (0–100)
    local uni="$4"      # 1 = uniform, 0 = zipfian
    local theta="$5"    # zipfian theta (ignored when uni=1)
    local th_mb="$6"    # informational only — monitor controls this; used for output label
    local safe_label="${label//\//_}"
    local outfile="${RESULTS_DIR}/dart_compute_${safe_label}_${th_mb}mb.txt"

    # Ratios must sum to 100; fill remainder with Update (neutral write-back op)
    local update_r=$(( 100 - read_r - range_r ))
    [ "$update_r" -lt 0 ] && update_r=0

    local bulk_n=$(( BULK_M  * 1000000 ))
    local warmup_n=$(( WARMUP_M * 1000000 ))
    local op_n=$(( RUN_M * 1000000 ))

    wait_for_monitor

    echo ""
    echo "──────────────────────────────────────────"
    echo " [COM][START] ${label}  th_mb=${th_mb} MB  uni=${uni}  theta=${theta}"
    echo "──────────────────────────────────────────"

    bin/compute \
        --monitor_addr=${MONITOR_IP}:${MONITOR_PORT} \
        --nic_index=${NIC_INDEX} \
        --mb_bulk_load_num=${bulk_n} \
        --mb_warmup_num=${warmup_n} \
        --mb_op_num=${op_n} \
        --mb_read_ratio=${read_r} \
        --mb_insert_ratio=0 \
        --mb_update_ratio=${update_r} \
        --mb_delete_ratio=0 \
        --mb_range_ratio=${range_r} \
        --mb_uniform=$(bool_flag "$uni") \
        --mb_zipfian=${theta} \
        --mb_scan_num=${SCAN_NUM} \
        --mb_node_id=${NODE_ID} \
        2>&1 | tee "$outfile"

    echo " [COM][END]   ${label}  th_mb=${th_mb} MB"
    echo " Results → ${outfile}"
    sleep 3
}

# ── main ─────────────────────────────────────────────────────────────────
{
TOTAL=$(( ${#DISTRIBUTIONS[@]} * ${#TH_MBS[@]} * 2 ))
echo "========================================================"
echo "  DART microbench — compute sweep"
echo "  ${TOTAL} experiments total"
echo "  MONITOR=${MONITOR_IP}:${MONITOR_PORT}"
echo "  Bulk=${BULK_M}M  Warmup=${WARMUP_M}M  Run=${RUN_M}M"
echo "  Scan size=${SCAN_NUM}  NodeID=${NODE_ID}"
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
        run_exp "point/${dlabel}" 100 0 "$uni" "$theta" "$TH_MB"
    done
done

# ── RANGE QUERIES (100 % range, scan=SCAN_NUM) ────────────────────────────
echo ""
echo "===== RANGE QUERIES (scan=${SCAN_NUM}) ====="
for DIST in "${DISTRIBUTIONS[@]}"; do
    read -r dlabel uni theta <<< "$DIST"
    echo ""
    echo "  >> ${dlabel}  (uniform=${uni}  theta=${theta})"
    for TH_MB in "${TH_MBS[@]}"; do
        run_exp "range/${dlabel}" 0 100 "$uni" "$theta" "$TH_MB"
    done
done

echo ""
echo "========================================================"
echo "  Compute sweep complete."
echo "  ${TOTAL} experiments finished."
echo "  Results in ${RESULTS_DIR}/"
echo ""
echo "  Quick parse:"
echo "    grep 'throughput'                 ${RESULTS_DIR}/dart_compute_*.txt"
echo "    grep 'latency'                    ${RESULTS_DIR}/dart_compute_*.txt"
echo "    grep 'Avg. rdma read / op'        ${RESULTS_DIR}/dart_compute_*.txt"
echo "========================================================"
} 2>&1 | tee "$LOGFILE"
