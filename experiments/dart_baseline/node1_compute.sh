#!/bin/bash
###############################################################################
# DART Baseline — Node 1 (Compute Node, 10.30.1.6)
#
# Polls for each run signal from node0, launches DART compute, ACKs done.
# No rebuild needed — DART has no compile-time page-size constants.
# Start at the same time as node0_memory.sh.
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
    "uniform_run  uniform"
    "zipf06_run   zipf06"
    "zipf08_run   zipf08"
    "zipf09_run   zipf09"
    "zipf099_run  zipf099"
)

# ── Helpers ───────────────────────────────────────────────────────────────────
log() { echo "[$(date '+%H:%M:%S')] [DART-N1] $*"; }

memc_set() {
    printf "set %s 0 0 %d\r\n%s\r\nquit\r\n" "$1" "${#2}" "$2" \
        | nc -w 2 "$MEMC_IP" "$MEMC_PORT" > /dev/null
}

memc_get() {
    printf "get %s\r\nquit\r\n" "$1" \
        | nc -w 3 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null \
        | grep -v "^VALUE\|^END\|^$" | tr -d '\r\n '
}

wait_for_signal() {
    local expected="$1"
    local elapsed=0
    log "Waiting for dart_ready=$expected ..."
    while true; do
        local val
        val=$(memc_get "dart_ready")
        [ "$val" = "$expected" ] && { log "  Signal received"; return 0; }
        (( elapsed % 10 == 0 )) && log "  dart_ready='$val' (want '$expected') — ${elapsed}s"
        sleep 2; elapsed=$(( elapsed + 2 ))
        [ "$elapsed" -ge 600 ] && { log "ERROR: timed out waiting for $expected"; exit 1; }
    done
}

log "════════════════════════════════════════════════════"
log "DART Flatness Sweep — Compute Node"
log "════════════════════════════════════════════════════"

for wl_cfg in "${WORKLOAD_RUNS[@]}"; do
    read -r WL_FILE WL_LABEL <<< "$wl_cfg"
    LABEL="dart_${WL_LABEL}"
    echo ""
    log "──────────────────────────────────────────────────"
    log "RUN: $LABEL"
    log "──────────────────────────────────────────────────"

    wait_for_signal "$LABEL"

    cd "$DART_DIR"
    log "Starting compute process (30 threads)..."
    bin/compute --monitor_addr=${MEMC_IP}:9898 --nic_index=0 \
        2>&1 | tee "$RESULTS_DIR/${LABEL}_compute.log"

    # ACK to node0 that compute is done
    memc_set "dart_node1_done" "$LABEL"
    log "Run $LABEL DONE."
    sleep 3
done

log "ALL DART RUNS COMPLETE"
ls -lh "$RESULTS_DIR/"
