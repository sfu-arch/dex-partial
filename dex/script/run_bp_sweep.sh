#!/bin/bash
# DEX B+ tree sweep — depth≈10, uniform→zipf, point+range, rpc_rate 0 and 1
#
# Uses newbench so every run gets:
#   throughput  +  [DEX] miss stats  +  Avg. rdma read/op  +  latency percentiles
#
# Node config (compiled into binary — must match btree_node.h):
#   innerNodeSize=256B → fanout=11   leafNodeSize=512B → cap=26
#   bulk=50M keys → depth=10,  dataset≈2.3GB on DSM
#
# All stdout is tee'd to LOGFILE so nothing is lost.

NODENUM=2
MEM_THREADS=4
BULK_M=50
WARMUP_M=10
RUN_M=50
THREADS=36
MAX_THREADS=36
INDEX=0
ADMIT_RATE=0.1
AUTOTUNE=0
TIMEBASED=1
EARLYSTOP=0   # 0 = all threads run to completion → unbiased latency histograms
CORRECT=0

INNER_NODE_SIZE=256
LEAF_NODE_SIZE=512

CACHES=(128 256 512)
RPC_RATES=(0 1)

DISTRIBUTIONS=(
    "uniform   1  0.99"
    "zipf0.30  0  0.30"
    "zipf0.50  0  0.50"
    "zipf0.60  0  0.60"
    "zipf0.99  0  0.99"
)

MEMC_HOST=10.30.1.9
LOGFILE="sweep_$(date +%Y%m%d_%H%M).log"
mkdir -p latency_results

echo "Logging to: ${LOGFILE}"

flush_memc() {
    sudo pkill -9 memcached 2>/dev/null; sleep 2
    sudo memcached -u root -l 0.0.0.0 -p 11211 -c 10000 -d
    sleep 2
    printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_HOST" 11211
    printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_HOST" 11211
}

run_one() {
    local label="$1"
    local read_r="$2"
    local range_r="$3"
    local uni="$4"
    local theta="$5"
    local cache="$6"
    local tag="$7"
    local rpc="$8"

    echo "[SWEEP] op=${label} cache=${cache}MB rpc=${rpc} inner=${INNER_NODE_SIZE}B leaf=${LEAF_NODE_SIZE}B uni=${uni} theta=${theta}"
    # Reset counters before launching so compute node always gets ID 0.
    flush_memc
    sleep 2
    sudo ./newbench \
        $NODENUM \
        $read_r 0 0 0 $range_r \
        $THREADS $MEM_THREADS $cache \
        $uni $theta \
        $BULK_M $WARMUP_M $RUN_M \
        $CORRECT $TIMEBASED $EARLYSTOP \
        $INDEX $rpc $ADMIT_RATE $AUTOTUNE $MAX_THREADS

    [ -f dex_read_latency.dat  ] && mv dex_read_latency.dat  "latency_results/${tag}_rpc${rpc}_read.dat"
    [ -f dex_range_latency.dat ] && mv dex_range_latency.dat "latency_results/${tag}_rpc${rpc}_range.dat"

    echo "[SWEEP_END] op=${label} cache=${cache}MB rpc=${rpc}"
    flush_memc
    echo ""
    sleep 3
}

# ── header ────────────────────────────────────────────────────────────────
{
TOTAL=$(( ${#DISTRIBUTIONS[@]} * ${#CACHES[@]} * 2 * ${#RPC_RATES[@]} ))
echo "======================================================="
echo " DEX | inner=${INNER_NODE_SIZE}B(f=11) leaf=${LEAF_NODE_SIZE}B(cap=26) | depth=10 | bulk=50M"
echo " Cache sweep: ${CACHES[*]} MB"
echo " RPC rates: ${RPC_RATES[*]}"
echo " Distributions: uniform 0.30 0.50 0.60 0.99"
echo " Binary: newbench (throughput + latency + RDMA stats per run)"
echo " Total runs: ${TOTAL}  (~$((TOTAL * 2)) min wall time)"
echo "======================================================="
echo ""

for RPC in "${RPC_RATES[@]}"; do
    echo "######################################################"
    echo " RPC_RATE = ${RPC}  ($([ $RPC -eq 0 ] && echo 'one-sided RDMA only' || echo '100% push-down to memory node'))"
    echo "######################################################"
    echo ""

    # ── POINT LOOKUPS ─────────────────────────────────────────────────────
    echo "===== POINT LOOKUPS (rpc=${RPC}) ====="
    for DIST in "${DISTRIBUTIONS[@]}"; do
        read -r dlabel uni theta <<< "$DIST"
        echo "  >> ${dlabel} (uni=${uni} theta=${theta})"
        for CACHE in "${CACHES[@]}"; do
            run_one "point/${dlabel}" 100 0 $uni $theta $CACHE "point_${dlabel}_${CACHE}mb" $RPC
        done
    done

    # ── RANGE QUERIES ─────────────────────────────────────────────────────
    echo "===== RANGE QUERIES (rpc=${RPC}, scan=100) ====="
    for DIST in "${DISTRIBUTIONS[@]}"; do
        read -r dlabel uni theta <<< "$DIST"
        echo "  >> ${dlabel} (uni=${uni} theta=${theta})"
        for CACHE in "${CACHES[@]}"; do
            run_one "range/${dlabel}" 0 100 $uni $theta $CACHE "range_${dlabel}_${CACHE}mb" $RPC
        done
    done

    echo ""
done

echo "======================================================="
echo " Done: ${TOTAL} runs"
echo " Log file:           ${LOGFILE}"
echo " Latency histograms: latency_results/*.dat"
echo "   grep '[SWEEP]'            → per-run config"
echo "   grep 'rdma read / op'     → remote load per run"
echo "   grep '\[DEX\]'            → cache miss evolution"
echo "   grep 'P99'                → tail latency per run"
echo "   grep 'rpc_rate=1'         → RPC push-down runs only"
echo "======================================================="
} 2>&1 | tee "$LOGFILE"
