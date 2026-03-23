#!/bin/bash
###############################################################################
# DEX Sweep — Node 1 (Compute Node, 10.30.1.6)
#
# Polls for each run signal from node0_memory.sh, then launches newbench_latency.
# Start at the same time as node0_memory.sh.
#
# MUST match node0_memory.sh exactly (same params, same label order).
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
RPC=0
ADMIT=0.1
TUNE=0
MAX_THREAD=30

CACHE_SWEEP=(
    "32  1 0.0  uniform_32mb"
    "64  1 0.0  uniform_64mb"
    "128 1 0.0  uniform_128mb"
    "256 1 0.0  uniform_256mb"
    "512 1 0.0  uniform_512mb"
    "32  0 0.99 zipf099_32mb"
    "64  0 0.99 zipf099_64mb"
    "128 0 0.99 zipf099_128mb"
    "256 0 0.99 zipf099_256mb"
    "512 0 0.99 zipf099_512mb"
)

LEAF_SIZE_CONFIGS=(
    "1024 1024 leaf1kb"
    "1024 2048 leaf2kb"
    "1024 4096 leaf4kb"
)
LEAF_SWEEP_CACHE=256; LEAF_SWEEP_UNIFORM=1; LEAF_SWEEP_THETA=0.0

INNER_SIZE_CONFIGS=(
    "1024 1024 inner1kb"
    "512  1024 inner512b"
    "128  1024 inner128b"
)
INNER_SWEEP_CACHE=256; INNER_SWEEP_UNIFORM=1; INNER_SWEEP_THETA=0.0

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
        [ "$val" = "$expected" ] && { log "  Signal received — launching"; return 0; }
        (( elapsed % 10 == 0 )) && log "  node0_ready='$val' (want '$expected') — ${elapsed}s"
        sleep 2; elapsed=$(( elapsed + 2 ))
        [ "$elapsed" -ge 600 ] && { log "ERROR: timed out waiting for $expected"; exit 1; }
    done
}

run_dex() {
    local label="$1" cache="$2" uniform="$3" theta="$4"
    echo ""
    log "══════════════════════════════════════════════════"
    log "RUN: $label  cache=${cache}MB  uniform=$uniform  theta=$theta"
    log "══════════════════════════════════════════════════"

    wait_for_signal "$label"

    cd "$BUILD_DIR"
    sudo ./newbench_latency \
        $NODES $READ $INSERT $UPDATE $DELETE $RANGE \
        $THREADS $MEM_THREADS \
        $cache \
        $uniform $theta \
        $BULK $WARMUP $OPS \
        $CHECK $TIMEBASE $EARLY \
        $INDEX $RPC $ADMIT $TUNE $MAX_THREAD \
        2>&1 | tee "$RESULTS_DIR/${label}_node1.log"

    log "Run $label DONE."
    cd "$SCRIPT_DIR"
}

# ── SECTION A+D ───────────────────────────────────────────────────────────────
log "SECTION A+D: Cache cliff & skew recovery"
for entry in "${CACHE_SWEEP[@]}"; do
    read -r CACHE UNIFORM THETA LABEL <<< "$entry"
    run_dex "$LABEL" "$CACHE" "$UNIFORM" "$THETA"
done

# ── SECTION B ─────────────────────────────────────────────────────────────────
log "SECTION B: Fat leaf dilution"
for cfg in "${LEAF_SIZE_CONFIGS[@]}"; do
    read -r INNER_SZ LEAF_SZ PAGE_LABEL <<< "$cfg"
    [ "$INNER_SZ" -eq 1024 ] && [ "$LEAF_SZ" -eq 1024 ] && continue
    run_dex "leafsize_${PAGE_LABEL}_cache${LEAF_SWEEP_CACHE}mb" \
        "$LEAF_SWEEP_CACHE" "$LEAF_SWEEP_UNIFORM" "$LEAF_SWEEP_THETA"
done

# ── SECTION C ─────────────────────────────────────────────────────────────────
log "SECTION C: Height effect"
for cfg in "${INNER_SIZE_CONFIGS[@]}"; do
    read -r INNER_SZ LEAF_SZ PAGE_LABEL <<< "$cfg"
    [ "$INNER_SZ" -eq 1024 ] && [ "$LEAF_SZ" -eq 1024 ] && continue
    run_dex "innersize_${PAGE_LABEL}_cache${INNER_SWEEP_CACHE}mb" \
        "$INNER_SWEEP_CACHE" "$INNER_SWEEP_UNIFORM" "$INNER_SWEEP_THETA"
done

log "ALL SECTIONS COMPLETE"
ls -lh "$RESULTS_DIR/"
