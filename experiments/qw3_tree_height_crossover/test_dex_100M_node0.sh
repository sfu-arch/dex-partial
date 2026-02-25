#!/bin/bash
###############################################################################
# TEST: DEX 100M Keys - Node 0 (Primary)
# Single test run for comparison with CHIME
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEX_DIR="$(cd "$SCRIPT_DIR/../../dex" && pwd)"
DEX_BUILD_DIR="$DEX_DIR/build"
RESULTS_DIR="$SCRIPT_DIR/results/test"
MEMC_IP=$(head -1 "$DEX_DIR/memcached.conf")
MEMC_PORT=$(sed -n '2p' "$DEX_DIR/memcached.conf")
mkdir -p "$RESULTS_DIR"

# ===================== CONFIGURATION =====================
NODE_COUNT=2
READ_RATIO=100
INSERT_RATIO=0
UPDATE_RATIO=0
DELETE_RATIO=0
RANGE_RATIO=0
TOTAL_THREADS=30
MEM_THREADS=4
CACHE_MB=64
WARMUP_M=1
RUN_M=10              # 10M ops for measurement
CHECK=0
TIME_BASED=0
EARLY_STOP=0
INDEX=0               # 0=DEX
RPC_RATE=0.0
ADMIT_RATE=1.0
AUTO_TUNE=0
MAX_THREAD=30

# Test config
KEY_M=100             # 100M keys
UNIFORM=1             # Uniform distribution
ZIPF=0.0

# ===================== CLEANUP & SETUP =====================
echo ">>> [cleanup] Killing stale processes..."
sudo pkill -9 newbench_latency 2>/dev/null || true
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
printf "set test_ready 0 0 1\r\n1\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
sleep 1

# ===================== BUILD =====================
echo ">>> [build] Building DEX..."
cd "$DEX_BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc) newbench_latency

# ===================== RUN =====================
echo ""
echo "============================================================"
echo "TEST: DEX 100M Keys - Uniform - 64MB Cache"
echo "============================================================"
echo ""

STDOUT_LOG="$RESULTS_DIR/dex_100M_uniform_stdout.log"
LATENCY_FILE="$RESULTS_DIR/dex_100M_uniform_latency.dat"

echo ">>> [exec] Launching DEX newbench_latency..."
echo ">>> Waiting for Node 1 to start..."

sudo "$DEX_BUILD_DIR/newbench_latency" \
    $NODE_COUNT $READ_RATIO $INSERT_RATIO $UPDATE_RATIO $DELETE_RATIO $RANGE_RATIO \
    $TOTAL_THREADS $MEM_THREADS $CACHE_MB \
    $UNIFORM $ZIPF \
    $KEY_M $WARMUP_M $RUN_M \
    $CHECK $TIME_BASED $EARLY_STOP \
    $INDEX $RPC_RATE $ADMIT_RATE $AUTO_TUNE $MAX_THREAD \
    2>&1 | tee "$STDOUT_LOG"

# Copy latency file
if [[ -f "dex_read_latency.dat" ]]; then
    mv dex_read_latency.dat "$LATENCY_FILE"
    echo ">>> Saved latency to: $LATENCY_FILE"
fi

echo ""
echo "============================================================"
echo "DEX 100M TEST COMPLETE"
echo "Results in: $RESULTS_DIR"
echo "============================================================"
