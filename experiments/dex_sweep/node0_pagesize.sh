#!/bin/bash
###############################################################################
# DEX Page Size Sweep — Node 0 (Memory Node, 10.30.1.9)
#
# Changes kLeafPageSize and kInternalPageSize in Common.h, force-rebuilds,
# then sweeps all 5 cache sizes for each configuration.
#
# Section B — Fat leaf dilution (kInternalPageSize=1024 fixed):
#   leaf=2KB: cache 32/64/128/256/512 MB, uniform
#   leaf=4KB: cache 32/64/128/256/512 MB, uniform
#
# Section C — Height effect (kLeafPageSize=1024 fixed):
#   inner=512B: cache 32/64/128/256/512 MB, uniform
#   inner=128B: cache 32/64/128/256/512 MB, uniform
#
# Rebuild handshake: node0 patches+rebuilds, signals node1 to patch+rebuild,
# waits for ACK, then starts run signals. Works on separate filesystems.
#
# rpc=0, admit=0.1, 30 threads. Full memcached restart after every run.
#
# USAGE:
#   Node 0 (10.30.1.9): bash node0_pagesize.sh
#   Node 1 (10.30.1.6): bash node1_pagesize.sh   (start at same time)
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DEX_DIR="$REPO_ROOT/dex"
BUILD_DIR="$DEX_DIR/build"
COMMON_H="$DEX_DIR/include/Common.h"
RESULTS_DIR="$SCRIPT_DIR/results_pagesize"
mkdir -p "$RESULTS_DIR"

MEMC_IP="10.30.1.9"
MEMC_PORT="11211"

NODES=2; THREADS=30; MEM_THREADS=4; BULK=50; WARMUP=10; OPS=50
READ=100; INSERT=0; UPDATE=0; DELETE=0; RANGE=0
CHECK=0; TIMEBASE=1; EARLY=0; INDEX=0; RPC=0; ADMIT=0.1; TUNE=0; MAX_THREAD=30

CACHE_SIZES=(32 64 128 256 512)

# ── Helpers ───────────────────────────────────────────────────────────────────
log() { echo "[$(date '+%H:%M:%S')] [NODE0-PS] $*"; }

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

patch_and_rebuild() {
    local inner_sz="$1" leaf_sz="$2" config_label="$3"

    log "Patching Common.h: kInternalPageSize=$inner_sz  kLeafPageSize=$leaf_sz"
    sed -i "s/constexpr uint32_t kInternalPageSize = [0-9]*/constexpr uint32_t kInternalPageSize = $inner_sz/" "$COMMON_H"
    sed -i "s/constexpr uint32_t kLeafPageSize = [0-9]*/constexpr uint32_t kLeafPageSize = $leaf_sz/" "$COMMON_H"
    grep "kInternalPageSize\|kLeafPageSize" "$COMMON_H"

    log "Force-rebuilding newbench_latency (cmake --clean-first)..."
    cd "$BUILD_DIR"
    cmake --build . --target newbench_latency --clean-first -j$(nproc) 2>&1 | tail -10
    cd "$SCRIPT_DIR"

    # Verify constants are baked in by checking alloc_unit printed at startup
    log "Verifying kLeafPageSize baked into binary (alloc_unit should = $leaf_sz)..."
    # alloc_unit is printed from kLeafPageSize; use timeout to avoid blocking
    timeout 5 sudo "$BUILD_DIR/newbench_latency" 1 100 0 0 0 0 2 4 32 1 0.0 50 1 1 0 1 0 0 0 0.1 0 2 2>/dev/null \
        | grep -E "alloc_unit|Inner page size|Leaf page size" | head -5 || true

    # Signal node1 to patch + rebuild its own copy
    log "Signalling node1 to rebuild for config=$config_label..."
    memc_set "node0_config" "$config_label"
    memc_set "node1_rebuild_done" "none"

    # Wait for node1 ACK
    local elapsed=0
    while true; do
        local val; val=$(memc_get "node1_rebuild_done")
        [ "$val" = "$config_label" ] && { log "  node1 rebuild ACK received"; break; }
        (( elapsed % 15 == 0 )) && log "  waiting for node1 rebuild ACK... (${elapsed}s)"
        sleep 3; elapsed=$(( elapsed + 3 ))
        [ "$elapsed" -ge 600 ] && { log "ERROR: timed out waiting for node1 rebuild ACK"; exit 1; }
    done
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
        $THREADS $MEM_THREADS $cache $uniform $theta \
        $BULK $WARMUP $OPS $CHECK $TIMEBASE $EARLY \
        $INDEX $RPC $ADMIT $TUNE $MAX_THREAD \
        2>&1 | tee "$RESULTS_DIR/${label}_node0.log"

    save_results "$label"
    log "Run $label DONE."
    cd "$SCRIPT_DIR"
    sleep 3
}

run_cache_sweep() {
    local prefix="$1" uniform="$2" theta="$3"
    for CACHE in "${CACHE_SIZES[@]}"; do
        run_dex "${prefix}_cache${CACHE}mb" "$CACHE" "$uniform" "$theta"
    done
}

# ═══════════════════════════════════════════════════════════════════════════════
echo ""
log "════════════════════════════════════════════════════"
log "SECTION B: Fat leaf dilution (inner=1024B fixed)"
log "  Sweeps: leaf=2KB and 4KB × 5 cache sizes, uniform"
log "════════════════════════════════════════════════════"

# B1: leaf=2KB
patch_and_rebuild 1024 2048 "inner1024_leaf2048"
run_cache_sweep "b_leaf2kb" 1 0.0

# B2: leaf=4KB
patch_and_rebuild 1024 4096 "inner1024_leaf4096"
run_cache_sweep "b_leaf4kb" 1 0.0

# ═══════════════════════════════════════════════════════════════════════════════
echo ""
log "════════════════════════════════════════════════════"
log "SECTION C: Height effect (leaf=1024B fixed)"
log "  Sweeps: inner=512B and 128B × 5 cache sizes, uniform"
log "════════════════════════════════════════════════════"

# C1: inner=512B (taller tree, fewer keys per node)
patch_and_rebuild 512 1024 "inner512_leaf1024"
run_cache_sweep "c_inner512b" 1 0.0

# C2: inner=128B (much taller tree)
patch_and_rebuild 128 1024 "inner128_leaf1024"
run_cache_sweep "c_inner128b" 1 0.0

# ── Restore defaults ──────────────────────────────────────────────────────────
log "Restoring default page sizes (1024/1024)..."
sed -i "s/constexpr uint32_t kInternalPageSize = [0-9]*/constexpr uint32_t kInternalPageSize = 1024/" "$COMMON_H"
sed -i "s/constexpr uint32_t kLeafPageSize = [0-9]*/constexpr uint32_t kLeafPageSize = 1024/" "$COMMON_H"
memc_set "node0_config" "done"

echo ""
log "════════════════════════════════════════════════════"
log "ALL PAGE SIZE RUNS COMPLETE"
log "Results: $RESULTS_DIR/"
ls -lh "$RESULTS_DIR/"
log "════════════════════════════════════════════════════"
