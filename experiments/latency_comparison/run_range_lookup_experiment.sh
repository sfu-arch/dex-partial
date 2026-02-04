#!/bin/bash
# Run Complete DEX vs CHIME Range+Lookup Latency Comparison
# This script orchestrates the full experiment

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=============================================================="
echo "DEX vs CHIME Range+Lookup Latency Comparison"
echo "=============================================================="
echo ""
echo "Configuration:"
echo "  - 70% Lookups + 30% Range Scans"
echo "  - 5M operations each"
echo "  - 500ns latency buckets"
echo "  - DEX: Offloading DISABLED (RPC_RATE=0)"
echo ""
echo "This script will:"
echo "  1. Run DEX benchmark"
echo "  2. Run CHIME benchmark"
echo "  3. Generate comparison plots"
echo ""

# Check for required build artifacts
if [ ! -f "$SCRIPT_DIR/../../dex/build/newbench_latency" ]; then
    echo "ERROR: DEX newbench_latency not found. Please build first:"
    echo "  cd dex/build && cmake .. && make -j newbench_latency"
    exit 1
fi

if [ ! -f "$SCRIPT_DIR/../../CHIME/build/microbench_latency" ]; then
    echo "ERROR: CHIME microbench_latency not found. Please build first:"
    echo "  cd CHIME/build && cmake .. && make -j microbench_latency"
    exit 1
fi

echo "All binaries found."
echo ""
echo "=============================================================="
echo "IMPORTANT: This is a 2-node experiment!"
echo "=============================================================="
echo ""
echo "You need to run commands on TWO SERVERS:"
echo ""
echo "=== DEX Benchmark ==="
echo "On Memory Node (start FIRST):"
echo "  bash $SCRIPT_DIR/dex_range_lookup_node1.sh"
echo ""
echo "On Compute Node (wait 5 seconds, then start):"
echo "  bash $SCRIPT_DIR/dex_range_lookup_node0.sh"
echo ""
echo "=== CHIME Benchmark ==="
echo "On Memory Node (start FIRST):"
echo "  bash $SCRIPT_DIR/chime_range_lookup_node1.sh"
echo ""
echo "On Compute Node (wait 5 seconds, then start):"
echo "  bash $SCRIPT_DIR/chime_range_lookup_node0.sh"
echo ""
echo "After both complete, run plotting script:"
echo "  python $SCRIPT_DIR/plot_latency_comparison.py \\"
echo "      --dex $SCRIPT_DIR/../../dex/build/dex_latency.dat \\"
echo "      --chime $SCRIPT_DIR/../../CHIME/build/chime_latency.dat"
echo ""
