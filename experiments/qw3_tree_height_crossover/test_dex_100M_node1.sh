#!/bin/bash
###############################################################################
# TEST: DEX 100M Keys - Node 1 (Secondary)
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEX_DIR="$(cd "$SCRIPT_DIR/../../dex" && pwd)"
DEX_BUILD_DIR="$DEX_DIR/build"
MEMC_IP=$(head -1 "$DEX_DIR/memcached.conf")
MEMC_PORT=$(sed -n '2p' "$DEX_DIR/memcached.conf")

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
RUN_M=10
CHECK=0
TIME_BASED=0
EARLY_STOP=0
INDEX=0
RPC_RATE=0.0
ADMIT_RATE=1.0
AUTO_TUNE=0
MAX_THREAD=30

KEY_M=100
UNIFORM=1
ZIPF=0.0

# ===================== CLEANUP =====================
echo ">>> [cleanup] Killing stale processes..."
sudo pkill -9 newbench_latency 2>/dev/null || true
sleep 1
sudo rm -f /dev/shm/dsm_* /dev/shm/rdma_* 2>/dev/null || true

# Hugepages
echo ">>> [hugepages] Setting 36864 pages..."
echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
ulimit -l unlimited 2>/dev/null || true

# ===================== BUILD =====================
echo ">>> [build] Building DEX..."
cd "$DEX_BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc) newbench_latency

# ===================== WAIT FOR NODE 0 =====================
echo ">>> Waiting for Node 0 to start memcached..."
while true; do
    ready=$(printf "get test_ready\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | grep -A1 "^VALUE" | tail -1 | tr -d '\r\n')
    if [ "$ready" = "1" ]; then
        echo ">>> Node 0 ready!"
        break
    fi
    sleep 2
done

# Wait for serverNum >= 1 (Node 0 registered)
echo ">>> Waiting for Node 0 to register..."
while true; do
    sn=$(printf "get serverNum\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | grep -A1 "^VALUE" | tail -1 | tr -d '\r')
    if [ -n "$sn" ] && [ "$sn" -ge 1 ] 2>/dev/null; then
        echo ">>> Node 0 registered (serverNum=$sn)"
        break
    fi
    sleep 1
done
sleep 1

# ===================== RUN =====================
echo ""
echo "============================================================"
echo "TEST: DEX 100M Keys - Node 1"
echo "============================================================"
echo ""

echo ">>> [exec] Launching DEX newbench_latency..."

sudo "$DEX_BUILD_DIR/newbench_latency" \
    $NODE_COUNT $READ_RATIO $INSERT_RATIO $UPDATE_RATIO $DELETE_RATIO $RANGE_RATIO \
    $TOTAL_THREADS $MEM_THREADS $CACHE_MB \
    $UNIFORM $ZIPF \
    $KEY_M $WARMUP_M $RUN_M \
    $CHECK $TIME_BASED $EARLY_STOP \
    $INDEX $RPC_RATE $ADMIT_RATE $AUTO_TUNE $MAX_THREAD

echo ""
echo "============================================================"
echo "DEX 100M Node 1 COMPLETE"
echo "============================================================"
