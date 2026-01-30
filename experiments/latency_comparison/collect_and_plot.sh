#!/bin/bash
# Collect latency data files and generate plots
# Run this script locally after running the benchmarks on remote nodes

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_DIR="${SCRIPT_DIR}/results"

mkdir -p "${OUTPUT_DIR}"

echo "=========================================="
echo "DEX vs CHIME Latency Comparison"
echo "=========================================="

# Check for data files
DEX_FILE=""
CHIME_FILE=""

# Look for DEX data
for f in "${SCRIPT_DIR}/dex_latency.dat" "${SCRIPT_DIR}/../../../dex/build/dex_latency.dat" "./dex_latency.dat"; do
    if [ -f "$f" ]; then
        DEX_FILE="$f"
        break
    fi
done

# Look for CHIME data
for f in "${SCRIPT_DIR}/chime_latency.dat" "${SCRIPT_DIR}/../../../CHIME/build/chime_latency.dat" "./chime_latency.dat"; do
    if [ -f "$f" ]; then
        CHIME_FILE="$f"
        break
    fi
done

if [ -z "$DEX_FILE" ]; then
    echo "ERROR: DEX latency data not found!"
    echo "Please copy dex_latency.dat to this directory or run the benchmark first."
    exit 1
fi

if [ -z "$CHIME_FILE" ]; then
    echo "ERROR: CHIME latency data not found!"
    echo "Please copy chime_latency.dat to this directory or run the benchmark first."
    exit 1
fi

echo "Found DEX data: ${DEX_FILE}"
echo "Found CHIME data: ${CHIME_FILE}"
echo ""

# Generate plots
python3 "${SCRIPT_DIR}/plot_latency_comparison.py" \
    --dex "${DEX_FILE}" \
    --chime "${CHIME_FILE}" \
    --output "${OUTPUT_DIR}/latency_comparison.png"

echo ""
echo "=========================================="
echo "Results saved to: ${OUTPUT_DIR}/"
echo "=========================================="
ls -la "${OUTPUT_DIR}/"
