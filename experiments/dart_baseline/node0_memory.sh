#!/bin/bash
###############################################################################
# DART Baseline — Node 0 (Memory Node, 10.30.1.9)
#
# Runs DART across uniform + zipf workloads to establish the flat baseline.
# Run this first, then start node1_compute.sh on 10.30.1.6.
#
# USAGE:
#   bash node0_memory.sh
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DART_DIR="$REPO_ROOT/DART-main"
RESULTS_DIR="$SCRIPT_DIR/results"
mkdir -p "$RESULTS_DIR"

MEMC_IP="10.30.1.9"
MEMC_PORT="11211"

WORKLOAD_RUNS=(
    "uniform_run   uniform"
    "zipf099_run   zipf099"
)

log() { echo "[$(date '+%H:%M:%S')] [DART-N0] $*"; }

signal_ready() {
    local label="$1"
    printf "set dart_ready 0 0 %d\r\n%s\r\nquit\r\n" "${#label}" "$label" \
        | nc -w 2 "$MEMC_IP" "$MEMC_PORT" > /dev/null
    log "  signalled node1: dart_ready=$label"
}

log "DART Baseline — Memory Node"
log "DART dir: $DART_DIR"

# Generate workloads if not already done
if [ ! -f "$DART_DIR/workloads/uniform_run" ]; then
    log "Generating YCSB workloads..."
    cd "$DART_DIR"
    bash benchmark_run/gen_workloads.sh
fi

for wl_cfg in "${WORKLOAD_RUNS[@]}"; do
    read -r WL_FILE WL_LABEL <<< "$wl_cfg"
    LABEL="dart_${WL_LABEL}"

    echo ""
    log "=== RUN: $LABEL | workload=$WL_FILE ==="

    # Signal compute node
    signal_ready "$LABEL"

    # Start memory process in background
    cd "$DART_DIR"
    log "Starting memory process..."
    bin/memory --monitor_addr=${MEMC_IP}:9898 --nic_index=0 \
        2>&1 | tee "$RESULTS_DIR/${LABEL}_memory.log" &
    MEMORY_PID=$!
    sleep 2

    # Start monitor (blocks until run completes)
    log "Starting monitor..."
    bin/monitor \
        --test_func=0 \
        --memory_num=1 \
        --compute_num=1 \
        --load_thread_num=30 \
        --run_thread_num=30 \
        --coro_num=1 \
        --mem_mb=4096 \
        --th_mb=10 \
        --workload_load=2m_load \
        --workload_run=${WL_FILE} \
        --bucket=256 \
        --run_max_request=2000000 \
        2>&1 | tee "$RESULTS_DIR/${LABEL}_monitor.log"

    wait $MEMORY_PID 2>/dev/null || true
    log "Run $LABEL complete."
    sleep 5
done

log "=== ALL DART RUNS COMPLETE ==="
ls -lh "$RESULTS_DIR/"
