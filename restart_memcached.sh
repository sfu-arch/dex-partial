#!/bin/bash
# restart_memcached.sh
# Kills and restarts memcached on the memory node (10.30.1.9),
# then resets serverNum and clientNum to 0 so DEX/DART can sync cleanly.
#
# Run this from EITHER node before each benchmark run.
# Usage: bash restart_memcached.sh

set -euo pipefail

MEMCACHED_HOST="10.30.1.9"
MEMCACHED_PORT="11211"
SSH_PORT="404"
PID_FILE="/tmp/memcached.pid"

# Colour helpers
RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
info()  { echo -e "${GREEN}[+]${NC} $*"; }
warn()  { echo -e "${YELLOW}[!]${NC} $*"; }
error() { echo -e "${RED}[x]${NC} $*"; exit 1; }

# ── 1. Kill existing memcached on memory node ────────────────────────────────
info "Killing memcached on ${MEMCACHED_HOST}..."
ssh -p ${SSH_PORT} -o ConnectTimeout=5 root@${MEMCACHED_HOST} bash <<'REMOTE'
set -euo pipefail
# Try PID file first
if [ -f /tmp/memcached.pid ]; then
    PID=$(cat /tmp/memcached.pid)
    if kill -0 "$PID" 2>/dev/null; then
        kill "$PID"
        echo "  killed PID $PID from pid file"
    else
        echo "  stale pid file, removing"
    fi
    rm -f /tmp/memcached.pid
fi
# Also kill any stray memcached processes
if pgrep memcached >/dev/null 2>&1; then
    pkill memcached && echo "  pkill memcached done" || true
fi
sleep 0.5
echo "  memcached stopped"
REMOTE

# ── 2. Start fresh memcached on memory node ──────────────────────────────────
info "Starting memcached on ${MEMCACHED_HOST}:${MEMCACHED_PORT}..."
ssh -p ${SSH_PORT} -o ConnectTimeout=5 root@${MEMCACHED_HOST} \
    "memcached -u root -l ${MEMCACHED_HOST} -p ${MEMCACHED_PORT} \
               -c 10000 -m 64 -d -P ${PID_FILE}"
sleep 1

# ── 3. Verify memcached is up ────────────────────────────────────────────────
info "Verifying memcached is reachable at ${MEMCACHED_HOST}:${MEMCACHED_PORT}..."
for attempt in 1 2 3; do
    if echo "version" | timeout 2 nc ${MEMCACHED_HOST} ${MEMCACHED_PORT} | grep -q "VERSION"; then
        info "  memcached is up"
        break
    fi
    if [ "$attempt" -eq 3 ]; then
        error "memcached did not come up after 3 attempts — check SSH and firewall"
    fi
    warn "  attempt $attempt failed, retrying..."
    sleep 1
done

# ── 4. Reset sync counters ───────────────────────────────────────────────────
# DEX uses these keys to count how many servers/clients have connected.
# Stale values (e.g. serverNum=1 from last run) cause infinite hangs.
info "Resetting serverNum and clientNum to 0..."

set_key() {
    local KEY=$1 VAL=$2
    REPLY=$(printf "set %s 0 0 1\r\n%s\r\nquit\r\n" "$KEY" "$VAL" \
            | timeout 3 nc ${MEMCACHED_HOST} ${MEMCACHED_PORT} 2>/dev/null || true)
    if echo "$REPLY" | grep -q "STORED"; then
        info "  $KEY = $VAL  ✓"
    else
        warn "  $KEY set did not return STORED (reply: $(echo $REPLY | tr -d '\r\n'))"
    fi
}

set_key "serverNum" "0"
set_key "clientNum" "0"

# ── 5. Quick sanity read-back ────────────────────────────────────────────────
info "Verifying keys..."
get_key() {
    printf "get %s\r\nquit\r\n" "$1" \
        | timeout 3 nc ${MEMCACHED_HOST} ${MEMCACHED_PORT} 2>/dev/null \
        | grep -v "^END" | grep -v "^VALUE" | tr -d '\r\n ' || echo "(empty)"
}

SERVER_VAL=$(get_key "serverNum")
CLIENT_VAL=$(get_key "clientNum")
info "  serverNum = '${SERVER_VAL}'"
info "  clientNum = '${CLIENT_VAL}'"

echo ""
info "memcached restarted and reset — ready to run benchmark."
echo ""
echo "  Memory node : ${MEMCACHED_HOST}:${MEMCACHED_PORT}"
echo "  serverNum   : 0"
echo "  clientNum   : 0"
echo ""
echo "  Next: start your benchmark on both nodes."
