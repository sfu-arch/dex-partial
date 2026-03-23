#!/bin/bash
###############################################################################
# DEX Page Size Sweep — Node 1 (Compute Node, 10.30.1.6)
#
# Paired with node0_pagesize.sh. Waits for rebuild signals from node0,
# patches Common.h locally, force-rebuilds, ACKs, then runs benchmarks.
# Start at the same time as node0_pagesize.sh.
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

log() { echo "[$(date '+%H:%M:%S')] [NODE1-PS] $*"; }

memc_set() {
    printf "set %s 0 0 %d\r\n%s\r\nquit\r\n" "$1" "${#2}" "$2" \
        | nc -w 2 "$MEMC_IP" "$MEMC_PORT" > /dev/null
}

memc_get() {
    printf "get %s\r\nquit\r\n" "$1" \
        | nc -w 3 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null \
        | grep -v "^VALUE\|^END\|^$" | tr -d '\r\n '
}

# Wait for node0 to signal a config, then patch + rebuild locally
wait_and_rebuild() {
    local expected_config="$1"
    local elapsed=0
    log "Waiting for node0_config=$expected_config ..."

    while true; do
        local val; val=$(memc_get "node0_config")
        [ "$val" = "$expected_config" ] && { log "  Config signal received"; break; }
        (( elapsed % 15 == 0 )) && log "  node0_config='$val' (want '$expected_config') — ${elapsed}s"
        sleep 3; elapsed=$(( elapsed + 3 ))
        [ "$elapsed" -ge 600 ] && { log "ERROR: timed out waiting for config $expected_config"; exit 1; }
    done

    # Parse inner/leaf from label format "inner<N>_leaf<M>"
    local inner_sz leaf_sz
    inner_sz=$(echo "$expected_config" | sed 's/inner\([0-9]*\)_leaf\([0-9]*\)/\1/')
    leaf_sz=$(echo  "$expected_config" | sed 's/inner\([0-9]*\)_leaf\([0-9]*\)/\2/')

    log "Patching Common.h: kInternalPageSize=$inner_sz  kLeafPageSize=$leaf_sz"
    sed -i "s/constexpr uint32_t kInternalPageSize = [0-9]*/constexpr uint32_t kInternalPageSize = $inner_sz/" "$COMMON_H"
    sed -i "s/constexpr uint32_t kLeafPageSize = [0-9]*/constexpr uint32_t kLeafPageSize = $leaf_sz/" "$COMMON_H"
    grep "kInternalPageSize\|kLeafPageSize" "$COMMON_H"

    log "Force-rebuilding newbench_latency (cmake --clean-first)..."
    cd "$BUILD_DIR"
    cmake --build . --target newbench_latency --clean-first -j$(nproc) 2>&1 | tail -10
    cd "$SCRIPT_DIR"

    log "Verifying binary (alloc_unit should = $leaf_sz)..."
    timeout 5 sudo "$BUILD_DIR/newbench_latency" 1 100 0 0 0 0 2 4 32 1 0.0 50 1 1 0 1 0 0 0 0.1 0 2 2>/dev/null \
        | grep -E "alloc_unit|Inner page size|Leaf page size" | head -5 || true

    # ACK to node0
    memc_set "node1_rebuild_done" "$expected_config"
    log "  ACK sent: node1_rebuild_done=$expected_config"
}

wait_for_run() {
    local expected="$1"
    local elapsed=0
    while true; do
        local val; val=$(memc_get "node0_ready")
        [ "$val" = "$expected" ] && { log "  Run signal received: $expected"; return 0; }
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

run_cache_sweep() {
    local prefix="$1" uniform="$2" theta="$3"
    for CACHE in "${CACHE_SIZES[@]}"; do
        run_dex "${prefix}_cache${CACHE}mb" "$CACHE" "$uniform" "$theta"
    done
}

# ── SECTION B ─────────────────────────────────────────────────────────────────
log "SECTION B: Fat leaf dilution"

wait_and_rebuild "inner1024_leaf2048"
run_cache_sweep "b_leaf2kb" 1 0.0

wait_and_rebuild "inner1024_leaf4096"
run_cache_sweep "b_leaf4kb" 1 0.0

# ── SECTION C ─────────────────────────────────────────────────────────────────
log "SECTION C: Height effect"

wait_and_rebuild "inner512_leaf1024"
run_cache_sweep "c_inner512b" 1 0.0

wait_and_rebuild "inner128_leaf1024"
run_cache_sweep "c_inner128b" 1 0.0

log "ALL PAGE SIZE RUNS COMPLETE"
ls -lh "$RESULTS_DIR/"
