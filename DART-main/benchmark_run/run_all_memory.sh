#!/usr/bin/env bash
set -e
cd "$(dirname "$0")/.."

CONFIGS="uniform zipf03 zipf05 zipf08 zipf099"
MONITOR_IP="10.30.1.9"

i=0
for CONFIG in $CONFIGS; do
    i=$((i+1))
    echo ""
    echo "========================================"
    echo "  Config $i/5: $CONFIG (memory)"
    echo "========================================"
    echo "[*] Waiting for monitor on ${MONITOR_IP}:9898..."
    while ! bash -c "echo > /dev/tcp/${MONITOR_IP}/9898" 2>/dev/null; do
        sleep 1
    done
    sleep 1
    echo "[*] Starting memory node..."
    bin/memory --monitor_addr=${MONITOR_IP}:9898 --nic_index=0
    echo "[*] Memory exited for $CONFIG"
    sleep 3
done
echo "All memory runs complete!"
