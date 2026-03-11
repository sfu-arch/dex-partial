#!/usr/bin/env bash
# run_memory_node.sh — Run on 10.30.1.9 (monitor + memory node)
# Sequentially runs all skew configs, waits for each to complete
set -e

cd "$(dirname "$0")/.."

MONITOR_IP="10.30.1.9"
RESULTS_DIR="benchmark_run/results"
mkdir -p "$RESULTS_DIR"

# Match DEX config: 30 threads, 10M ops
LOAD_THREADS=30
RUN_THREADS=30
CORO_NUM=1
MEM_MB=65536       # 64GB remote memory for 100M records
TH_MB=10
BUCKET=256
RUN_MAX_REQ=10000000  # 10M ops matching DEX op_num

CONFIGS=("uniform" "zipf03" "zipf05" "zipf08" "zipf099")
LABELS=("Uniform" "Zipfian-0.3" "Zipfian-0.5" "Zipfian-0.8" "Zipfian-0.99")

echo "============================================"
echo "  DART Benchmark — Memory Node (10.30.1.9)"
echo "  30 threads, 100M load, 10M run ops"
echo "============================================"

for i in "${!CONFIGS[@]}"; do
    CONFIG="${CONFIGS[$i]}"
    LABEL="${LABELS[$i]}"
    LOAD_FILE="100m_load"
    RUN_FILE="${CONFIG}_run"
    RESULT_FILE="$RESULTS_DIR/dart_${CONFIG}.txt"

    echo ""
    echo "========================================"
    echo "  Config $((i+1))/5: $LABEL"
    echo "  Load: $LOAD_FILE | Run: $RUN_FILE"
    echo "========================================"

    # Clean binary caches to force re-parse (in case workloads changed)
    rm -f "workload/split/${LOAD_FILE}__bin_" "workload/split/${LOAD_FILE}__bin_buffer_"
    rm -f "workload/split/${RUN_FILE}__bin_" "workload/split/${RUN_FILE}__bin_buffer_"

    # Start memory in background
    echo "[*] Starting memory node..."
    bin/memory --monitor_addr=${MONITOR_IP}:9898 --nic_index=0 &
    MEMORY_PID=$!
    sleep 1

    # Start monitor in foreground (blocks until experiment completes)
    echo "[*] Starting monitor... (waiting for compute node to connect)"
    echo ""
    echo ">>> On 10.30.1.6, run:  bin/compute --monitor_addr=${MONITOR_IP}:9898 --nic_index=0"
    echo ""
    bin/monitor \
        --test_func=0 \
        --memory_num=1 \
        --compute_num=1 \
        --load_thread_num=${LOAD_THREADS} \
        --run_thread_num=${RUN_THREADS} \
        --coro_num=${CORO_NUM} \
        --mem_mb=${MEM_MB} \
        --th_mb=${TH_MB} \
        --workload_load=${LOAD_FILE} \
        --workload_run=${RUN_FILE} \
        --bucket=${BUCKET} \
        --run_max_request=${RUN_MAX_REQ} \
        2>&1 | tee "$RESULT_FILE"

    # Wait for memory to exit
    wait $MEMORY_PID 2>/dev/null || true

    echo ""
    echo "[*] Config $LABEL done. Results saved to $RESULT_FILE"
    echo ""

    # Brief pause between runs
    sleep 3
done

echo "============================================"
echo "  All benchmarks complete!"
echo "  Results in $RESULTS_DIR/"
echo "============================================"
ls -lh "$RESULTS_DIR/"
