#!/usr/bin/env bash
# run_compute_auto.sh — Run on 10.30.1.6 (compute node) — AUTOMATIC
# Sequentially runs compute for all skew configs
# Retries connecting until monitor is ready (no manual intervention needed)
set -e

cd "$(dirname "$0")/.."

MONITOR_IP="10.30.1.9"
RESULTS_DIR="benchmark_run/results"
mkdir -p "$RESULTS_DIR"

CONFIGS=("uniform" "zipf03" "zipf05" "zipf08" "zipf099")
LABELS=("Uniform" "Zipfian-0.3" "Zipfian-0.5" "Zipfian-0.8" "Zipfian-0.99")

echo "============================================"
echo "  DART Benchmark — Compute Node AUTO"
echo "  30 threads, 100M load, 10M run ops"
echo "============================================"

for i in "${!CONFIGS[@]}"; do
    CONFIG="${CONFIGS[$i]}"
    LABEL="${LABELS[$i]}"
    LOAD_FILE="100m_load"
    RUN_FILE="${CONFIG}_run"
    RESULT_FILE="$RESULTS_DIR/dart_compute_${CONFIG}.txt"

    echo ""
    echo "========================================"
    echo "  Config $((i+1))/5: $LABEL"
    echo "========================================"

    # Clean binary caches
    rm -f "workload/split/${LOAD_FILE}__bin_" "workload/split/${LOAD_FILE}__bin_buffer_"
    rm -f "workload/split/${RUN_FILE}__bin_" "workload/split/${RUN_FILE}__bin_buffer_"

    # Retry loop: wait until monitor is accepting connections on port 9898
    echo "[*] Waiting for monitor on ${MONITOR_IP}:9898..."
    while ! bash -c "echo > /dev/tcp/${MONITOR_IP}/9898" 2>/dev/null; do
        sleep 2
    done
    sleep 2  # small grace period after port becomes available

    echo "[*] Starting compute node..."
    bin/compute --monitor_addr=${MONITOR_IP}:9898 --nic_index=0 2>&1 | tee "$RESULT_FILE"

    echo ""
    echo "[*] Config $LABEL done. Results saved to $RESULT_FILE"
    echo ""

    sleep 3
done

echo "============================================"
echo "  All benchmarks complete!"
echo "  Results in $RESULTS_DIR/"
echo "============================================"
ls -lh "$RESULTS_DIR/"
