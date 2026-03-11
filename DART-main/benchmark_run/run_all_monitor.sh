#!/usr/bin/env bash
set -e
cd "$(dirname "$0")/.."
mkdir -p benchmark_run/results

CONFIGS="uniform zipf03 zipf05 zipf08 zipf099"
RUNS="uniform_run zipf03_run zipf05_run zipf08_run zipf099_run"

i=0
for CONFIG in $CONFIGS; do
    RUN_FILE=$(echo $RUNS | cut -d' ' -f$((i+1)))
    i=$((i+1))
    echo ""
    echo "========================================"
    echo "  Config $i/5: $CONFIG"
    echo "========================================"
    rm -f workload/split/*__bin_*
    echo "[*] Starting monitor for $CONFIG..."
    bin/monitor --test_func=0 --memory_num=1 --compute_num=1 --load_thread_num=30 --run_thread_num=30 --coro_num=1 --mem_mb=4096 --th_mb=10 --workload_load=2m_load --workload_run=$RUN_FILE --bucket=256 --run_max_request=2000000 2>&1 | tee benchmark_run/results/dart_monitor_${CONFIG}.txt
    echo "[*] Config $CONFIG done."
    sleep 3
done
echo "All monitor runs complete!"
