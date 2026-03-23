#!/bin/bash
###############################################################################
# DART Baseline — Node 0 (Memory Node, 10.30.1.9)
#
# Confirms DART flatness (~1.67 Mops/s) across all skew levels:
#   uniform, zipf 0.6, 0.8, 0.9, 0.99
#
# 30 threads. Full memcached restart after every run.
# No rebuild needed — DART has no compile-time page-size constants.
#
# USAGE:
#   Node 0 (10.30.1.9): bash node0_memory.sh
#   Node 1 (10.30.1.6): bash node1_compute.sh   (start at same time)
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DART_DIR="$REPO_ROOT/DART-main"
RESULTS_DIR="$SCRIPT_DIR/results"
mkdir -p "$RESULTS_DIR"

MEMC_IP="10.30.1.9"
MEMC_PORT="11211"

# Flatness sweep — all skew levels (expect ~1.67 Mops/s for ALL)
WORKLOAD_RUNS=(
    "uniform_run  uniform"
    "zipf06_run   zipf06"
    "zipf08_run   zipf08"
    "zipf09_run   zipf09"
    "zipf099_run  zipf099"
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
    [[ "$reply" != *"VALUE"* ]] && { log "ERROR: memcached not up after restart"; exit 1; }
    log "  memcached OK"
}

memc_set() {
    printf "set %s 0 0 %d\r\n%s\r\nquit\r\n" "$1" "${#2}" "$2" \
        | nc -w 2 "$MEMC_IP" "$MEMC_PORT" > /dev/null
}

memc_get() {
    printf "get %s\r\nquit\r\n" "$1" \
        | nc -w 3 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null \
        | grep -v "^VALUE\|^END\|^$" | tr -d '\r\n '
}

wait_for_node1_ready() {
    local expected="$1"
    local elapsed=0
    log "  Waiting for node1 compute to finish: $expected ..."
    while true; do
        local val
        val=$(memc_get "dart_node1_done")
        [ "$val" = "$expected" ] && { log "  node1 done: $expected"; return 0; }
        sleep 3; elapsed=$(( elapsed + 3 ))
        [ "$elapsed" -ge 900 ] && { log "ERROR: timed out waiting for node1 $expected"; exit 1; }
    done
}

# Generate YCSB workloads if not present
if [ ! -f "$DART_DIR/workloads/uniform_run" ]; then
    log "Generating YCSB workloads..."
    cd "$DART_DIR"
    bash benchmark_run/gen_workloads.sh
fi

log "════════════════════════════════════════════════════"
log "DART Flatness Sweep — Memory Node"
log "Expected: ~1.67 Mops/s for ALL workloads"
log "════════════════════════════════════════════════════"

for wl_cfg in "${WORKLOAD_RUNS[@]}"; do
    read -r WL_FILE WL_LABEL <<< "$wl_cfg"
    LABEL="dart_${WL_LABEL}"
    echo ""
    log "──────────────────────────────────────────────────"
    log "RUN: $LABEL  workload=$WL_FILE"
    log "──────────────────────────────────────────────────"

    restart_memcached

    # Signal node1 which run is starting
    memc_set "dart_ready" "$LABEL"
    log "  signalled dart_ready=$LABEL"

    cd "$DART_DIR"

    # Start memory process in background
    log "Starting memory process..."
    bin/memory --monitor_addr=${MEMC_IP}:9898 --nic_index=0 \
        2>&1 | tee "$RESULTS_DIR/${LABEL}_memory.log" &
    MEMORY_PID=$!
    sleep 2

    # Start monitor (blocks until run completes)
    log "Starting monitor (30 load + 30 run threads)..."
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
    wait_for_node1_ready "$LABEL"
    log "Run $LABEL DONE."
    sleep 5
done

memc_set "dart_ready" "done"

log "════════════════════════════════════════════════════"
log "ALL DART RUNS COMPLETE"
log "Results in: $RESULTS_DIR/"
ls -lh "$RESULTS_DIR/"
log "════════════════════════════════════════════════════"
