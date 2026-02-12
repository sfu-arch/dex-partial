#!/bin/bash
###############################################################################
# APEX Remote Deployment Script
#
# Deploys the full APEX project to both remote servers, installs dependencies,
# and prepares the environment for running experiments.
#
# USAGE:
#   ./deploy.sh                  # Deploy to both nodes
#   ./deploy.sh setup            # Deploy + install dependencies
#   ./deploy.sh sync             # Just rsync code (no dependency install)
#   ./deploy.sh check            # Check RDMA connectivity between nodes
#
# CONFIGURATION:
#   Edit the variables below to match your cluster.
###############################################################################
set -e

# ─── Cluster Configuration ─────────────────────────────────────────
# Memory node (runs memcached + far-memory server)
MEM_NODE_IP="10.30.1.9"
MEM_NODE_USER="apa222"         # SSH username
MEM_NODE_SSH_PORT=22           # SSH port (22 default, 404 for some clusters)

# Compute node (runs workloads, collects results)
COMP_NODE_IP="10.30.1.6"
COMP_NODE_USER="apa222"        # SSH username
COMP_NODE_SSH_PORT=22          # SSH port

# Remote path where the project will be deployed
REMOTE_BASE="/home/${MEM_NODE_USER}/dex-partial"

# Local project root
LOCAL_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

log() {
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] [DEPLOY] $1"
}

ssh_mem() {
    ssh -p "$MEM_NODE_SSH_PORT" -o StrictHostKeyChecking=no "${MEM_NODE_USER}@${MEM_NODE_IP}" "$@"
}

ssh_comp() {
    ssh -p "$COMP_NODE_SSH_PORT" -o StrictHostKeyChecking=no "${COMP_NODE_USER}@${COMP_NODE_IP}" "$@"
}

# ─── Sync Code ──────────────────────────────────────────────────────
sync_code() {
    log "Syncing APEX code to memory node ($MEM_NODE_IP)..."
    rsync -avz --delete \
        -e "ssh -p $MEM_NODE_SSH_PORT -o StrictHostKeyChecking=no" \
        --exclude 'build/' --exclude '.git/' --exclude '__pycache__/' \
        --exclude 'results/' --exclude '*.o' --exclude '*.a' \
        "$LOCAL_ROOT/apex/" \
        "${MEM_NODE_USER}@${MEM_NODE_IP}:${REMOTE_BASE}/apex/"

    log "Syncing APEX code to compute node ($COMP_NODE_IP)..."
    rsync -avz --delete \
        -e "ssh -p $COMP_NODE_SSH_PORT -o StrictHostKeyChecking=no" \
        --exclude 'build/' --exclude '.git/' --exclude '__pycache__/' \
        --exclude 'results/' --exclude '*.o' --exclude '*.a' \
        "$LOCAL_ROOT/apex/" \
        "${COMP_NODE_USER}@${COMP_NODE_IP}:${REMOTE_BASE}/apex/"

    log "Syncing experiment scripts to both nodes..."
    rsync -avz \
        -e "ssh -p $MEM_NODE_SSH_PORT -o StrictHostKeyChecking=no" \
        --exclude 'results/' \
        "$LOCAL_ROOT/experiments/" \
        "${MEM_NODE_USER}@${MEM_NODE_IP}:${REMOTE_BASE}/experiments/"

    rsync -avz \
        -e "ssh -p $COMP_NODE_SSH_PORT -o StrictHostKeyChecking=no" \
        --exclude 'results/' \
        "$LOCAL_ROOT/experiments/" \
        "${COMP_NODE_USER}@${COMP_NODE_IP}:${REMOTE_BASE}/experiments/"

    log "Code sync complete."
}

# ─── Install Dependencies ──────────────────────────────────────────
install_deps() {
    local node_ip="$1"
    local node_user="$2"
    local node_port="$3"
    local node_name="$4"

    log "Installing dependencies on $node_name ($node_ip)..."

    ssh -p "$node_port" -o StrictHostKeyChecking=no "${node_user}@${node_ip}" << 'REMOTE_INSTALL'
set -e
echo ">>> Installing system packages..."
sudo apt-get update -qq
sudo apt-get install -y -qq \
    cmake build-essential \
    memcached libmemcached-dev \
    libnuma-dev \
    libboost-all-dev \
    libibverbs-dev librdmacm-dev \
    libtbb-dev \
    netcat-openbsd \
    2>/dev/null

# cityhash (if not already installed)
if ! ldconfig -p | grep -q libcityhash; then
    echo ">>> Installing cityhash..."
    cd /tmp
    git clone https://github.com/google/cityhash.git 2>/dev/null || true
    cd cityhash
    ./configure && make all CXXFLAGS="-g -O3" && sudo make install
    sudo ldconfig
fi

echo ">>> Dependencies installed."
REMOTE_INSTALL

    log "Dependencies installed on $node_name."
}

# ─── Setup Hugepages ───────────────────────────────────────────────
setup_hugepages_remote() {
    local node_ip="$1"
    local node_user="$2"
    local node_port="$3"
    local node_name="$4"

    log "Setting up hugepages on $node_name ($node_ip)..."
    ssh -p "$node_port" -o StrictHostKeyChecking=no "${node_user}@${node_ip}" << 'REMOTE_HP'
echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
ulimit -l unlimited 2>/dev/null || true
echo "Hugepages: $(grep HugePages_Free /proc/meminfo)"
REMOTE_HP
}

