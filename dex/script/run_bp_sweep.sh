#!/bin/bash
# DEX B+ tree depth-10 cache sweep: uniform vs zipf, point vs range
#
# Node config:
#   pageSize=192B  → inner fanout=7, leaf capacity=6
#   bulk=50M keys  → depth≈10 (levels 0-9), dataset≈2GB on DSM
#   megaLevel=4    → sub-trees of levels 0-3 co-located per paper
#
# What this shows:
#   - Uniform workload: low cache hit rate → high RDMA reads/op
#   - As cache grows 128→256→512MB: remote load drops even under uniform
#   - Zipf at same cache sizes: much lower remote load (skew helps caching)
#   - Range queries: no offloading (per paper §7), pure one-sided RDMA
#
# Run on compute node; memory node runs same binary (acts as server).
# Stats printed per 2s interval: [DEX] inner_miss leaf_miss dirty_wb total_remote
# Final stats: Avg. rdma read/op, write/op, rpc/op, all/op, read size/op

NODENUM=2
MEM_THREADS=4
BULK_M=50          # 50M keys → depth≈10 with innerNodeSize=192/leafNodeSize=192
WARMUP_M=10
RUN_M=50
THREADS=36
MAX_THREADS=36
INDEX=0            # DEX only
RPC_RATE=0
ADMIT_RATE=0.1     # lazy leaf admission (paper default)
AUTOTUNE=0
TIMEBASED=1
EARLYSTOP=1
CORRECT=0

# ── Node sizes (must match btree_node.h at compile time) ──────────────────
# Edit innerNodeSize / leafNodeSize in include/cache/btree_node.h, rebuild,
# then update these for correct labelling in output.
INNER_NODE_SIZE=256   # → inner fanout  = (256-72)/16 = 11
LEAF_NODE_SIZE=512    # → leaf capacity = (512-96)/16 = 26   (fat leaf)

# Cache sizes to sweep (MB) — this is the main independent variable for
# remote-load tracking.  More cache → fewer RDMA reads per op.
CACHES=(128 256 512)

# Distribution sweep: uniform first, then increasing skew
# uniform_workload=1 ignores theta; for zipf entries use uniform_workload=0
# Format: "label  uniform_flag  theta"
DISTRIBUTIONS=(
    "uniform   1  0.99"   # uniform (theta unused but must be valid)
    "zipf0.30  0  0.30"
    "zipf0.50  0  0.50"
    "zipf0.60  0  0.60"
    "zipf0.99  0  0.99"
)

MEMC_HOST=10.30.1.9   # memcached server IP for counter reset

# Reset memcached and zero the serverNum/clientNum counters so the next run
# can complete its rendezvous cleanly.  Called AFTER each benchmark binary exits.
flush_memc() {
    sudo pkill -9 memcached 2>/dev/null; sleep 2
    sudo memcached -u root -l 0.0.0.0 -p 11211 -c 10000 -d
    sleep 2
    printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_HOST" 11211
    printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_HOST" 11211
}

echo "======================================================="
echo " DEX B+ Tree | inner=256B(f=11) leaf=512B(cap=26) | depth≈10 | bulk=50M"
echo " Cache sweep: ${CACHES[*]} MB"
echo " Distributions: uniform → zipf 0.30 → 0.50 → 0.60 → 0.99"
echo " Index=DEX, rpc_rate=${RPC_RATE}, admit_rate=${ADMIT_RATE}"
echo "======================================================="
echo ""

run_one() {
    local label="$1"
    local read_r="$2"
    local range_r="$3"
    local uni="$4"
    local theta="$5"
    local cache="$6"

    # Structured tag — grep this line then take next "Avg. rdma read / op" line
    echo "[SWEEP] op=${label} cache=${cache}MB inner=${INNER_NODE_SIZE}B leaf=${LEAF_NODE_SIZE}B uni=${uni} theta=${theta}"
    ./restartMemc.sh
    sudo ./newbench \
        $NODENUM \
        $read_r 0 0 0 $range_r \
        $THREADS $MEM_THREADS $cache \
        $uni $theta \
        $BULK_M $WARMUP_M $RUN_M \
        $CORRECT $TIMEBASED $EARLYSTOP \
        $INDEX $RPC_RATE $ADMIT_RATE $AUTOTUNE $MAX_THREADS
    echo "[SWEEP_END] op=${label} cache=${cache}MB"
    flush_memc
    echo ""
    sleep 3
}

