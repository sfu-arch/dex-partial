#!/bin/bash
###############################################################################
# TEST: CHIME 100M Keys - Node 1 (Secondary)
# Uses chime_bench (no code changes needed)
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CHIME_DIR="$(cd "$SCRIPT_DIR/../../CHIME" && pwd)"
CHIME_BUILD_DIR="$CHIME_DIR/build"
MEMC_IP=$(head -1 "$CHIME_DIR/memcached.conf")
MEMC_PORT=$(sed -n '2p' "$CHIME_DIR/memcached.conf")

# ===================== CONFIGURATION =====================
NODE_COUNT=2
THREAD_COUNT=30
READ_RATIO=100
ZIPF_THETA=0.0
BULK_LOAD_M=100
OPS_M=10

# ===================== CLEANUP =====================
echo ">>> [cleanup] Killing stale processes..."
sudo pkill -9 chime_bench 2>/dev/null || true
sleep 1
sudo rm -f /dev/shm/dsm_* /dev/shm/rdma_* 2>/dev/null || true

# Hugepages
echo ">>> [hugepages] Setting 36864 pages..."
echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
ulimit -l unlimited 2>/dev/null || true

# ===================== BUILD =====================
echo ">>> [build] Building CHIME..."
mkdir -p "$CHIME_BUILD_DIR"
cd "$CHIME_BUILD_DIR"
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc) chime_bench

# ===================== WAIT FOR NODE 0 =====================
echo ">>> Waiting for Node 0 to start (serverNum >= 1)..."
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
echo "TEST: CHIME 100M Keys - Node 1"
echo "============================================================"
echo ""

echo ">>> [exec] Launching CHIME chime_bench..."

sudo "$CHIME_BUILD_DIR/chime_bench" \
    $NODE_COUNT $THREAD_COUNT $READ_RATIO $ZIPF_THETA $BULK_LOAD_M $OPS_M

echo ""
echo "============================================================"
echo "CHIME 100M Node 1 COMPLETE"
echo "============================================================"
