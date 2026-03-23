#!/bin/bash
###############################################################################
# DEX Sweep — Node 1 (Compute Node, 10.30.1.6)
#
# Polls memcached for each run signal from node0, then launches newbench_latency.
# Start this AFTER or AT THE SAME TIME as node0_memory.sh.
#
# USAGE:
#   bash node1_compute.sh
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$REPO_ROOT/dex/build"
RESULTS_DIR="$SCRIPT_DIR/results"
mkdir -p "$RESULTS_DIR"

MEMC_IP="10.30.1.9"
MEMC_PORT="11211"

# ── Must exactly match node0_memory.sh ───────────────────────────────────────
CACHE_SIZES=(32 64 128 256 512)

PAGE_CONFIGS=(
    "1024 1024 1kb_1kb"
)
# "1024 2048 1kb_2kb"
# "1024 4096 1kb_4kb"
# "512  1024 512b_1kb"

WORKLOADS=(
    "1 0.0  uniform"
    "0 0.99 zipf099"
)

NODES=2
THREADS=30
MEM_THREADS=4
BULK=50
WARMUP=10
OPS=50
READ=100
INSERT=0; UPDATE=0; DELETE=0; RANGE=0
CHECK=0
TIMEBASE=1
EARLY=0
INDEX=0
RPC=1
ADMIT=0.1
TUNE=0
MAX_THREAD=30

# ── Helpers ───────────────────────────────────────────────────────────────────
log() { echo "[$(date '+%H:%M:%S')] [NODE1] $*"; }

get_key() {
    printf "get %s\r\nquit\r\n" "$1" \
        | nc -w 3 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null \
        | grep -v "^VALUE\|^END" | tr -d '\r\n '
}

wait_for_signal() {
    local expected="$1"
    local elapsed=0
    log "Waiting for node0_ready=$expected ..."
    while true; do
        local val
        val=$(get_key "node0_ready")
        [ "$val" = "$expected" ] && { log "  Signal received: $expected"; return 0; }
        (( elapsed % 10 == 0 )) && log "  node0_ready='$val' (want '$expected') — ${elapsed}s elapsed"
        sleep 2; elapsed=$(( elapsed + 2 ))
        [ "$elapsed" -ge 600 ] && { log "ERROR: timed out waiting for $expected"; exit 1; }
    done
}

run_dex() {
    local label="$1" cache="$2" uniform="$3" theta="$4"
    log "=== RUN: $label | cache=${cache}MB | uniform=$uniform | theta=$theta ==="

    wait_for_signal "$label"

    cd "$BUILD_DIR"
    log "Launching newbench_latency..."
    sudo ./newbench_latency \
        $NODES $READ $INSERT $UPDATE $DELETE $RANGE \
        $THREADS $MEM_THREADS \
        $cache \
        $uniform $theta \
        $BULK $WARMUP $OPS \
        $CHECK $TIMEBASE $EARLY \
        $INDEX $RPC $ADMIT $TUNE $MAX_THREAD \
        2>&1 | tee "$RESULTS_DIR/${label}_node1.log"

    log "Run $label done."
    cd "$SCRIPT_DIR"
}

# ── Main ──────────────────────────────────────────────────────────────────────
log "DEX Sweep — Compute Node"
log "Results: $RESULTS_DIR"
echo ""

for page_cfg in "${PAGE_CONFIGS[@]}"; do
    read -r INNER_SZ LEAF_SZ PAGE_LABEL <<< "$page_cfg"

    for wl_cfg in "${WORKLOADS[@]}"; do
        read -r UNIFORM THETA WL_LABEL <<< "$wl_cfg"

        for CACHE in "${CACHE_SIZES[@]}"; do
            LABEL="${PAGE_LABEL}_${WL_LABEL}_cache${CACHE}mb"
            run_dex "$LABEL" "$CACHE" "$UNIFORM" "$THETA"
        done
    done
done

log "=== ALL RUNS COMPLETE ==="
ls -lh "$RESULTS_DIR/"
