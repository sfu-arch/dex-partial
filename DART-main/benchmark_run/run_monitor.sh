#!/usr/bin/env bash
# run_monitor.sh — Run on 10.30.1.9 in TERMINAL 1
# Starts the monitor for each config sequentially (foreground, blocks until done)
# Order: start this FIRST, then run_memory.sh in terminal 2, then compute on 10.30.1.6
set -e

cd "$(dirname "$0")/.."

MONITOR_IP="10.30.1.9"
RESULTS_DIR="benchmark_run/results"
mkdir -p "$RESULTS_DIR"

LOAD_THREADS=30
RUN_THREADS=30
CORO_NUM=1
MEM_MB=4096
TH_MB=10
BUCKET=256
RUN_MAX_REQ=2000000

CONFIGS=("uniform" "zipf03" "zipf05" "zipf08" "zipf099")
LABELS=("Uniform" "Zipfian-0.3" "Zipfian-0.5" "Zipfian-0.8" "Zipfian-0.99")

echo "============================================"
echo "  DART Monitor — 10.30.1.9 (Terminal 1)"
echo "  30 threads, 2M load, 2M run ops"
echo "============================================"

for i in "${!CONFIGS[@]}"; do
    CONFIG="${CONFIGS[$i]}"
    LABEL="${LABELS[$i]}"
    LOAD_FILE="2m_load"
    RUN_FILE="${CONFIG}_run"
    RESULT_FILE="$RESULTS_DIR/dart_monitor_${CONFIG}.txt"

    echo ""
    echo "========================================"
    echo "  Config $((i+1))/5: $LABEL"
    echo "  Load: $LOAD_FILE | Run: $RUN_FILE"
    echo "========================================"

    # Clean binary caches
    rm -f "workload/split/${LOAD_FILE}__bin_" "workload/split/${LOAD_FILE}__bin_buffer_"
    rm -f "workload/split/${RUN_FILE}__bin_" "workload/split/${RUN_FILE}__bin_buffer_"

    echo "[*] Starting monitor (foreground)..."
    echo ">>> Now start memory in terminal 2, then compute on 10.30.1.6"
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

    echo ""
    echo "[*] Config $LABEL done."
    echo ""
    sleep 3
done

echo "============================================"
echo "  All monitor runs complete!"
echo "============================================"
