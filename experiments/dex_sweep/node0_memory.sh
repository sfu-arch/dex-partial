#!/bin/bash
###############################################################################
# DEX Sweep — Node 0 (Memory Node, 10.30.1.9)
#
# Sweeps cache sizes AND page sizes (requires recompile for page size changes).
# Run this FIRST. Then start node1_compute.sh on 10.30.1.6.
#
# USAGE:
#   bash node0_memory.sh
#
# OUTPUT: results/ directory with .dat and .log files per run
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DEX_DIR="$REPO_ROOT/dex"
BUILD_DIR="$DEX_DIR/build"
COMMON_H="$DEX_DIR/include/Common.h"
RESULTS_DIR="$SCRIPT_DIR/results"
mkdir -p "$RESULTS_DIR"

MEMC_IP="10.30.1.9"
MEMC_PORT="11211"

# ── Experiment knobs ──────────────────────────────────────────────────────────
# Cache sizes to sweep (MB)
CACHE_SIZES=(32 64 128 256 512)

# Page size configurations: "internal_bytes leaf_bytes label"
PAGE_CONFIGS=(
    "1024 1024 1kb_1kb"
)
# To also test fat leaves, add more rows:
#   "1024 2048 1kb_2kb"
#   "1024 4096 1kb_4kb"
#   "512  1024 512b_1kb"

# Workloads: "uniform theta label"
WORKLOADS=(
    "1 0.0  uniform"
    "0 0.99 zipf099"
)

# Fixed benchmark params
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
RPC=1        # rpc=0 causes segfault — keep at 1
ADMIT=0.1
TUNE=0
MAX_THREAD=30

# ── Helpers ───────────────────────────────────────────────────────────────────
log() { echo "[$(date '+%H:%M:%S')] [NODE0] $*"; }

reset_memcached() {
    log "Resetting memcached counters..."
    printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" > /dev/null
    printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" > /dev/null
    log "  serverNum=0 clientNum=0 done"
}

verify_memcached() {
    local reply
    reply=$(printf "get serverNum\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | head -1)
    if [[ "$reply" != *"VALUE"* ]]; then
        log "ERROR: memcached not responding. Start it first:"
        log "  sudo pkill -9 memcached; sleep 2"
        log "  sudo memcached -u root -l 0.0.0.0 -p $MEMC_PORT -c 10000 -d"
        log "  sleep 2"
        log "  printf 'set serverNum 0 0 1\r\n0\r\nquit\r\n' | nc -w 2 $MEMC_IP $MEMC_PORT"
        log "  printf 'set clientNum 0 0 1\r\n0\r\nquit\r\n' | nc -w 2 $MEMC_IP $MEMC_PORT"
        exit 1
    fi
    log "memcached OK"
}

set_page_sizes() {
    local internal_sz="$1"
    local leaf_sz="$2"
    log "Setting kInternalPageSize=$internal_sz kLeafPageSize=$leaf_sz in Common.h..."
    sed -i "s/constexpr uint32_t kInternalPageSize = [0-9]*/constexpr uint32_t kInternalPageSize = $internal_sz/" "$COMMON_H"
    sed -i "s/constexpr uint32_t kLeafPageSize = [0-9]*/constexpr uint32_t kLeafPageSize = $leaf_sz/" "$COMMON_H"
    # Verify
    grep "kInternalPageSize\|kLeafPageSize" "$COMMON_H"
}

rebuild_dex() {
    log "Rebuilding DEX..."
    cd "$BUILD_DIR"
    make -j$(nproc) newbench_latency 2>&1 | tail -5
    log "Build complete."
}

signal_ready() {
    local label="$1"
    printf "set node0_ready 0 0 %d\r\n%s\r\nquit\r\n" "${#label}" "$label" \
        | nc -w 2 "$MEMC_IP" "$MEMC_PORT" > /dev/null
    log "  signalled node1: node0_ready=$label"
}

run_dex() {
    local label="$1" cache="$2" uniform="$3" theta="$4"
    log "=== RUN: $label | cache=${cache}MB | uniform=$uniform | theta=$theta ==="

    reset_memcached
    signal_ready "$label"

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
        2>&1 | tee "$RESULTS_DIR/${label}_node0.log"

    [ -f "dex_read_latency.dat"  ] && cp "dex_read_latency.dat"  "$RESULTS_DIR/${label}_read_latency.dat"
    [ -f "dex_range_latency.dat" ] && cp "dex_range_latency.dat" "$RESULTS_DIR/${label}_range_latency.dat"
    log "Run $label done. Results in $RESULTS_DIR/"
    cd "$SCRIPT_DIR"
    sleep 3
}

# ── Main ──────────────────────────────────────────────────────────────────────
log "DEX Sweep — Memory Node"
log "Repo: $REPO_ROOT"
log "Results: $RESULTS_DIR"
echo ""

verify_memcached

for page_cfg in "${PAGE_CONFIGS[@]}"; do
    read -r INNER_SZ LEAF_SZ PAGE_LABEL <<< "$page_cfg"

    log ">>> Page config: internal=${INNER_SZ}B leaf=${LEAF_SZ}B ($PAGE_LABEL)"
    set_page_sizes "$INNER_SZ" "$LEAF_SZ"
    rebuild_dex

    for wl_cfg in "${WORKLOADS[@]}"; do
        read -r UNIFORM THETA WL_LABEL <<< "$wl_cfg"

        for CACHE in "${CACHE_SIZES[@]}"; do
            LABEL="${PAGE_LABEL}_${WL_LABEL}_cache${CACHE}mb"
            run_dex "$LABEL" "$CACHE" "$UNIFORM" "$THETA"
        done
    done
done

log "=== ALL RUNS COMPLETE ==="
log "Results in: $RESULTS_DIR/"
ls -lh "$RESULTS_DIR/"
