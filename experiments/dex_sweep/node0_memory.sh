#!/bin/bash
###############################################################################
# DEX Sweep — Node 0 (Memory Node, 10.30.1.9)
#
# Covers all crossover variables from DEX_DART_CROSSOVER.md:
#   A) Cache cliff        — cache 32→64→128→256→512 MB (uniform)
#   B) Fat leaf dilution  — kLeafPageSize 1KB / 2KB / 4KB (recompile)
#   C) Height effect      — kInternalPageSize 1KB / 512B / 128B (recompile)
#   D) Skew recovery      — zipf theta=0.99 across cache sizes
#
# After EVERY run: full memcached restart + counter reset.
# rpc=0  (one-sided RDMA only — fix applied in leanstore_cache.h)
# 30 threads on both nodes.
#
# USAGE:
#   1. Start this on Node 0 (10.30.1.9)
#   2. Start node1_compute.sh on Node 1 (10.30.1.6) at the same time
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

# ── Fixed benchmark params (match node1_compute.sh exactly) ──────────────────
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
RPC=0          # one-sided RDMA only (segfault fixed in leanstore_cache.h)
ADMIT=0.1
TUNE=0
MAX_THREAD=30

# ── Experiment matrix ─────────────────────────────────────────────────────────

# A+D: Cache cliff & skew recovery — default page sizes (1KB/1KB)
#      Format: "cache_mb uniform theta label"
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

# B: Fat leaf dilution — uniform, 256MB cache, vary kLeafPageSize
#    Format: "internal_bytes leaf_bytes label"
LEAF_SIZE_CONFIGS=(
    "1024 1024 leaf1kb"
    "1024 2048 leaf2kb"
    "1024 4096 leaf4kb"
)
LEAF_SWEEP_CACHE=256
LEAF_SWEEP_UNIFORM=1
LEAF_SWEEP_THETA=0.0

# C: Height effect — uniform, 256MB cache, vary kInternalPageSize (smaller=taller tree)
INNER_SIZE_CONFIGS=(
    "1024 1024 inner1kb"
    "512  1024 inner512b"
    "128  1024 inner128b"
)
INNER_SWEEP_CACHE=256
INNER_SWEEP_UNIFORM=1
INNER_SWEEP_THETA=0.0

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
    # Verify
    local reply
    reply=$(printf "get serverNum\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | head -1)
    if [[ "$reply" != *"VALUE"* ]]; then
        log "ERROR: memcached still not responding after restart"
        exit 1
    fi
    log "  memcached OK — serverNum=0 clientNum=0"
}

signal_ready() {
    local label="$1"
    printf "set node0_ready 0 0 %d\r\n%s\r\nquit\r\n" "${#label}" "$label" \
        | nc -w 2 "$MEMC_IP" "$MEMC_PORT" > /dev/null
    log "  signalled node1: node0_ready=$label"
}

set_page_sizes() {
    local internal_sz="$1" leaf_sz="$2"
    log "Patching Common.h: kInternalPageSize=$internal_sz kLeafPageSize=$leaf_sz"
    sed -i "s/constexpr uint32_t kInternalPageSize = [0-9]*/constexpr uint32_t kInternalPageSize = $internal_sz/" "$COMMON_H"
    sed -i "s/constexpr uint32_t kLeafPageSize = [0-9]*/constexpr uint32_t kLeafPageSize = $leaf_sz/" "$COMMON_H"
    grep "kInternalPageSize\|kLeafPageSize" "$COMMON_H"
}

rebuild() {
    log "Rebuilding newbench_latency..."
    cd "$BUILD_DIR"
    make -j$(nproc) newbench_latency 2>&1 | tail -3
    log "Build OK."
}

save_results() {
    local label="$1"
    [ -f "$BUILD_DIR/dex_read_latency.dat"  ] && cp "$BUILD_DIR/dex_read_latency.dat"  "$RESULTS_DIR/${label}_read_latency.dat"  && log "  saved ${label}_read_latency.dat"
    [ -f "$BUILD_DIR/dex_range_latency.dat" ] && cp "$BUILD_DIR/dex_range_latency.dat" "$RESULTS_DIR/${label}_range_latency.dat" && log "  saved ${label}_range_latency.dat"
}

run_dex() {
    local label="$1" cache="$2" uniform="$3" theta="$4"
    echo ""
    log "══════════════════════════════════════════════════"
    log "RUN: $label  cache=${cache}MB  uniform=$uniform  theta=$theta"
    log "══════════════════════════════════════════════════"

    restart_memcached
    signal_ready "$label"

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

# ── SECTION A+D: Cache cliff + skew recovery (default 1KB/1KB pages) ─────────
log "================================================================"
log "SECTION A+D: Cache cliff & skew recovery (1KB internal, 1KB leaf)"
log "================================================================"

set_page_sizes 1024 1024
rebuild

for entry in "${CACHE_SWEEP[@]}"; do
    read -r CACHE UNIFORM THETA LABEL <<< "$entry"
    run_dex "$LABEL" "$CACHE" "$UNIFORM" "$THETA"
done

# ── SECTION B: Fat leaf dilution (256MB cache, uniform, vary leaf size) ───────
log "================================================================"
log "SECTION B: Fat leaf dilution (cache=${LEAF_SWEEP_CACHE}MB, uniform)"
log "================================================================"

for cfg in "${LEAF_SIZE_CONFIGS[@]}"; do
    read -r INNER_SZ LEAF_SZ PAGE_LABEL <<< "$cfg"
    # skip 1kb_1kb — already done above
    [ "$INNER_SZ" -eq 1024 ] && [ "$LEAF_SZ" -eq 1024 ] && continue
    set_page_sizes "$INNER_SZ" "$LEAF_SZ"
    rebuild
    run_dex "leafsize_${PAGE_LABEL}_cache${LEAF_SWEEP_CACHE}mb" \
        "$LEAF_SWEEP_CACHE" "$LEAF_SWEEP_UNIFORM" "$LEAF_SWEEP_THETA"
done

# ── SECTION C: Height effect (256MB cache, uniform, vary internal node size) ──
log "================================================================"
log "SECTION C: Height effect (cache=${INNER_SWEEP_CACHE}MB, uniform)"
log "================================================================"

for cfg in "${INNER_SIZE_CONFIGS[@]}"; do
    read -r INNER_SZ LEAF_SZ PAGE_LABEL <<< "$cfg"
    [ "$INNER_SZ" -eq 1024 ] && [ "$LEAF_SZ" -eq 1024 ] && continue
    set_page_sizes "$INNER_SZ" "$LEAF_SZ"
    rebuild
    run_dex "innersize_${PAGE_LABEL}_cache${INNER_SWEEP_CACHE}mb" \
        "$INNER_SWEEP_CACHE" "$INNER_SWEEP_UNIFORM" "$INNER_SWEEP_THETA"
done

# ── Restore default page sizes ────────────────────────────────────────────────
log "Restoring default page sizes (1024/1024)..."
set_page_sizes 1024 1024
rebuild

log "================================================================"
log "ALL SECTIONS COMPLETE"
log "Results in: $RESULTS_DIR/"
ls -lh "$RESULTS_DIR/"
log "================================================================"
