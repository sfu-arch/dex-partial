#!/bin/bash
# run_other.sh — compute node (10.30.1.6)
# Waits for the memory node to restart memcached and reset counters,
# then launches newbench_latency to pair with the memory node's run.sh.

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
MEMC_ADDR=$(head -1 memcached.conf)
MEMC_PORT=$(awk 'NR==2{print}' memcached.conf)

wait_for_memcached() {
    echo "[wait] waiting for memcached on ${MEMC_ADDR}:${MEMC_PORT} to be reset..."
    local attempts=0
    local max=30  # 30s timeout

    while [ $attempts -lt $max ]; do
        # Check memcached is up
        if ! echo "version" | timeout 2 nc ${MEMC_ADDR} ${MEMC_PORT} 2>/dev/null | grep -q "VERSION"; then
            echo "[wait]   memcached not up yet ($((attempts+1))/${max})..."
            sleep 1; attempts=$((attempts+1)); continue
        fi

        # Check serverNum=0 (memory node has reset it)
        SERVER=$(printf "get serverNum\r\nquit\r\n" \
            | timeout 2 nc ${MEMC_ADDR} ${MEMC_PORT} 2>/dev/null \
            | grep -v "^VALUE\|^END" | tr -d '\r\n ' || echo "x")

        if [ "$SERVER" = "0" ]; then
            echo "[wait] memcached ready — serverNum=0"
            return 0
        fi

        echo "[wait]   serverNum='${SERVER}', not reset yet ($((attempts+1))/${max})..."
        sleep 1; attempts=$((attempts+1))
    done

    echo "[wait] ERROR: memcached never became ready after ${max}s"
    exit 1
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
                wait_for_memcached
                sudo ./newbench_latency $nodenum ${read[$op]} ${insert[$op]} ${update[$op]} ${delete[$op]} ${range[$op]} ${threads[$t]} ${mem_threads[1]} ${cache[3]} $uni ${zipf[0]} $bulk $warmup $runnum $correct $timebase $early $idx $rpc $admit $tune 36
            done
        done
    done
done