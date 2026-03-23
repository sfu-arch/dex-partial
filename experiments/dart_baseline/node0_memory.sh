#!/bin/bash
###############################################################################
# DART Baseline — Node 0 (Memory Node, 10.30.1.9)
#
# Covers DEX_DART_CROSSOVER.md section "DART flatness":
#   Runs uniform + zipf (0.6, 0.8, 0.9, 0.99) to confirm ~1.67 Mops/s flat.
#
# After EVERY run: full memcached restart + counter reset.
# 30 threads on both nodes.
#
# USAGE:
#   1. Start this on Node 0 (10.30.1.9)
#   2. Start node1_compute.sh on Node 1 (10.30.1.6) at the same time
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DART_DIR="$REPO_ROOT/DART-main"
RESULTS_DIR="$SCRIPT_DIR/results"
mkdir -p "$RESULTS_DIR"

MEMC_IP="10.30.1.9"
MEMC_PORT="11211"

# DART flatness sweep — all zipf levels + uniform (30 threads each)
WORKLOAD_RUNS=(
    "uniform_run   uniform"
    "zipf06_run    zipf06"
    "zipf08_run    zipf08"
    "zipf09_run    zipf09"
    "zipf099_run   zipf099"
)

# ── Helpers ───────────────────────────────────────────────────────────────────
log() { echo "[$(date '+%H:%M:%S')] [DART-N0] $*"; }

restart_memcached() {
    log "Restarting memcached..."
    sudo pkill -9 memcached 2>/dev/null || true
    sleep 2
    sudo memcached -u root -l 0.0.0.0 -p "$MEMC_PORT" -c 10000 -d
    sleep 2
    printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" > /dev/null
    printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" > /dev/null
    local reply
    reply=$(printf "get serverNum\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | head -1)
    if [[ "$reply" != *"VALUE"* ]]; then
        log "ERROR: memcached not responding after restart"; exit 1
    fi
    log "  memcached OK — serverNum=0 clientNum=0"
}

signal_ready() {
    local label="$1"
    printf "set dart_ready 0 0 %d\r\n%s\r\nquit\r\n" "${#label}" "$label" \
        | nc -w 2 "$MEMC_IP" "$MEMC_PORT" > /dev/null
    log "  signalled node1: dart_ready=$label"
}

# Generate workloads if not already present
if [ ! -f "$DART_DIR/workloads/uniform_run" ]; then
    log "Generating YCSB workloads..."
    cd "$DART_DIR"
    bash benchmark_run/gen_workloads.sh
fi

log "================================================================"
log "DART Flatness Sweep — Memory Node"
log "Workloads: ${WORKLOAD_RUNS[*]}"
log "Expected: ~1.67 Mops/s for ALL workloads (confirms skew insensitivity)"
log "================================================================"

for wl_cfg in "${WORKLOAD_RUNS[@]}"; do
    read -r WL_FILE WL_LABEL <<< "$wl_cfg"
    LABEL="dart_${WL_LABEL}"
    echo ""
    log "══════════════════════════════════════════"
    log "RUN: $LABEL  workload=$WL_FILE"
    log "══════════════════════════════════════════"

    restart_memcached
    signal_ready "$LABEL"

    cd "$DART_DIR"
    log "Starting memory process..."
    bin/memory --monitor_addr=${MEMC_IP}:9898 --nic_index=0 \
        2>&1 | tee "$RESULTS_DIR/${LABEL}_memory.log" &
    MEMORY_PID=$!
    sleep 2

    log "Starting monitor (30 threads)..."
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
    log "Run $LABEL DONE."
    sleep 5
done

log "================================================================"
log "ALL DART RUNS COMPLETE"
log "Results in: $RESULTS_DIR/"
ls -lh "$RESULTS_DIR/"
log "================================================================"
