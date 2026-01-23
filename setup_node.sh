#!/bin/bash
#
# Setup script for DEX and CHIME experiments
# Run this on EACH node in your cluster
#
# Usage: bash setup_node.sh <node0_ip>
#
# Example: bash setup_node.sh 10.0.2.1
#

set -e

if [ -z "$1" ]; then
    echo "Usage: bash setup_node.sh <node0_ip>"
    echo ""
    echo "  <node0_ip> = IP address of the primary compute node (Node 0)"
    echo ""
    echo "Example: bash setup_node.sh 10.0.2.1"
    exit 1
fi

NODE0_IP=$1

echo "=============================================="
echo "  Setting up DEX/CHIME Benchmark Environment"
echo "=============================================="
echo ""
echo "Node 0 IP: $NODE0_IP"
echo ""

# Get script directory
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$SCRIPT_DIR"

# Step 1: Install dependencies
echo ">>> [1/6] Installing dependencies..."
sudo apt update
sudo apt install -y \
    rdma-core \
    libibverbs-dev \
    librdmacm-dev \
    ibverbs-utils \
    perftest \
    memcached \
    libmemcached-dev \
    cmake \
    g++ \
    libnuma-dev \
    libtbb-dev \
    libboost-all-dev \
    netcat \
    curl \
    python3

# Install cityhash (may need to build from source)
if ! dpkg -l | grep -q libcityhash; then
    echo ">>> Installing cityhash..."
    sudo apt install -y libcityhash-dev 2>/dev/null || {
        echo ">>> Building cityhash from source..."
        cd /tmp
        git clone https://github.com/google/cityhash.git
        cd cityhash
        ./configure
        make -j
        sudo make install
        sudo ldconfig
        cd "$ROOT_DIR"
    }
fi

# Step 2: Setup hugepages
echo ""
echo ">>> [2/6] Setting up hugepages..."
echo 36864 | sudo tee /proc/sys/vm/nr_hugepages > /dev/null
ulimit -l unlimited 2>/dev/null || true

# Make hugepages persistent across reboots
if ! grep -q "vm.nr_hugepages" /etc/sysctl.conf; then
    echo "vm.nr_hugepages=36864" | sudo tee -a /etc/sysctl.conf
fi

# Step 3: Configure memcached IP
echo ""
echo ">>> [3/6] Configuring memcached IP ($NODE0_IP)..."

# DEX config
cat > "$ROOT_DIR/dex/memcached.conf" << EOF
$NODE0_IP
11211
EOF

# CHIME config
cat > "$ROOT_DIR/CHIME/memcached.conf" << EOF
$NODE0_IP
11211
EOF

# Step 4: Build DEX
echo ""
echo ">>> [4/6] Building DEX..."
cd "$ROOT_DIR/dex"
./script/hugepage.sh 2>/dev/null || true
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
cp ../script/*.sh . 2>/dev/null || true

# Step 5: Build CHIME
echo ""
echo ">>> [5/6] Building CHIME..."
cd "$ROOT_DIR/CHIME"
mkdir -p build
cd build
cmake ..
make -j$(nproc)
cp ../script/*.sh . 2>/dev/null || true

# Step 6: Verify RDMA
echo ""
echo ">>> [6/6] Verifying RDMA setup..."
echo ""
echo "RDMA Devices:"
ibv_devinfo 2>/dev/null || echo "WARNING: No RDMA devices found or ibv_devinfo failed"
echo ""

# Done
echo ""
echo "=============================================="
echo "  Setup Complete!"
echo "=============================================="
echo ""
echo "Memcached configured to use: $NODE0_IP:11211"
echo ""
echo "NEXT STEPS:"
echo ""
echo "  1. Run this setup script on ALL nodes in your cluster"
echo ""
echo "  2. To run DEX benchmark:"
echo "     - On Node 0: cd $ROOT_DIR && bash experiments/qw1_zipfian_skew/dex_node0.sh"
echo "     - On Node 1+: cd $ROOT_DIR && bash experiments/qw1_zipfian_skew/dex_node1.sh"
echo ""
echo "  3. To run CHIME benchmark:"
echo "     - On Node 0: cd $ROOT_DIR && bash experiments/qw1_zipfian_skew/chime_node0.sh"
echo "     - On Node 1+: cd $ROOT_DIR && bash experiments/qw1_zipfian_skew/chime_node1.sh"
echo ""
