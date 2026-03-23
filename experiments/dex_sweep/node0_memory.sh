#!/bin/bash
###############################################################################
# DEX Sweep — Node 0 (Memory Node, 10.30.1.9)
#
# Covers all crossover variables from DEX_DART_CROSSOVER.md:
#   A) Cache cliff + skew  — 1KB/1KB pages, uniform + zipf099, 5 cache sizes
#   B) Fat leaf dilution   — leaf 2KB & 4KB, uniform, 5 cache sizes each
#   C) Height effect       — inner 512B & 128B, uniform, 5 cache sizes each
#
# Rebuild handshake: node0 signals each new page config → waits for node1 to
# ACK rebuild done → then proceeds with run signals. Works on both shared NFS
# and separate filesystems.
#
# rpc=0, admit=0.1, 30 threads, full memcached restart after every run.
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
COMMON_H="$DEX_DIR/include/Common.h"
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

memc_get() {
    printf "get %s\r\nquit\r\n" "$1" \
        | nc -w 3 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null \
        | grep -v "^VALUE\|^END\|^$" | tr -d '\r\n '
}

# Signal node1 to rebuild for a new page config, then wait for its ACK.
signal_rebuild_and_wait() {
    local config_label="$1"
    log "Signalling node1 to rebuild: config=$config_label"
    memc_set "node0_config" "$config_label"
    # Clear any stale ACK from a previous run
    memc_set "node1_rebuild_done" "none"

    local elapsed=0
    while true; do
        local val
        val=$(memc_get "node1_rebuild_done")
        [ "$val" = "$config_label" ] && { log "  node1 rebuild ACK received"; return 0; }
        (( elapsed % 15 == 0 )) && log "  waiting for node1 rebuild ACK... (${elapsed}s)"
        sleep 3; elapsed=$(( elapsed + 3 ))
        [ "$elapsed" -ge 600 ] && { log "ERROR: timed out waiting for node1 rebuild ACK"; exit 1; }
    done
}

signal_run_ready() {
    local label="$1"
    memc_set "node0_ready" "$label"
    log "  signalled node0_ready=$label"
}

set_page_sizes() {
    local internal_sz="$1" leaf_sz="$2"
    log "Patching Common.h: kInternalPageSize=$internal_sz  kLeafPageSize=$leaf_sz"
    sed -i "s/constexpr uint32_t kInternalPageSize = [0-9]*/constexpr uint32_t kInternalPageSize = $internal_sz/" "$COMMON_H"
    sed -i "s/constexpr uint32_t kLeafPageSize = [0-9]*/constexpr uint32_t kLeafPageSize = $leaf_sz/" "$COMMON_H"
    grep "kInternalPageSize\|kLeafPageSize" "$COMMON_H"
}

rebuild() {
    log "Rebuilding newbench_latency..."
    cd "$BUILD_DIR"
    make -j$(nproc) newbench_latency 2>&1 | tail -3
    log "Build OK."
    cd "$SCRIPT_DIR"
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
    signal_run_ready "$label"

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

# Run the full 5-cache-size uniform sweep for a given page config + label prefix
run_cache_sweep_uniform() {
    local prefix="$1"
    for CACHE in "${CACHE_SIZES[@]}"; do
        run_dex "${prefix}_uniform_cache${CACHE}mb" "$CACHE" 1 0.0
    done
}

# ═══════════════════════════════════════════════════════════════════════════════
# SECTION A+D — default 1KB/1KB pages, uniform + zipf099, all cache sizes
# ═══════════════════════════════════════════════════════════════════════════════
echo ""
log "════════════════════════════════════════════════════"
log "SECTION A+D: Cache cliff & skew recovery"
log "  Pages: internal=1024B  leaf=1024B"
log "════════════════════════════════════════════════════"

set_page_sizes 1024 1024
rebuild
signal_rebuild_and_wait "inner1024_leaf1024"

# Uniform sweep
for CACHE in "${CACHE_SIZES[@]}"; do
    run_dex "ad_inner1024_leaf1024_uniform_cache${CACHE}mb" "$CACHE" 1 0.0
done

# Zipf sweep (Section D)
for CACHE in "${CACHE_SIZES[@]}"; do
    run_dex "ad_inner1024_leaf1024_zipf099_cache${CACHE}mb" "$CACHE" 0 0.99
done

# ═══════════════════════════════════════════════════════════════════════════════
# SECTION B — fat leaf dilution, uniform, 5 cache sizes per leaf config
# ═══════════════════════════════════════════════════════════════════════════════
echo ""
log "════════════════════════════════════════════════════"
log "SECTION B: Fat leaf dilution (uniform, 5 cache sizes)"
log "════════════════════════════════════════════════════"

# B1: 2KB leaves
set_page_sizes 1024 2048
rebuild
signal_rebuild_and_wait "inner1024_leaf2048"
run_cache_sweep_uniform "b_inner1024_leaf2048"

# B2: 4KB leaves
set_page_sizes 1024 4096
rebuild
signal_rebuild_and_wait "inner1024_leaf4096"
run_cache_sweep_uniform "b_inner1024_leaf4096"

# ═══════════════════════════════════════════════════════════════════════════════
# SECTION C — height effect, uniform, 5 cache sizes per inner-node config
# ═══════════════════════════════════════════════════════════════════════════════
echo ""
log "════════════════════════════════════════════════════"
log "SECTION C: Height effect (uniform, 5 cache sizes)"
log "════════════════════════════════════════════════════"

# C1: 512B internal nodes (taller tree)
set_page_sizes 512 1024
rebuild
signal_rebuild_and_wait "inner512_leaf1024"
run_cache_sweep_uniform "c_inner512_leaf1024"

# C2: 128B internal nodes (much taller tree)
set_page_sizes 128 1024
rebuild
signal_rebuild_and_wait "inner128_leaf1024"
run_cache_sweep_uniform "c_inner128_leaf1024"

# ── Restore defaults ──────────────────────────────────────────────────────────
log "Restoring default page sizes 1024/1024..."
set_page_sizes 1024 1024
rebuild
memc_set "node0_config" "done"

echo ""
log "════════════════════════════════════════════════════"
log "ALL SECTIONS COMPLETE"
log "Results in: $RESULTS_DIR/"
ls -lh "$RESULTS_DIR/"
log "════════════════════════════════════════════════════"
