#!/bin/bash
#
# QW1 Experiment: CHIME Key Access Distribution  
# Run CHIME benchmark with standard Zipfian workload (theta ≈ 0.99)
#
# NOTE: CHIME uses YCSB framework for workload generation.
# The standard YCSB Zipfian uses theta ≈ 0.99 by default.
# Varying theta requires modifying YCSB source or using zipfconstant property.
#

set -e

# Configuration
CN_NUM=2              # Number of compute nodes
CLIENT_NUM=18         # Clients per compute node
CORO_NUM=2            # Coroutines per client
KEY_TYPE="randint"
WORKLOAD="a"          # YCSB-A: 50% read, 50% update

# Output directory  
OUTPUT_DIR="./results_qw1_chime"
mkdir -p $OUTPUT_DIR

echo "=== QW1 CHIME Experiment: Standard Zipfian Workload ==="
echo "Output directory: $OUTPUT_DIR"
echo ""
echo "NOTE: CHIME uses YCSB's built-in Zipfian (theta ≈ 0.99)"
echo "To vary theta, modify YCSB workload spec with 'zipfconstant=<value>'"
echo ""

# Ensure workloads are generated
if [ ! -f "../ycsb/workloads/load_${KEY_TYPE}_workload${WORKLOAD}" ]; then
    echo ">>> Generating YCSB workloads..."
    cd ../ycsb
    python3 gen_workload.py workload${WORKLOAD} ${KEY_TYPE} full
    cd ../build
fi

# Clear memcached and prepare
echo ">>> Restarting memcached..."
./restartMemc.sh

# Split workloads across nodes
echo ">>> Splitting workloads for $CN_NUM nodes, $CLIENT_NUM clients each..."
python3 ../ycsb/split_workload.py ${WORKLOAD} ${KEY_TYPE} ${CN_NUM} ${CLIENT_NUM}

# Run benchmark
echo ">>> Running CHIME benchmark..."
./ycsb_test ${CN_NUM} ${CLIENT_NUM} ${CORO_NUM} ${KEY_TYPE} ${WORKLOAD} \
    2>&1 | tee "$OUTPUT_DIR/chime_workload${WORKLOAD}.log"

# Collect latency data
echo ""
echo ">>> Collecting latency data..."
EPOCH_NUM=10  # Match TEST_EPOCH in ycsb_test.cpp
python3 ../us_lat/cluster_latency.py ${CN_NUM} 1 ${EPOCH_NUM} \
    2>&1 | tee "$OUTPUT_DIR/latency_summary.log"

# Copy latency files
echo ">>> Copying latency histograms..."
cp ../us_lat/epoch_*.lat "$OUTPUT_DIR/" 2>/dev/null || true

echo ""
echo "=== QW1 CHIME Experiment Complete ==="
echo "Results saved in: $OUTPUT_DIR"
echo ""
echo "Key metrics from log:"
echo "  - 'cluster throughput'"
echo "  - 'cache hit rate'"
echo "  - Latency percentiles in latency_summary.log"
