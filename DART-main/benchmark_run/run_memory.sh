#!/usr/bin/env bash
# run_memory.sh — Run on 10.30.1.9 in TERMINAL 2
# Starts the memory process for each config
# Auto-waits for monitor to be listening before connecting
set -e

cd "$(dirname "$0")/.."

MONITOR_IP="10.30.1.9"
CONFIGS=("uniform" "zipf03" "zipf05" "zipf08" "zipf099")
LABELS=("Uniform" "Zipfian-0.3" "Zipfian-0.5" "Zipfian-0.8" "Zipfian-0.99")

echo "============================================"
echo "  DART Memory — 10.30.1.9 (Terminal 2)"
echo "============================================"

for i in "${!CONFIGS[@]}"; do
    CONFIG="${CONFIGS[$i]}"
    LABEL="${LABELS[$i]}"

    echo ""
    echo "========================================"
    echo "  Config $((i+1))/5: $LABEL"
    echo "========================================"

    # Wait for monitor to be listening on port 9898
    echo "[*] Waiting for monitor on ${MONITOR_IP}:9898..."
    while ! bash -c "echo > /dev/tcp/${MONITOR_IP}/9898" 2>/dev/null; do
        sleep 1
    done
    sleep 1

    echo "[*] Starting memory node..."
    bin/memory --monitor_addr=${MONITOR_IP}:9898 --nic_index=0

    echo "[*] Memory exited for config $LABEL"
    echo ""
    sleep 3
done

echo "============================================"
echo "  All memory runs complete!"
echo "============================================"
