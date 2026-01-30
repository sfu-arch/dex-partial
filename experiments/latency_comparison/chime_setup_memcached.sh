#!/bin/bash
#
# CHIME Memcached Setup Script
# Run this FIRST on the compute node (10.30.1.9) before starting any benchmark nodes
#

echo "=========================================="
echo " CHIME Memcached Setup"
echo "=========================================="

# Kill any running benchmarks
echo "Cleaning up previous processes..."
sudo pkill -9 simple_bench 2>/dev/null || true
sudo pkill -9 microbench_latency 2>/dev/null || true
sudo pkill -9 ycsb_test 2>/dev/null || true
sudo pkill -9 ycsb_test_latency 2>/dev/null || true

# Flush memcached
echo "Flushing memcached..."
echo "flush_all" | nc -w1 127.0.0.1 11211 || echo "flush_all" | timeout 2 nc 127.0.0.1 11211 || true
sleep 1

# Initialize keys
echo "Initializing memcached keys..."
printf "set serverNum 0 0 1\r\n0\r\n" | nc -w1 127.0.0.1 11211 || printf "set serverNum 0 0 1\r\n0\r\n" | timeout 2 nc 127.0.0.1 11211 || true
printf "set clientNum 0 0 1\r\n0\r\n" | nc -w1 127.0.0.1 11211 || printf "set clientNum 0 0 1\r\n0\r\n" | timeout 2 nc 127.0.0.1 11211 || true

# Verify
echo ""
echo "Verifying memcached state:"
echo -n "serverNum: "
(echo "get serverNum"; sleep 0.2) | nc -w1 127.0.0.1 11211 2>/dev/null | grep -v "^END" | tail -1 || echo "?"
echo -n "clientNum: "
(echo "get clientNum"; sleep 0.2) | nc -w1 127.0.0.1 11211 2>/dev/null | grep -v "^END" | tail -1 || echo "?"

echo ""
echo "=========================================="
echo " Memcached ready! Now start nodes:"
echo " 1. Memory node (10.30.1.6): ./chime_microbench_node1.sh"
echo " 2. Wait 3-5 seconds"
echo " 3. Compute node (10.30.1.9): ./chime_microbench_node0.sh"
echo "=========================================="