# ─── Build on Remote ───────────────────────────────────────────────
build_remote() {
    local node_ip="$1"
    local node_user="$2"
    local node_port="$3"
    local node_name="$4"

    log "Building APEX on $node_name ($node_ip)..."
    ssh -p "$node_port" -o StrictHostKeyChecking=no "${node_user}@${node_ip}" << REMOTE_BUILD
set -e
cd ${REMOTE_BASE}/apex
rm -rf build && mkdir build && cd build
cmake .. 2>&1 | tail -5
make -j\$(nproc) 2>&1 | tail -3
echo "Build complete. Binaries:"
ls -la latency_bench ycsb_bench 2>/dev/null
REMOTE_BUILD

    log "Build complete on $node_name."
}

# ─── Check RDMA ────────────────────────────────────────────────────
check_rdma() {
    log "Checking RDMA on memory node ($MEM_NODE_IP)..."
    ssh_mem << 'CHECK'
echo "=== ibdev2netdev ==="
ibdev2netdev 2>/dev/null || echo "ibdev2netdev not found"
echo ""
echo "=== ibv_devinfo (summary) ==="
ibv_devinfo 2>/dev/null | head -20 || echo "ibv_devinfo not found"
echo ""
echo "=== show_gids ==="
show_gids 2>/dev/null | head -10 || echo "show_gids not found"
CHECK

    log "Checking RDMA on compute node ($COMP_NODE_IP)..."
    ssh_comp << 'CHECK'
echo "=== ibdev2netdev ==="
ibdev2netdev 2>/dev/null || echo "ibdev2netdev not found"
echo ""
echo "=== ibv_devinfo (summary) ==="
ibv_devinfo 2>/dev/null | head -20 || echo "ibv_devinfo not found"
echo ""
echo "=== show_gids ==="
show_gids 2>/dev/null | head -10 || echo "show_gids not found"
CHECK
}

# ─── Update memcached.conf ─────────────────────────────────────────
update_memc_conf() {
    log "Updating memcached.conf on both nodes..."
    ssh_mem "echo -e '${MEM_NODE_IP}\n11211' > ${REMOTE_BASE}/apex/memcached.conf"
    ssh_comp "echo -e '${MEM_NODE_IP}\n11211' > ${REMOTE_BASE}/apex/memcached.conf"
    log "memcached.conf updated: ${MEM_NODE_IP}:11211"
}

# ─── Fetch Results ──────────────────────────────────────────────────
fetch_results() {
    local local_results="$LOCAL_ROOT/experiments/apex_skew/results/apex"
    mkdir -p "$local_results"

    log "Fetching results from compute node ($COMP_NODE_IP)..."
    rsync -avz \
        -e "ssh -p $COMP_NODE_SSH_PORT -o StrictHostKeyChecking=no" \
        "${COMP_NODE_USER}@${COMP_NODE_IP}:${REMOTE_BASE}/experiments/apex_skew/results/" \
        "$LOCAL_ROOT/experiments/apex_skew/results/" \
        2>/dev/null || log "No results to fetch yet."

    log "Results fetched to: $LOCAL_ROOT/experiments/apex_skew/results/"
    ls -la "$local_results/" 2>/dev/null || true
}

# ─── Main ───────────────────────────────────────────────────────────
case "${1:-deploy}" in
    deploy|sync)
        sync_code
        update_memc_conf
        ;;
    setup)
        sync_code
        install_deps "$MEM_NODE_IP" "$MEM_NODE_USER" "$MEM_NODE_SSH_PORT" "memory-node"
        install_deps "$COMP_NODE_IP" "$COMP_NODE_USER" "$COMP_NODE_SSH_PORT" "compute-node"
        update_memc_conf
        build_remote "$MEM_NODE_IP" "$MEM_NODE_USER" "$MEM_NODE_SSH_PORT" "memory-node"
        build_remote "$COMP_NODE_IP" "$COMP_NODE_USER" "$COMP_NODE_SSH_PORT" "compute-node"
        ;;
    build)
        build_remote "$MEM_NODE_IP" "$MEM_NODE_USER" "$MEM_NODE_SSH_PORT" "memory-node"
        build_remote "$COMP_NODE_IP" "$COMP_NODE_USER" "$COMP_NODE_SSH_PORT" "compute-node"
        ;;
    check)
        check_rdma
        ;;
    hugepages|hp)
        setup_hugepages_remote "$MEM_NODE_IP" "$MEM_NODE_USER" "$MEM_NODE_SSH_PORT" "memory-node"
        setup_hugepages_remote "$COMP_NODE_IP" "$COMP_NODE_USER" "$COMP_NODE_SSH_PORT" "compute-node"
        ;;
    fetch)
        fetch_results
        ;;
    *)
        echo "APEX Remote Deployment"
        echo ""
        echo "Usage: $0 <command>"
        echo ""
        echo "Commands:"
        echo "  deploy    - Sync code to both nodes (default)"
        echo "  setup     - Full setup: sync + install deps + build"
        echo "  sync      - Just rsync code (alias for deploy)"
        echo "  build     - Build on both nodes"
        echo "  check     - Check RDMA connectivity"
        echo "  hp        - Setup hugepages on both nodes"
        echo "  fetch     - Fetch results from compute node"
        echo ""
        echo "Cluster:"
        echo "  Memory node:  ${MEM_NODE_USER}@${MEM_NODE_IP}:${MEM_NODE_SSH_PORT}"
        echo "  Compute node: ${COMP_NODE_USER}@${COMP_NODE_IP}:${COMP_NODE_SSH_PORT}"
        echo "  Remote path:  ${REMOTE_BASE}"
        exit 1
        ;;
esac
