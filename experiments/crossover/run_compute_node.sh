#!/bin/bash
# ============================================================
# run_compute_node.sh  —  Run on COMPUTE NODE (10.30.1.6)
#
# Paired with run_memory_node.sh on 10.30.1.9.
# For each (cache_size, workload) pair:
#   1. Polls memcached until the memory node signals "ready_run=<label>"
#   2. Immediately launches newbench_latency to pair with memory node
#   3. Saves stdout log to results/
#
# Start this script FIRST (or at the same time) as run_memory_node.sh.
# It will wait safely for each signal before launching.
# ============================================================
set -euo pipefail

# ── Config ───────────────────────────────────────────────────────────────────
MEMC_HOST="10.30.1.9"
MEMC_PORT="11211"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DEX_BUILD="$REPO_ROOT/dex/build"
RESULTS_DIR="$SCRIPT_DIR/results"
mkdir -p "$RESULTS_DIR"

# Must match run_memory_node.sh exactly
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
EARLY=0          # NO early stop
INDEX=0
RPC=1
ADMIT=0.1
TUNE=0
MAX_THREAD=30

CACHES=(32 64 128 256 512)
declare -A WORKLOAD_UNIFORM=([uniform]=1  [zipf099]=0)
declare -A WORKLOAD_THETA=(  [uniform]=0  [zipf099]=0.99)
# uniform=1 → theta is ignored by newbench_latency (init_key_generator skips zipf init)
# uniform=0 → theta=0.99 activates Zipfian distribution
WORKLOADS=(uniform zipf099)

# ── Helpers ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; CYAN='\033[0;36m'; NC='\033[0m'
info()    { echo -e "${GREEN}[+]${NC} $*"; }
warn()    { echo -e "${YELLOW}[!]${NC} $*"; }
error()   { echo -e "${RED}[x]${NC} $*"; exit 1; }
section() { echo -e "\n${CYAN}══════════════════════════════════════════${NC}"; echo -e "${CYAN}  $*${NC}"; echo -e "${CYAN}══════════════════════════════════════════${NC}"; }

get_memcached_key() {
    local key="$1"
    printf "get %s\r\nquit\r\n" "$key" \
        | timeout 3 nc "${MEMC_HOST}" "${MEMC_PORT}" 2>/dev/null \
        | grep -v "^VALUE\|^END" | tr -d '\r\n ' \
        || echo ""
}

wait_for_run() {
    local expected_label="$1"
    local timeout_s=300   # 5 minutes max wait per run
    local elapsed=0

    info "Waiting for memory node to signal: ready_run=${expected_label}"
    while true; do
        # Check if memcached is reachable at all
        if ! echo "version" | timeout 2 nc "${MEMC_HOST}" "${MEMC_PORT}" 2>/dev/null | grep -q "VERSION"; then
            warn "  memcached not reachable yet (${elapsed}s elapsed)..."
            sleep 2; elapsed=$(( elapsed + 2 ))
            [ "$elapsed" -ge "$timeout_s" ] && error "Timed out waiting for memcached after ${timeout_s}s"
            continue
        fi

        # Check if the expected run signal is set
        CURRENT=$(get_memcached_key "ready_run")
        if [ "$CURRENT" = "$expected_label" ]; then
            info "  Signal received: ready_run=${expected_label} — launching now"
            return 0
        fi

        # Still waiting — print status every 5s
        if (( elapsed % 5 == 0 )); then
            info "  ready_run='${CURRENT}' (want '${expected_label}') — waiting... (${elapsed}s)"
        fi
        sleep 1; elapsed=$(( elapsed + 1 ))
        [ "$elapsed" -ge "$timeout_s" ] && error "Timed out after ${timeout_s}s waiting for run ${expected_label}"
    done
}

run_dex() {
    local label="$1" cache="$2" uniform="$3" theta="$4"
    local out_prefix="$RESULTS_DIR/dex_${label}_compute"

    section "DEX  label=${label}  cache=${cache}MB  uniform=${uniform}  theta=${theta}"

    # Block until memory node is ready for this exact run
    wait_for_run "${label}"

    # cd to dex/build so ../memcached.conf resolves correctly
    cd "$DEX_BUILD"

    info "Launching newbench_latency..."
    sudo ./newbench_latency \
        $NODES $READ $INSERT $UPDATE $DELETE $RANGE \
        $THREADS $MEM_THREADS \
        $cache \
        $uniform $theta \
        $BULK $WARMUP $OPS \
        $CHECK $TIMEBASE $EARLY \
        $INDEX $RPC $ADMIT $TUNE $MAX_THREAD \
        2>&1 | tee "${out_prefix}_stdout.log"

    cd "$SCRIPT_DIR"
    info "Run ${label} complete on compute node."
}

# ── Main sweep ───────────────────────────────────────────────────────────────
echo ""
info "DEX Cache Crossover Experiment — Compute Node (10.30.1.6)"
info "Sweep: caches=${CACHES[*]}  workloads=${WORKLOADS[*]}"
info "Results → ${RESULTS_DIR}/"
echo ""
info "Waiting for memory node (${MEMC_HOST}) to start run_memory_node.sh..."
echo ""

TOTAL=$(( ${#CACHES[@]} * ${#WORKLOADS[@]} ))
RUN=0

for WORKLOAD in "${WORKLOADS[@]}"; do
    for CACHE in "${CACHES[@]}"; do
        RUN=$(( RUN + 1 ))
        LABEL="${WORKLOAD}_cache${CACHE}"
        UNIFORM="${WORKLOAD_UNIFORM[$WORKLOAD]}"
        THETA="${WORKLOAD_THETA[$WORKLOAD]}"
        info "Run ${RUN}/${TOTAL}: ${LABEL}"
        run_dex "$LABEL" "$CACHE" "$UNIFORM" "$THETA"
    done
done

section "All ${TOTAL} runs complete on compute node"
info "Stdout logs saved in: ${RESULTS_DIR}/"
ls -lh "$RESULTS_DIR/"
