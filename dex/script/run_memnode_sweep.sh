#!/bin/bash
# Memory node sweep -- mirrors run_bp_sweep.sh on the compute node exactly.
#
# HOW IT WORKS:
#   1. After each experiment the compute node calls flush_memc which sets
#      serverNum=0 in memcached.  This script polls for that reset as the
#      signal to start the next binary.
#   2. After the binary exits this script resets the counters via nc (no
#      local memcached restart -- memcached lives on the compute node).
#
# USAGE:
#   On memory node:  ./run_memnode_sweep.sh
#   On compute node: ./run_bp_sweep.sh        (start roughly at same time)
#
# The memory node binary will wait in DSMKeeper until the compute node
# connects, so exact timing is not critical -- just start within ~60s.

NODENUM=2
MEM_THREADS=4
BULK_M=50
WARMUP_M=10
RUN_M=50
THREADS=36
MAX_THREADS=36
INDEX=0
RPC_RATE=0
ADMIT_RATE=0.1
AUTOTUNE=0
TIMEBASED=1
CORRECT=0

MEMC_HOST=10.30.1.9
MEMC_PORT=11211

CACHES=(128 256 512)

# Must match compute node's DISTRIBUTIONS exactly (same order)
DISTRIBUTIONS=(
    "uniform   1  0.99"
    "zipf0.30  0  0.30"
    "zipf0.50  0  0.50"
    "zipf0.60  0  0.60"
    "zipf0.99  0  0.99"
)

LOGFILE="memnode_sweep_$(date +%Y%m%d_%H%M).log"

# ── helpers ───────────────────────────────────────────────────────────────

# Read serverNum from memcached
get_server_num() {
    printf "get serverNum\r\n" \
        | nc -w 2 "$MEMC_HOST" "$MEMC_PORT" 2>/dev/null \
        | awk '/^[0-9]/{print $1; exit}'
}

# Block until compute node's flush_memc has set serverNum=0
wait_for_reset() {
    echo "[MEM] waiting for serverNum=0 (compute flush) ..."
    local waited=0
    while true; do
        local val
        val=$(get_server_num)
        if [ "$val" = "0" ]; then
            echo "[MEM] reset detected after ${waited}s"
            break
        fi
        sleep 1
        (( waited++ ))
        if (( waited % 10 == 0 )); then
            echo "[MEM] still waiting... serverNum=${val}"
        fi
    done
    # Give compute node time to finish restartMemc before we register
    sleep 2
}

# After our binary exits: reset counters via nc only (no local memcached)
flush_counters() {
    printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_HOST" "$MEMC_PORT"
    printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_HOST" "$MEMC_PORT"
}

# ── per-experiment runner ─────────────────────────────────────────────────

run_exp() {
    local label="$1"
    local read_r="$2"
    local range_r="$3"
    local uni="$4"
    local theta="$5"
    local cache="$6"

    wait_for_reset

    echo "[MEM][START] op=${label} cache=${cache}MB uni=${uni} theta=${theta}"
    sudo ./newbench_latency \
        $NODENUM \
        $read_r 0 0 0 $range_r \
        $THREADS $MEM_THREADS $cache \
        $uni $theta \
        $BULK_M $WARMUP_M $RUN_M \
        $CORRECT $TIMEBASED 0 \
        $INDEX $RPC_RATE $ADMIT_RATE $AUTOTUNE $MAX_THREADS
    echo "[MEM][END]   op=${label} cache=${cache}MB"

    # Reset counters so next wait_for_reset starts from a known state
    flush_counters
    sleep 3
}

# ── main ─────────────────────────────────────────────────────────────────

{
TOTAL=$(( ${#DISTRIBUTIONS[@]} * ${#CACHES[@]} * 2 ))
echo "======================================================="
echo " Memory node sweep | ${TOTAL} experiments"
echo " MEMC_HOST=${MEMC_HOST}  binary=newbench_latency"
echo "======================================================="
echo ""

echo "===== POINT LOOKUPS ====="
for DIST in "${DISTRIBUTIONS[@]}"; do
    read -r dlabel uni theta <<< "$DIST"
    echo "  >> ${dlabel}"
    for CACHE in "${CACHES[@]}"; do
        run_exp "point/${dlabel}" 100 0 $uni $theta $CACHE
    done
done

echo "===== RANGE QUERIES (scan=100) ====="
for DIST in "${DISTRIBUTIONS[@]}"; do
    read -r dlabel uni theta <<< "$DIST"
    echo "  >> ${dlabel}"
    for CACHE in "${CACHES[@]}"; do
        run_exp "range/${dlabel}" 0 100 $uni $theta $CACHE
    done
done

echo "======================================================="
echo " Memory node sweep complete."
echo " Log: ${LOGFILE}"
echo "======================================================="
} 2>&1 | tee "$LOGFILE"
