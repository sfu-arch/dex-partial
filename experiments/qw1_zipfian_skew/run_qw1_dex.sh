#!/bin/bash
#
# QW1 Experiment: DEX Key Access Distribution
# Run DEX benchmark under varying Zipfian skew parameters
#

set -e

# Configuration
NODENUM=2
THREADS=18
MEM_THREADS=4
CACHE_MB=256
BULK_LOAD=50      # 50M keys bulk loaded
WARMUP=10         # 10M warmup ops  
RUN_NUM=50        # 50M benchmark ops

# Fixed parameters
CORRECT=0
TIMEBASE=1
EARLY=1
INDEX=0           # 0=DEX
RPC=1
ADMIT=0.1
TUNE=0
MAX_THREAD=36

# Workload: YCSB-A equivalent (50% read, 50% update)
READ=50
INSERT=0
UPDATE=50
DELETE=0
RANGE=0

# Output directory
OUTPUT_DIR="./results_qw1_dex"
mkdir -p $OUTPUT_DIR

echo "=== QW1 DEX Experiment: Zipfian Skew Variation ==="
echo "Output directory: $OUTPUT_DIR"

# Run uniform distribution
echo ""
echo ">>> Running UNIFORM distribution..."
./restartMemc.sh
./newbench $NODENUM $READ $INSERT $UPDATE $DELETE $RANGE \
    $THREADS $MEM_THREADS $CACHE_MB \
    1 0.0 \
    $BULK_LOAD $WARMUP $RUN_NUM \
    $CORRECT $TIMEBASE $EARLY $INDEX $RPC $ADMIT $TUNE $MAX_THREAD \
    2>&1 | tee "$OUTPUT_DIR/uniform.log"
sleep 5

# Run Zipfian with varying skew
for THETA in 0.6 0.8 0.9 0.99; do
    echo ""
    echo ">>> Running Zipfian with theta=$THETA..."
    ./restartMemc.sh
    ./newbench $NODENUM $READ $INSERT $UPDATE $DELETE $RANGE \
        $THREADS $MEM_THREADS $CACHE_MB \
        0 $THETA \
        $BULK_LOAD $WARMUP $RUN_NUM \
        $CORRECT $TIMEBASE $EARLY $INDEX $RPC $ADMIT $TUNE $MAX_THREAD \
        2>&1 | tee "$OUTPUT_DIR/zipf_${THETA}.log"
    sleep 5
done

echo ""
echo "=== QW1 DEX Experiment Complete ==="
echo "Results saved in: $OUTPUT_DIR"
echo ""
echo "To extract metrics, grep for:"
echo "  - 'Avg. rdma read / op'"
echo "  - 'Avg. rdma write / op'"  
echo "  - 'Avg. rdma read size/ op'"
echo "  - 'Avg. rdma RW size / op'"
echo "  - 'Final throughput'"
