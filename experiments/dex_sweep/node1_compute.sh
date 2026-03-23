#!/bin/bash
###############################################################################
# DEX Sweep — Node 1 (Compute Node, 10.30.1.6)
#
# Polls memcached for rebuild signals from node0, patches Common.h locally,
# rebuilds newbench_latency, ACKs node0, then runs each benchmark pair.
#
# Works on BOTH shared NFS and separate filesystems.
# Start at the same time as node0_memory.sh.
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

CACHE_SIZES=(32 64 128 256 512)

# ── Helpers ───────────────────────────────────────────────────────────────────
log() { echo "[$(date '+%H:%M:%S')] [NODE1] $*"; }

memc_set() {
    printf "set %s 0 0 %d\r\n%s\r\nquit\r\n" "$1" "${#2}" "$2" \
        | nc -w 2 "$MEMC_IP" "$MEMC_PORT" > /dev/null
}

memc_get() {
    printf "get %s\r\nquit\r\n" "$1" \
        | nc -w 3 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null \
        | grep -v "^VALUE\|^END\|^$" | tr -d '\r\n '
}

# Poll until node0_config changes to the expected value, then rebuild locally.
wait_and_rebuild() {
    local expected_config="$1"
    local elapsed=0
    log "Waiting for node0_config=$expected_config ..."

    while true; do
        local val
        val=$(memc_get "node0_config")
        if [ "$val" = "$expected_config" ]; then
            log "  Config signal received: $expected_config — rebuilding..."
            break
        fi
        (( elapsed % 15 == 0 )) && log "  node0_config='$val' (want '$expected_config') — ${elapsed}s"
        sleep 3; elapsed=$(( elapsed + 3 ))
        [ "$elapsed" -ge 600 ] && { log "ERROR: timed out waiting for config $expected_config"; exit 1; }
    done

    # Parse config label: format is "inner<N>_leaf<M>"
    local inner_sz leaf_sz
    inner_sz=$(echo "$expected_config" | sed 's/inner\([0-9]*\)_leaf\([0-9]*\)/\1/')
    leaf_sz=$(echo  "$expected_config" | sed 's/inner\([0-9]*\)_leaf\([0-9]*\)/\2/')

    log "Patching Common.h: kInternalPageSize=$inner_sz  kLeafPageSize=$leaf_sz"
    sed -i "s/constexpr uint32_t kInternalPageSize = [0-9]*/constexpr uint32_t kInternalPageSize = $inner_sz/" "$COMMON_H"
    sed -i "s/constexpr uint32_t kLeafPageSize = [0-9]*/constexpr uint32_t kLeafPageSize = $leaf_sz/" "$COMMON_H"
    grep "kInternalPageSize\|kLeafPageSize" "$COMMON_H"

    log "Rebuilding newbench_latency..."
    cd "$BUILD_DIR"
    make -j$(nproc) newbench_latency 2>&1 | tail -3
    log "Build OK."
    cd "$SCRIPT_DIR"

    # ACK to node0
    memc_set "node1_rebuild_done" "$expected_config"
    log "  ACK sent: node1_rebuild_done=$expected_config"
}

wait_for_run() {
    local expected="$1"
    local elapsed=0
    while true; do
        local val
        val=$(memc_get "node0_ready")
        [ "$val" = "$expected" ] && { log "  Run signal received: $expected"; return 0; }
        (( elapsed % 10 == 0 )) && log "  node0_ready='$val' (want '$expected') — ${elapsed}s"
        sleep 2; elapsed=$(( elapsed + 2 ))
        [ "$elapsed" -ge 600 ] && { log "ERROR: timed out waiting for run $expected"; exit 1; }
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

run_cache_sweep_uniform() {
    local prefix="$1"
    for CACHE in "${CACHE_SIZES[@]}"; do
        run_dex "${prefix}_uniform_cache${CACHE}mb" "$CACHE" 1 0.0
    done
}

# ═══════════════════════════════════════════════════════════════════════════════
# SECTION A+D
# ═══════════════════════════════════════════════════════════════════════════════
log "SECTION A+D: Cache cliff & skew recovery"
wait_and_rebuild "inner1024_leaf1024"

for CACHE in "${CACHE_SIZES[@]}"; do
    run_dex "ad_inner1024_leaf1024_uniform_cache${CACHE}mb" "$CACHE" 1 0.0
done
for CACHE in "${CACHE_SIZES[@]}"; do
    run_dex "ad_inner1024_leaf1024_zipf099_cache${CACHE}mb" "$CACHE" 0 0.99
done

# ═══════════════════════════════════════════════════════════════════════════════
# SECTION B
# ═══════════════════════════════════════════════════════════════════════════════
log "SECTION B: Fat leaf dilution"

wait_and_rebuild "inner1024_leaf2048"
run_cache_sweep_uniform "b_inner1024_leaf2048"

wait_and_rebuild "inner1024_leaf4096"
run_cache_sweep_uniform "b_inner1024_leaf4096"

# ═══════════════════════════════════════════════════════════════════════════════
# SECTION C
# ═══════════════════════════════════════════════════════════════════════════════
log "SECTION C: Height effect"

wait_and_rebuild "inner512_leaf1024"
run_cache_sweep_uniform "c_inner512_leaf1024"

wait_and_rebuild "inner128_leaf1024"
run_cache_sweep_uniform "c_inner128_leaf1024"

log "ALL SECTIONS COMPLETE"
ls -lh "$RESULTS_DIR/"