# ============================================================
# 1. POINT LOOKUPS
#    Sweep: uniform → zipf 0.30 → 0.50 → 0.60 → 0.99
#    Per distribution, sweep cache: 128 → 256 → 512 MB
#    Watch: rdma_read/op drops as θ↑ (more skew = better cache hit)
#           rdma_read/op drops as cache↑ (more pages fit in cache)
# ============================================================
echo "===== POINT LOOKUPS ====="
for DIST in "${DISTRIBUTIONS[@]}"; do
    read -r dlabel uni theta <<< "$DIST"
    echo "  >> distribution: ${dlabel} (uni=${uni} theta=${theta})"
    for CACHE in "${CACHES[@]}"; do
        run_one "point/${dlabel}" 100 0 $uni $theta $CACHE
    done
done

# ============================================================
# 2. RANGE QUERIES (scan_num=100 keys per scan, no offloading)
#    Same distribution sweep + cache sweep.
#    Watch: higher rdma_read/op than point (must traverse multiple leaves)
#           uniform is worst case — consecutive leaf pages not cached
# ============================================================
echo "===== RANGE QUERIES (scan=100) ====="
for DIST in "${DISTRIBUTIONS[@]}"; do
    read -r dlabel uni theta <<< "$DIST"
    echo "  >> distribution: ${dlabel} (uni=${uni} theta=${theta})"
    for CACHE in "${CACHES[@]}"; do
        run_one "range/${dlabel}" 0 100 $uni $theta $CACHE
    done
done

echo "======================================================="
echo " Throughput sweep done: $(( ${#DISTRIBUTIONS[@]} * ${#CACHES[@]} * 2 )) runs"
echo "======================================================="
echo ""

# ============================================================
# LATENCY SWEEP — uses newbench_latency binary
#
# Runs all 5 distributions × 2 workload types at 256MB cache.
# After each run the hardcoded output files (dex_read_latency.dat /
# dex_range_latency.dat) are moved to uniquely named files so they
# are not overwritten.
#
# Output per run (printed to stdout + saved to .dat file):
#   P50 / P90 / P95 / P99 / P99.9 latency in nanoseconds
#   Raw histogram: latency_ns <TAB> count  (500ns bucket granularity)
# ============================================================
LATENCY_CACHE=256   # fixed cache for latency sweep
mkdir -p latency_results

run_latency() {
    local label="$1"
    local read_r="$2"
    local range_r="$3"
    local uni="$4"
    local theta="$5"
    local cache="$6"
    local tag="$7"   # used to name the saved .dat files

    echo "--- [LATENCY] ${label} | cache=${cache}MB ---"
    ./restartMemc.sh
    sudo ./newbench_latency \
        $NODENUM \
        $read_r 0 0 0 $range_r \
        $THREADS $MEM_THREADS $cache \
        $uni $theta \
        $BULK_M $WARMUP_M $RUN_M \
        $CORRECT $TIMEBASED 0 \
        $INDEX $RPC_RATE $ADMIT_RATE $AUTOTUNE $MAX_THREADS

    # Move hardcoded output files to per-config names
    [ -f dex_read_latency.dat  ] && mv dex_read_latency.dat  "latency_results/${tag}_read.dat"
    [ -f dex_range_latency.dat ] && mv dex_range_latency.dat "latency_results/${tag}_range.dat"

    echo "--- [LATENCY] done: ${label} → latency_results/${tag}_{read,range}.dat ---"
    flush_memc
    echo ""
    sleep 3
}

echo "===== LATENCY: POINT LOOKUPS (cache=${LATENCY_CACHE}MB) ====="
for DIST in "${DISTRIBUTIONS[@]}"; do
    read -r dlabel uni theta <<< "$DIST"
    tag="point_${dlabel}_${LATENCY_CACHE}mb"
    run_latency "point/${dlabel}" 100 0 $uni $theta $LATENCY_CACHE "$tag"
done

echo "===== LATENCY: RANGE QUERIES (cache=${LATENCY_CACHE}MB) ====="
for DIST in "${DISTRIBUTIONS[@]}"; do
    read -r dlabel uni theta <<< "$DIST"
    tag="range_${dlabel}_${LATENCY_CACHE}mb"
    run_latency "range/${dlabel}" 0 100 $uni $theta $LATENCY_CACHE "$tag"
done

echo "======================================================="
echo " All sweeps complete."
echo ""
echo " Throughput + RDMA stats: in stdout above (grep 'rdma read / op')"
echo " Latency histograms:      latency_results/*.dat"
echo "   Filename pattern: {point|range}_{dist}_{cache}mb_{read|range}.dat"
echo "   Each file: # P50/P90/P95/P99/P99.9 in header, then latency_ns<TAB>count"
echo "======================================================="
