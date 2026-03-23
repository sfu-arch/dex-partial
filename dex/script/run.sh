#!/bin/bash
# run.sh — memory node (10.30.1.9)
# Restart memcached before each run, then launch newbench_latency.
# run_other.sh on the compute node (10.30.1.6) polls until memcached is ready.

#ops
read=(100 50 95 0 0)
insert=(0 0 0 100 5)
update=(0 50 5 0 0)
delete=(0 0 0 0 0)
range=(0 0 0 0 95)

#fixed-op
readonly=(100 0 0 0 0)
updateonly=(0 0 100 0 0)

#exp
threads=(0 2 18 36 72 108 144)
#threads=(0 2 16 32 64 96 128)
mem_threads=(0 4)
cache=(0 64 128 256 512 1024)
uniform=(0 1)
zipf=(0.99)
bulk=50
warmup=10
runnum=50
nodenum=2

#other
correct=0
timebase=1
early=1
#index=(0 1 2)
rpc=1
admit=0.1
tune=0

# ── memcached config ──────────────────────────────────────────────────────────
# memcached.conf is one level up when run from dex/build/, or same dir from dex/script/
CONF=$([ -f memcached.conf ] && echo memcached.conf || echo ../memcached.conf)
MEMC_ADDR=$(head -1 "$CONF")
MEMC_PORT=$(awk 'NR==2{print}' "$CONF")
PID_FILE=/tmp/memcached.pid
# Path to newbench_latency binary (relative to this script's location)
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEX_BUILD="$SCRIPT_DIR/../build"

restart_memcached() {
    echo "[memcached] restarting on ${MEMC_ADDR}:${MEMC_PORT} (local)..."

    # kill old instance locally (this script runs ON the memory node)
    if [ -f "$PID_FILE" ]; then
        PID=$(cat "$PID_FILE")
        sudo kill "$PID" 2>/dev/null && echo "  killed pid $PID" || echo "  stale pid file"
        sudo rm -f "$PID_FILE"
    fi
    pgrep memcached >/dev/null 2>&1 && sudo pkill memcached && echo "  pkill done" || true

    # wait for port to fully release (TIME_WAIT can hold it ~30s)
    for i in $(seq 1 30); do
        ss -tlnp "sport = :${MEMC_PORT}" 2>/dev/null | grep -q ":${MEMC_PORT}" || break
        [ "$i" -eq 30 ] && { echo "[memcached] ERROR: port ${MEMC_PORT} still held after 30s"; exit 1; }
        sleep 1
    done
    sleep 0.5

    # start fresh locally
    sudo memcached -l "${MEMC_ADDR}" -p "${MEMC_PORT}" -c 10000 -m 64 -d -P "${PID_FILE}"
    sleep 1

    # verify up
    for attempt in 1 2 3; do
        if echo "version" | timeout 2 nc ${MEMC_ADDR} ${MEMC_PORT} | grep -q "VERSION"; then
            echo "[memcached] up"
            break
        fi
        [ "$attempt" -eq 3 ] && { echo "[memcached] ERROR: did not come up"; exit 1; }
        sleep 2
    done

    # reset sync counters (stale values cause infinite hangs)
    printf "set serverNum 0 0 1\r\n0\r\nquit\r\n" | timeout 3 nc ${MEMC_ADDR} ${MEMC_PORT} > /dev/null
    printf "set clientNum 0 0 1\r\n0\r\nquit\r\n" | timeout 3 nc ${MEMC_ADDR} ${MEMC_PORT} > /dev/null
    echo "[memcached] serverNum=0 clientNum=0 — ready"
}

# ── main loop ─────────────────────────────────────────────────────────────────
for uni in 0
do
    for op in 0
    do
        for idx in 0
        do
            for t in 1
            do
                restart_memcached
                cd "$DEX_BUILD"
                sudo ./newbench_latency $nodenum ${read[$op]} ${insert[$op]} ${update[$op]} ${delete[$op]} ${range[$op]} ${threads[$t]} ${mem_threads[1]} ${cache[3]} $uni ${zipf[0]} $bulk $warmup $runnum $correct $timebase $early $idx $rpc $admit $tune 36
                cd "$SCRIPT_DIR"
                sleep 2
            done
        done
    done
done