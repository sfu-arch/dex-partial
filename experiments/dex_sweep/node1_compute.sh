#!/bin/bash
###############################################################################
# DEX Cache Sweep — Node 1 (Compute Node, 10.30.1.6)
#
# Paired with node0_memory.sh. No recompile needed (default 1KB/1KB pages).
# Start at the same time as node0_memory.sh.
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
BUILD_DIR="$REPO_ROOT/dex/build"
RESULTS_DIR="$SCRIPT_DIR/results"
mkdir -p "$RESULTS_DIR"

MEMC_IP="10.30.1.9"
MEMC_PORT="11211"

NODES=2; THREADS=30; MEM_THREADS=4; BULK=50; WARMUP=10; OPS=50
READ=100; INSERT=0; UPDATE=0; DELETE=0; RANGE=0
CHECK=0; TIMEBASE=1; EARLY=0; INDEX=0; RPC=0; ADMIT=0.1; TUNE=0; MAX_THREAD=30

CACHE_SIZES=(32 64 128 256 512)

log() { echo "[$(date '+%H:%M:%S')] [NODE1] $*"; }

memc_get() {
    printf "get %s\r\nquit\r\n" "$1" \
        | nc -w 3 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null \
        | grep -v "^VALUE\|^END\|^$" | tr -d '\r\n '
}

wait_for_run() {
    local expected="$1"
    local elapsed=0
    log "Waiting for node0_ready=$expected ..."
    while true; do
        local val; val=$(memc_get "node0_ready")
        [ "$val" = "$expected" ] && { log "  Signal received"; return 0; }
        (( elapsed % 10 == 0 )) && log "  node0_ready='$val' (want '$expected') — ${elapsed}s"
        sleep 2; elapsed=$(( elapsed + 2 ))
        [ "$elapsed" -ge 600 ] && { log "ERROR: timed out"; exit 1; }
    done
}

run_dex() {
    local label="$1" cache="$2" uniform="$3" theta="$4"
    echo ""
    log "──────────────────────────────────────────────────"
    log "RUN: $label  cache=${cache}MB  uniform=$uniform  theta=$theta"
    log "──────────────────────────────────────────────────"
    wait_for_run "$label"
    cd "$BUILD_DIR"
    sudo ./newbench_latency \
        $NODES $READ $INSERT $UPDATE $DELETE $RANGE \
        $THREADS $MEM_THREADS $cache $uniform $theta \
        $BULK $WARMUP $OPS $CHECK $TIMEBASE $EARLY \
        $INDEX $RPC $ADMIT $TUNE $MAX_THREAD \
        2>&1 | tee "$RESULTS_DIR/${label}_node1.log"
    log "Run $label DONE."
    cd "$SCRIPT_DIR"
}

log "SECTION A: Cache cliff — uniform"
for CACHE in "${CACHE_SIZES[@]}"; do
    run_dex "cache_uniform_${CACHE}mb" "$CACHE" 1 0.0
done

log "SECTION D: Skew recovery — zipf=0.99"
for CACHE in "${CACHE_SIZES[@]}"; do
    run_dex "cache_zipf099_${CACHE}mb" "$CACHE" 0 0.99
done

log "ALL CACHE SWEEP RUNS COMPLETE"
ls -lh "$RESULTS_DIR/"
