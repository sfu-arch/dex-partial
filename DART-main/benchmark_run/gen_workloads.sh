#!/usr/bin/env bash
# gen_workloads.sh — Run on BOTH nodes (10.30.1.9 and 10.30.1.6)
# Generates YCSB workload files for all skew levels
# 100M records load, 10M operations run
set -e

cd "$(dirname "$0")/.."

YCSB_DIR="workload/ycsb-0.17.0"
SPEC_DIR="benchmark_run/workload_specs"
SPLIT_DIR="workload/split"

mkdir -p "$SPLIT_DIR"

if [ ! -f "$YCSB_DIR/bin/ycsb.sh" ]; then
    echo "ERROR: YCSB not found at $YCSB_DIR/bin/ycsb.sh"
    echo "Make sure ycsb-0.17.0 is extracted in workload/"
    exit 1
fi

# All configs share the same load file (100M inserts)
# Only need to generate it once with any spec (they all have recordcount=100000000)
CONFIGS=("uniform" "zipf03" "zipf05" "zipf08" "zipf099")
SPECS=("uniform_c.spec" "zipf03_c.spec" "zipf05_c.spec" "zipf08_c.spec" "zipf099_c.spec")

echo "============================================"
echo "  DART Workload Generation (10M records)"
echo "============================================"

# Generate the common load file (same for all — 100M inserts, distribution irrelevant)
LOAD_FILE="$SPLIT_DIR/10m_load"
if [ -f "$LOAD_FILE" ] && [ "$(wc -l < "$LOAD_FILE")" -gt 1000 ]; then
    echo "[SKIP] Load file $LOAD_FILE already exists ($(wc -l < "$LOAD_FILE") lines)"
else
    echo ""
    echo "[1/6] Generating load file: $LOAD_FILE (10M records)..."
    cd "$YCSB_DIR"
    bin/ycsb.sh load basic -P "../../$SPEC_DIR/uniform_c.spec" -s > "../../$LOAD_FILE"
    cd ../..
    echo "  Done: $(wc -l < "$LOAD_FILE") lines"
fi

# Generate run files for each distribution
for i in "${!CONFIGS[@]}"; do
    CONFIG="${CONFIGS[$i]}"
    SPEC="${SPECS[$i]}"
    RUN_FILE="$SPLIT_DIR/${CONFIG}_run"

    # Clean stale binary caches
    rm -f "${RUN_FILE}__bin_" "${RUN_FILE}__bin_buffer_"

    if [ -f "$RUN_FILE" ] && [ "$(wc -l < "$RUN_FILE")" -gt 1000 ]; then
        echo "[SKIP] Run file $RUN_FILE already exists ($(wc -l < "$RUN_FILE") lines)"
    else
        echo ""
        echo "[$((i+2))/6] Generating run file: $RUN_FILE ($CONFIG, 10M operations)..."
        cd "$YCSB_DIR"
        bin/ycsb.sh run basic -P "../../$SPEC_DIR/$SPEC" -s > "../../$RUN_FILE"
        cd ../..
        echo "  Done: $(wc -l < "$RUN_FILE") lines"
    fi
done

# Clean load file binary caches too
rm -f "${LOAD_FILE}__bin_" "${LOAD_FILE}__bin_buffer_"

echo ""
echo "============================================"
echo "  All workloads generated!"
echo "============================================"
echo ""
echo "Files in $SPLIT_DIR:"
ls -lh "$SPLIT_DIR/"
