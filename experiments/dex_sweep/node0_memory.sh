#!/bin/bash
###############################################################################
# DEX Cache Sweep — Node 0 (Memory Node, 10.30.1.9)
#
# ONLY sweeps cache sizes with DEFAULT page sizes (1KB internal, 1KB leaf).
# No recompile needed.
#
# Section A: uniform workload, 5 cache sizes (32/64/128/256/512 MB)
# Section D: zipf=0.99 workload, 5 cache sizes
#
# For page size experiments (leaf/inner node changes) use node0_pagesize.sh
#
# rpc=0, admit=0.1, 30 threads. Full memcached restart after every run.
#
# USAGE:
#   Node 0 (10.30.1.9): bash node0_memory.sh
#   Node 1 (10.30.1.6): bash node1_compute.sh   (start at same time)
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DEX_DIR="$REPO_ROOT/dex"
BUILD_DIR="$DEX_DIR/build"
RESULTS_DIR="$SCRIPT_DIR/results"
mkdir -p "$RESULTS_DIR"

MEMC_IP="10.30.1.9"
MEMC_PORT="11211"

# ── Fixed benchmark params ────────────────────────────────────────────────────
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

CACHE_SIZES=(32 64 128 256 512)

# ── Helpers ───────────────────────────────────────────────────────────────────
log() { echo "[$(date '+%H:%M:%S')] [NODE0] $*"; }

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

save_results() {
    local label="$1"
    [ -f "$BUILD_DIR/dex_read_latency.dat"  ] && \
        cp "$BUILD_DIR/dex_read_latency.dat"  "$RESULTS_DIR/${label}_read_latency.dat"  && \
        log "  saved ${label}_read_latency.dat"
    [ -f "$BUILD_DIR/dex_range_latency.dat" ] && \
        cp "$BUILD_DIR/dex_range_latency.dat" "$RESULTS_DIR/${label}_range_latency.dat" && \
        log "  saved ${label}_range_latency.dat"
}

run_dex() {
    local label="$1" cache="$2" uniform="$3" theta="$4"
    echo ""
    log "──────────────────────────────────────────────────"
    log "RUN: $label  cache=${cache}MB  uniform=$uniform  theta=$theta"
    log "──────────────────────────────────────────────────"

    restart_memcached
    memc_set "node0_ready" "$label"
    log "  signalled node0_ready=$label"

    cd "$BUILD_DIR"
    sudo ./newbench_latency \
        $NODES $READ $INSERT $UPDATE $DELETE $RANGE \
        $THREADS $MEM_THREADS \
        $cache \
        $uniform $theta \
        $BULK $WARMUP $OPS \
        $CHECK $TIMEBASE $EARLY \
        $INDEX $RPC $ADMIT $TUNE $MAX_THREAD \
        2>&1 | tee "$RESULTS_DIR/${label}_node0.log"

    save_results "$label"
    log "Run $label DONE."
    cd "$SCRIPT_DIR"
    sleep 3
}

# ── Verify default page sizes are in binary ───────────────────────────────────
log "Verifying binary uses default 1KB/1KB page sizes..."
grep "kInternalPageSize\|kLeafPageSize" "$DEX_DIR/include/Common.h"

# ═══════════════════════════════════════════════════════════════════════════════
log "════════════════════════════════════════════════════"
log "SECTION A: Cache cliff — uniform, 1KB/1KB pages"
log "════════════════════════════════════════════════════"

for CACHE in "${CACHE_SIZES[@]}"; do
    run_dex "cache_uniform_${CACHE}mb" "$CACHE" 1 0.0
done

log "════════════════════════════════════════════════════"
log "SECTION D: Skew recovery — zipf=0.99, 1KB/1KB pages"
log "════════════════════════════════════════════════════"

for CACHE in "${CACHE_SIZES[@]}"; do
    run_dex "cache_zipf099_${CACHE}mb" "$CACHE" 0 0.99
done

memc_set "node0_ready" "done"
log "════════════════════════════════════════════════════"
log "ALL CACHE SWEEP RUNS COMPLETE"
log "Results: $RESULTS_DIR/"
ls -lh "$RESULTS_DIR/"
log "════════════════════════════════════════════════════"
