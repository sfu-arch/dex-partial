#!/bin/bash
#############################################################################
# Reset Memcached Script
#
# This script completely flushes and resets memcached for a clean start.
# Run this on the MEMORY NODE before starting any experiment.
#
# What it does:
#   1. Kills all related processes (tbc_bench, newbench, memcached)
#   2. Starts fresh memcached
#   3. Flushes all keys
#   4. Initializes coordination keys (serverNum=0, clientNum=0)
#   5. Verifies memcached is working
#
# USAGE: ./reset_memcached.sh [memory_node_ip] [port]
#   Examples:
#     ./reset_memcached.sh                    # Use defaults
#     ./reset_memcached.sh 10.30.1.9 11211    # Specify IP and port
#############################################################################

set -e

# Configuration (edit or pass as arguments)
MEMORY_NODE_IP="${1:-10.30.1.9}"
MEMCACHED_PORT="${2:-11211}"

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

log()   { echo -e "${CYAN}[reset]${NC} $1"; }
ok()    { echo -e "${GREEN}[OK]${NC} $1"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $1"; }
fail()  { echo -e "${RED}[FAIL]${NC} $1"; }

echo ""
echo "╔════════════════════════════════════════════════════════════╗"
echo "║          Memcached Reset Script                            ║"
echo "╠════════════════════════════════════════════════════════════╣"
echo "║  Target: $MEMORY_NODE_IP:$MEMCACHED_PORT"
echo "╚════════════════════════════════════════════════════════════╝"
echo ""

# Step 1: Kill processes
log "Step 1/5: Killing processes..."
sudo pkill -9 tbc_bench 2>/dev/null && ok "Killed tbc_bench" || true
sudo pkill -9 newbench 2>/dev/null && ok "Killed newbench" || true
sudo pkill -9 newbench_latency 2>/dev/null && ok "Killed newbench_latency" || true
sudo pkill -9 latency_bench 2>/dev/null && ok "Killed latency_bench" || true
sudo pkill -9 memcached 2>/dev/null && ok "Killed memcached" || true
sleep 2

# Step 2: Start fresh memcached
log "Step 2/5: Starting fresh memcached..."
sudo memcached -u root -l 0.0.0.0 -p "$MEMCACHED_PORT" -c 10000 -d
sleep 2

if pgrep -x memcached > /dev/null; then
    ok "memcached is running"
else
    fail "memcached failed to start!"
    exit 1
fi

# Step 3: Flush all keys
log "Step 3/5: Flushing all keys..."
FLUSH_RESULT=$(printf "flush_all\r\nquit\r\n" | nc -w 3 "$MEMORY_NODE_IP" "$MEMCACHED_PORT" 2>/dev/null | head -1)
if [[ "$FLUSH_RESULT" == *"OK"* ]]; then
    ok "flush_all completed"
else
    warn "flush_all returned: $FLUSH_RESULT"
fi
sleep 1

# Step 4: Initialize coordination keys
log "Step 4/5: Initializing coordination keys..."
printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 3 "$MEMORY_NODE_IP" "$MEMCACHED_PORT" > /dev/null
printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | nc -w 3 "$MEMORY_NODE_IP" "$MEMCACHED_PORT" > /dev/null
ok "Set serverNum=0, clientNum=0"
sleep 1

# Step 5: Verify
log "Step 5/5: Verifying..."
REPLY=$(printf "get serverNum\r\nquit\r\n" | nc -w 3 "$MEMORY_NODE_IP" "$MEMCACHED_PORT" 2>/dev/null)

if [[ "$REPLY" == *"VALUE serverNum"* ]]; then
    ok "Verification passed!"
    echo ""
    echo "╔════════════════════════════════════════════════════════════╗"
    echo "║  ${GREEN}✓ Memcached reset complete${NC}                                ║"
    echo "║                                                            ║"
    echo "║  memcached running on: $MEMORY_NODE_IP:$MEMCACHED_PORT"
    echo "║  serverNum = 0                                             ║"
    echo "║  clientNum = 0                                             ║"
    echo "║                                                            ║"
    echo "║  Ready to run experiments!                                 ║"
    echo "╚════════════════════════════════════════════════════════════╝"
    echo ""
else
    fail "Verification failed!"
    echo "Reply was: $REPLY"
    exit 1
fi
