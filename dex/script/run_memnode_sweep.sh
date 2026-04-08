#!/bin/bash
# Memory node sweep -- mirrors run_bp_sweep.sh on the compute node exactly.
#
# HOW IT WORKS:
#   Mirrors run_other.sh: just launches ./newbench for each experiment.
#   DSMKeeper's serverEnter()/serverConnect() handle rendezvous internally.
#   The compute node (run_bp_sweep.sh) resets memcached counters before each
#   run, so it always gets ID 0.  This script's binary gets ID 1.
#
# USAGE:
#   On compute node: ./run_bp_sweep.sh
#   On memory node:  ./run_memnode_sweep.sh   (start roughly at same time)
#
# Start order does not matter — DSMKeeper waits for all nodes internally.

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

# ── per-experiment runner ─────────────────────────────────────────────────

run_exp() {
    local label="$1"
    local read_r="$2"
    local range_r="$3"
    local uni="$4"
    local theta="$5"
    local cache="$6"

    echo "[MEM][START] op=${label} cache=${cache}MB uni=${uni} theta=${theta}"
    sudo ./newbench \
        $NODENUM \
        $read_r 0 0 0 $range_r \
        $THREADS $MEM_THREADS $cache \
        $uni $theta \
        $BULK_M $WARMUP_M $RUN_M \
        $CORRECT $TIMEBASED 0 \
        $INDEX $RPC_RATE $ADMIT_RATE $AUTOTUNE $MAX_THREADS
    echo "[MEM][END]   op=${label} cache=${cache}MB"

    # No counter reset here — compute node handles that before each run.
    # Just sleep briefly to let both sides finish cleanly.
    sleep 10
}

# ── main ─────────────────────────────────────────────────────────────────

{
TOTAL=$(( ${#DISTRIBUTIONS[@]} * ${#CACHES[@]} * 2 ))
echo "======================================================="
echo " Memory node sweep | ${TOTAL} experiments"
echo " MEMC_HOST=${MEMC_HOST}  binary=newbench"
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
