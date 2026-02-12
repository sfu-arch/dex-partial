#!/bin/bash
###############################################################################
# APEX Zipfian Skew Sweep — Node 0 (Memory Node + Memcached Host)
#
# This script runs on the MEMORY NODE (10.30.1.9).
# It provides far-memory storage and memcached coordination.
# It does NOT run workloads — the compute node does that.
#
# Sweeps: Uniform, Zipfian θ = 0.6, 0.8, 0.9, 0.99
# Workload: 100% Lookup (identical to DEX/CHIME experiments)
#
# USAGE:
#   1. Run this FIRST on Node 0 (memory node):
#      ./apex_node0.sh
#   2. Then start the compute node script on Node 1:
#      ./apex_node1.sh
#
# PREREQUISITES:
#   - RDMA NIC configured (ibdev2netdev shows active port)
#   - Huge pages available (script sets them up)
#   - Dependencies installed (see apex/script/installLibs.sh)
#   - memcached.conf has correct IP (10.30.1.9) and port (11211)
###############################################################################
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APEX_DIR="$SCRIPT_DIR/../../apex"
MEMC_IP=$(head -1 "$APEX_DIR/memcached.conf")
MEMC_PORT=$(sed -n '2p' "$APEX_DIR/memcached.conf")

# ===================== SHARED CONFIGURATION (must match node1!) =====================
NODE_COUNT=2
THREAD_COUNT=30         # Worker threads on compute node
READ_RATIO=100          # 100% point reads
RANGE_RATIO=0           # 0% range scans
TOTAL_OPS=30000000      # 30M ops (matches DEX RUN_M=30)
RANGE_SIZE=100          # Range scan size (unused when RANGE_RATIO=0)

SKEW_CONFIGS=(
    "1 0.0   uniform"
    "0 0.6   zipf_0.6"
    "0 0.8   zipf_0.8"
    "0 0.9   zipf_0.9"
    "0 0.99  zipf_0.99"
)

# Iteration counter for cross-node synchronization
ITERATION=0

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [APEX-N0] $1"
}

# ===================== Flush & reset memcached =====================
flush_and_reset_memcached() {
    log "Killing any existing memcached..."
    sudo pkill -9 memcached 2>/dev/null || true
    sleep 2

    log "Starting fresh memcached on 0.0.0.0:${MEMC_PORT}..."
    sudo memcached -u root -l 0.0.0.0 -p "$MEMC_PORT" -c 10000 -d
    sleep 2

    # flush_all clears any stale data
    log "Flushing all keys..."
    printf "flush_all\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null || true
    sleep 1

    # Seed coordination keys (DSMKeeper expects these at 0)
    log "Setting serverNum=0, clientNum=0..."
    printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
    printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
    sleep 1

    # Verify memcached is live
    local reply
    reply=$(printf "get serverNum\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null | head -1)
    if [[ "$reply" == *"VALUE"* ]]; then
        log "memcached verified OK"
    else
        log "WARNING: memcached may not be responding. Retrying..."
        sudo pkill -9 memcached 2>/dev/null || true
        sleep 2
        sudo memcached -u root -l 0.0.0.0 -p "$MEMC_PORT" -c 10000 -d
        sleep 3
        printf "flush_all\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" 2>/dev/null || true
        printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
        printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
        sleep 1
    fi

    # Set iteration sentinel (node1 polls this to know node0 is ready)
    ITERATION=$((ITERATION + 1))
    local iter_len=${#ITERATION}
    printf "set apex_iter 0 0 %d\r\n%s\r\nquit\r\n" "$iter_len" "$ITERATION" | nc -w 2 "$MEMC_IP" "$MEMC_PORT" || true
    log "Set apex_iter=$ITERATION (node1 sync sentinel)"
}

# ===================== Cleanup =====================
cleanup_previous_run() {
    log "Killing stale APEX processes..."
    sudo pkill -9 latency_bench 2>/dev/null || true
    sudo pkill -9 ycsb_bench 2>/dev/null || true
    sleep 1

    log "Clearing /dev/shm RDMA artifacts..."
    sudo rm -f /dev/shm/dsm_* /dev/shm/rdma_* 2>/dev/null || true
}

# ===================== Hugepages =====================
setup_hugepages() {
    log "Setting up hugepages (36864 × 2MB = 72 GB)..."
    echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
    ulimit -l unlimited 2>/dev/null || true
    local hp_free=$(grep HugePages_Free /proc/meminfo | awk '{print $2}')
    log "Hugepages available: $hp_free"
}

# ===================== BUILD =====================
log "Building APEX..."
cd "$APEX_DIR"
rm -rf build && mkdir build && cd build
cmake .. > /dev/null 2>&1
make -j$(nproc) latency_bench
log "Build complete."

# ===================== SWEEP =====================
echo ""
echo "============================================================"
echo "  APEX Skew Sweep — Node 0 (Memory Server)"
echo "  Configs: uniform, zipf_0.6, zipf_0.8, zipf_0.9, zipf_0.99"
echo "  NOTE: This node provides memory only."
echo "        Latency results are saved on Node 1."
echo "============================================================"

for config in "${SKEW_CONFIGS[@]}"; do
    read -r UNIFORM ZIPF_THETA LABEL <<< "$config"
    
    echo ""
    echo "============================================================"
    echo "  APEX Memory — $LABEL (uniform=$UNIFORM, theta=$ZIPF_THETA)"
    echo "============================================================"
    
    cleanup_previous_run
    setup_hugepages
    flush_and_reset_memcached
    
    cd "$APEX_DIR/build"
    log "Starting APEX memory server for $LABEL ... waiting for Node 1."
    echo ""
    
    sudo ./latency_bench \
        ${NODE_COUNT} ${THREAD_COUNT} ${READ_RATIO} ${RANGE_RATIO} \
        ${TOTAL_OPS} ${RANGE_SIZE} ${ZIPF_THETA} ${UNIFORM} \
        2>&1 | tee "/tmp/apex_node0_${LABEL}_stdout.log"
    
    log "$LABEL memory server done. Sleeping 5s before next..."
    sleep 5
done

echo ""
echo "============================================================"
echo "  APEX SWEEP COMPLETE — Node 0 (Memory Server)"
echo "  Latency results are on Node 1."
echo "============================================================"
