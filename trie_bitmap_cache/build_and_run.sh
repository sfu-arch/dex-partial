#!/bin/bash
# ----------------------------------------------------------------------
# build_and_run.sh  —  Build TBC and optionally run a benchmark
#
# Usage:
#   ./build_and_run.sh              # build only
#   ./build_and_run.sh run          # build + run with default params
#   ./build_and_run.sh run <args>   # build + run with custom params
# ----------------------------------------------------------------------

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

echo "=== Building TBC (Trie+Bitmap Cache) ==="
mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc) tbc_bench

echo "=== Build complete: ${BUILD_DIR}/tbc_bench ==="

if [ "$1" = "run" ]; then
    shift
    if [ $# -eq 0 ]; then
        # Default: 2 nodes, 100% reads, 16 threads, 256MB cache, zipfian 0.99,
        # 10M bulk, 2M warmup, 20M ops
        echo "=== Running with default parameters ==="
        exec "${BUILD_DIR}/tbc_bench" \
            2           \
            100 0 0 0 0 \
            16 1        \
            256         \
            0 0.99      \
            10 2 20     \
            0 1 1       \
            0           \
            0 1         \
            0 32
    else
        echo "=== Running with custom parameters ==="
        exec "${BUILD_DIR}/tbc_bench" "$@"
    fi
fi
