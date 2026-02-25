#!/bin/bash
###############################################################################
# TEST: CHIME 100M Keys - Node 0 (Primary)
# Uses chime_bench (no code changes needed)
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHIME_DIR="$(cd "$SCRIPT_DIR/../../CHIME" && pwd)"
CHIME_BUILD_DIR="$CHIME_DIR/build"
RESULTS_DIR="$SCRIPT_DIR/results/test"
MEMC_IP=$(head -1 "$CHIME_DIR/memcached.conf")
MEMC_PORT=$(sed -n '2p' "$CHIME_DIR/memcached.conf")
mkdir -p "$RESULTS_DIR"

# ===================== CONFIGURATION =====================
NODE_COUNT=2
THREAD_COUNT=30
READ_RATIO=100        # 100% reads
ZIPF_THETA=0.0        # 0.0 = uniform distribution
BULK_LOAD_M=100       # 100M keys
OPS_M=10              # 10M operations

# ===================== CLEANUP & SETUP =====================
echo ">>> [cleanup] Killing stale processes..."
sudo pkill -9 chime_bench 2>/dev/null || true
sudo pkill -9 memcached 2>/dev/null || true
sleep 2
sudo rm -f /dev/shm/dsm_* /dev/shm/rdma_* 2>/dev/null || true

# Hugepages
echo ">>> [hugepages] Setting 36864 pages..."
echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
ulimit -l unlimited 2>/dev/null || true

# Start memcached
echo ">>> [memcached] Starting fresh memcached..."
sudo memcached -u root -l 0.0.0.0 -p "$MEMC_PORT" -c 10000 -d
sleep 2
printf "flush_all\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null || true
printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
sleep 1

# ===================== BUILD =====================
echo ">>> [build] Building CHIME..."
mkdir -p "$CHIME_BUILD_DIR"
cd "$CHIME_BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc) chime_bench

# ===================== RUN =====================
echo ""
echo "============================================================"
echo "TEST: CHIME 100M Keys - Uniform Distribution"
echo "  Nodes: $NODE_COUNT, Threads: $THREAD_COUNT"
echo "  Read ratio: $READ_RATIO%, Zipfian: $ZIPF_THETA"
echo "  Bulk load: ${BULK_LOAD_M}M keys, Ops: ${OPS_M}M"
echo "============================================================"
echo ""

STDOUT_LOG="$RESULTS_DIR/chime_100M_uniform_stdout.log"

echo ">>> [exec] Launching CHIME chime_bench..."
echo ">>> Start Node 1 script now!"

# chime_bench arguments:
# 1:node_count 2:thread_count 3:read_ratio 4:zipfian_theta 5:bulk_M 6:ops_M

sudo "$CHIME_BUILD_DIR/chime_bench" \
    $NODE_COUNT $THREAD_COUNT $READ_RATIO $ZIPF_THETA $BULK_LOAD_M $OPS_M \
    2>&1 | tee "$STDOUT_LOG"

# Copy latency file if generated
if [[ -f "chime_latency.dat" ]]; then
    mv chime_latency.dat "$RESULTS_DIR/chime_100M_uniform_latency.dat"
    echo ">>> Saved latency to: $RESULTS_DIR/chime_100M_uniform_latency.dat"
fi

echo ""
echo "============================================================"
echo "CHIME 100M TEST COMPLETE"
echo "Results in: $RESULTS_DIR"
echo "============================================================"
