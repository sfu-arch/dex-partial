#!/bin/bash
#
# QW1 Master Experiment Script
# Orchestrates DEX and CHIME experiments for key access distribution study
#
# Prerequisites:
# - Both DEX and CHIME built in their respective build/ directories
# - memcached running and configured
# - RDMA environment properly set up
# - Sufficient huge pages allocated
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEX_DIR="$SCRIPT_DIR/../../dex"
CHIME_DIR="$SCRIPT_DIR/../../CHIME"

echo "=============================================="
echo "QW1 Experiment: Key Access Distribution Impact"
echo "=============================================="
echo ""
echo "DEX directory:   $DEX_DIR"
echo "CHIME directory: $CHIME_DIR"
echo ""

# Check builds exist
if [ ! -f "$DEX_DIR/build/newbench" ]; then
    echo "ERROR: DEX not built. Run from $DEX_DIR:"
    echo "  mkdir build && cd build"
    echo "  cmake -DCMAKE_BUILD_TYPE=Release .."
    echo "  make -j"
    exit 1
fi

if [ ! -f "$CHIME_DIR/build/ycsb_test" ]; then
    echo "ERROR: CHIME not built. Run from $CHIME_DIR:"
    echo "  mkdir build && cd build"
    echo "  cmake -DENABLE_CACHE=on .."
    echo "  make -j"
    exit 1
fi

# Create results directory
RESULTS_DIR="$SCRIPT_DIR/results_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$RESULTS_DIR/dex"
mkdir -p "$RESULTS_DIR/chime"

echo "Results will be saved to: $RESULTS_DIR"
echo ""

# ==================== DEX Experiments ====================
echo "=============================================="
echo "Running DEX Experiments"
echo "=============================================="

cd "$DEX_DIR/build"

# Configuration (adjust for your cluster)
NODENUM=2
THREADS=18
MEM_THREADS=4
CACHE_MB=256
BULK_LOAD=50
WARMUP=10
RUN_NUM=50
INDEX=0  # DEX

# YCSB-A equivalent
READ=50
INSERT=0
UPDATE=50
DELETE=0
RANGE=0

# Run uniform distribution
echo ""
echo ">>> DEX: Running UNIFORM distribution..."
./restartMemc.sh || true
sleep 2
./newbench $NODENUM $READ $INSERT $UPDATE $DELETE $RANGE \
    $THREADS $MEM_THREADS $CACHE_MB \
    1 0.0 \
    $BULK_LOAD $WARMUP $RUN_NUM \
    0 1 1 $INDEX 1 0.1 0 36 \
    2>&1 | tee "$RESULTS_DIR/dex/uniform.log"

# Run Zipfian with varying theta
for THETA in 0.6 0.8 0.9 0.99; do
    echo ""
    echo ">>> DEX: Running Zipfian theta=$THETA..."
    ./restartMemc.sh || true
    sleep 2
    ./newbench $NODENUM $READ $INSERT $UPDATE $DELETE $RANGE \
        $THREADS $MEM_THREADS $CACHE_MB \
        0 $THETA \
        $BULK_LOAD $WARMUP $RUN_NUM \
        0 1 1 $INDEX 1 0.1 0 36 \
        2>&1 | tee "$RESULTS_DIR/dex/zipf_${THETA}.log"
done

# ==================== CHIME Experiments ====================
echo ""
echo "=============================================="
echo "Running CHIME Experiments"
echo "=============================================="
echo ""
echo "NOTE: CHIME uses YCSB-generated workloads."
echo "Standard YCSB Zipfian uses theta ≈ 0.99 by default."
echo ""

cd "$CHIME_DIR/build"

CN_NUM=2
CLIENT_NUM=18
CORO_NUM=2
KEY_TYPE="randint"

# Generate workload if needed
if [ ! -f "../ycsb/workloads/load_${KEY_TYPE}_workloada" ]; then
    echo ">>> Generating CHIME workloads..."
    cd ../ycsb
    python3 gen_workload.py workloada ${KEY_TYPE} full
    cd ../build
fi

# Run CHIME with standard workload A (Zipfian ≈ 0.99)
echo ">>> CHIME: Running workload A (Zipfian theta ≈ 0.99)..."
./restartMemc.sh || true
sleep 2

python3 ../ycsb/split_workload.py a ${KEY_TYPE} ${CN_NUM} ${CLIENT_NUM}

./ycsb_test ${CN_NUM} ${CLIENT_NUM} ${CORO_NUM} ${KEY_TYPE} a \
    2>&1 | tee "$RESULTS_DIR/chime/workload_a.log"

# Copy latency files
cp ../us_lat/epoch_*.lat "$RESULTS_DIR/chime/" 2>/dev/null || true

# Collect cluster latency
echo ">>> Collecting CHIME latency statistics..."
python3 ../us_lat/cluster_latency.py ${CN_NUM} 1 10 \
    2>&1 | tee "$RESULTS_DIR/chime/latency_summary.log"

# ==================== Parse Results ====================
echo ""
echo "=============================================="
echo "Parsing Results"
echo "=============================================="

cd "$SCRIPT_DIR"
python3 parse_results.py \
    --dex-dir "$RESULTS_DIR/dex" \
    --chime-dir "$RESULTS_DIR/chime" \
    --output-dir "$RESULTS_DIR/plots"

# Save JSON results
python3 parse_results.py \
    --dex-dir "$RESULTS_DIR/dex" \
    --chime-dir "$RESULTS_DIR/chime" \
    --json > "$RESULTS_DIR/results.json"

echo ""
echo "=============================================="
echo "QW1 Experiment Complete!"
echo "=============================================="
echo ""
echo "Results location: $RESULTS_DIR"
echo "  - DEX logs:    $RESULTS_DIR/dex/"
echo "  - CHIME logs:  $RESULTS_DIR/chime/"
echo "  - Plots:       $RESULTS_DIR/plots/"
echo "  - JSON data:   $RESULTS_DIR/results.json"
echo ""
