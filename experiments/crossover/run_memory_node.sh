#!/bin/bash
# ============================================================
# run_memory_node.sh  —  Run on MEMORY NODE (10.30.1.9)
#
# Runs the full DEX cache-crossover experiment sweep.
# For each (cache_size, workload) pair:
#   1. Restarts memcached LOCALLY and resets all sync counters
#   2. Writes a "ready_<run_id>" signal key into memcached
#   3. Launches newbench_latency (waits for compute node to connect)
#   4. Saves latency .dat files to results/
#
# Paired script: run_compute_node.sh must be running on 10.30.1.6
# ============================================================
set -euo pipefail

# ── Config ───────────────────────────────────────────────────────────────────
MEMC_HOST="10.30.1.9"
MEMC_PORT="11211"
PID_FILE="/tmp/memcached.pid"

# Paths — scripts live in experiments/crossover/, binary in dex/build/
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
DEX_BUILD="$REPO_ROOT/dex/build"
RESULTS_DIR="$SCRIPT_DIR/results"
mkdir -p "$RESULTS_DIR"

# Experiment parameters (fixed across all runs)
NODES=2          # 1 memory + 1 compute
THREADS=30       # client threads
MEM_THREADS=4    # memory node threads
BULK=50          # 50M keys bulk load
WARMUP=10        # 10M warmup ops (not measured)
OPS=50           # 50M measured ops
READ=100         # 100% point reads
INSERT=0; UPDATE=0; DELETE=0; RANGE=0
CHECK=0          # no correctness check
TIMEBASE=1       # op-count based run
EARLY=0          # NO early stop — full run always
INDEX=0          # 0 = DEX
RPC=1
ADMIT=0.1
TUNE=0
MAX_THREAD=30

# Sweep variables
CACHES=(32 64 128 256 512)
# uniform: UNIFORM=1 theta ignored
# zipf099: UNIFORM=0 theta=0.99
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

restart_memcached() {
    info "Restarting memcached on localhost (${MEMC_HOST}:${MEMC_PORT})..."

    # Kill existing instances — sudo required when memcached runs as another user
    if [ -f "$PID_FILE" ]; then
        PID=$(cat "$PID_FILE")
        sudo kill "$PID" 2>/dev/null && info "  killed pid $PID" || warn "  stale pid file"
        sudo rm -f "$PID_FILE"
    fi
    if pgrep memcached >/dev/null 2>&1; then
        sudo pkill memcached && info "  pkill done" || true
    fi

    # Wait for port to be fully released (avoids TIME_WAIT bind failure)
    info "  waiting for port ${MEMC_PORT} to be free..."
    for i in $(seq 1 20); do
        if ! ss -tlnp 2>/dev/null | grep -q ":${MEMC_PORT}" && \
           ! ss -tnp  2>/dev/null | grep -q ":${MEMC_PORT}"; then
            info "  port ${MEMC_PORT} is free"
            break
        fi
        [ "$i" -eq 20 ] && warn "  port still in use after 10s — attempting start anyway"
        sleep 0.5
    done

    # Start fresh, bound to cluster IP (not 127.0.0.1)
    sudo memcached -u nobody -l "${MEMC_HOST}" -p "${MEMC_PORT}" -c 10000 -m 64 -d -P "${PID_FILE}" \
        || sudo memcached -l "${MEMC_HOST}" -p "${MEMC_PORT}" -c 10000 -m 64 -d -P "${PID_FILE}"
    sleep 1

    # Verify it's up
    for attempt in 1 2 3 4 5; do
        if echo "version" | timeout 2 nc "${MEMC_HOST}" "${MEMC_PORT}" 2>/dev/null | grep -q "VERSION"; then
            info "  memcached is up"
            break
        fi
        [ "$attempt" -eq 5 ] && error "memcached did not come up after 5 attempts"
        warn "  attempt $attempt failed, retrying..."
        sleep 1
    done

    # Reset sync counters — stale values cause infinite hangs in DSMKeeper
    printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | timeout 3 nc "${MEMC_HOST}" "${MEMC_PORT}" > /dev/null
    printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | timeout 3 nc "${MEMC_HOST}" "${MEMC_PORT}" > /dev/null
    info "  serverNum=0  clientNum=0  reset"
}

signal_compute_ready() {
    local run_id="$1"
    # Write the run_id so compute node knows exactly which run to expect
    printf "set ready_run 0 0 %d\r\n%s\r\nquit\r\n" "${#run_id}" "${run_id}" \
        | timeout 3 nc "${MEMC_HOST}" "${MEMC_PORT}" > /dev/null
    info "  signalled compute node: ready_run=${run_id}"
}

run_dex() {
    local label="$1" cache="$2" uniform="$3" theta="$4"
    local out_prefix="$RESULTS_DIR/dex_${label}"

    section "DEX  label=${label}  cache=${cache}MB  uniform=${uniform}  theta=${theta}"

    # Restart memcached and reset counters for this run
    restart_memcached

    # Signal the compute node — it will start polling and launch when it sees this
    signal_compute_ready "${label}"

    # cd to dex/build so ../memcached.conf resolves correctly
    cd "$DEX_BUILD"

    info "Launching newbench_latency (waiting for compute node to connect)..."
    sudo ./newbench_latency \
        $NODES $READ $INSERT $UPDATE $DELETE $RANGE \
        $THREADS $MEM_THREADS \
        $cache \
        $uniform $theta \
        $BULK $WARMUP $OPS \
        $CHECK $TIMEBASE $EARLY \
        $INDEX $RPC $ADMIT $TUNE $MAX_THREAD \
        2>&1 | tee "${out_prefix}_stdout.log"

    # Save latency histograms
    [ -f "dex_read_latency.dat"  ] && mv "dex_read_latency.dat"  "${out_prefix}_read_latency.dat"  && info "  saved ${out_prefix}_read_latency.dat"
    [ -f "dex_range_latency.dat" ] && mv "dex_range_latency.dat" "${out_prefix}_range_latency.dat" && info "  saved ${out_prefix}_range_latency.dat"

    cd "$SCRIPT_DIR"
    info "Run ${label} complete."
    sleep 3   # brief pause before next run
}

# ── Main sweep ───────────────────────────────────────────────────────────────
echo ""
info "DEX Cache Crossover Experiment — Memory Node (${MEMC_HOST})"
info "Sweep: caches=${CACHES[*]}  workloads=${WORKLOADS[*]}"
info "Results → ${RESULTS_DIR}/"
echo ""
warn "Ensure run_compute_node.sh is running on 10.30.1.6 BEFORE proceeding."
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

section "All ${TOTAL} runs complete"
info "Results saved in: ${RESULTS_DIR}/"
ls -lh "$RESULTS_DIR/"
