#!/usr/bin/env python3
"""
Simple workload generator for CHIME that doesn't require YCSB.
Generates load and transaction files directly in the format CHIME expects.
"""

import os
import random
import sys

def generate_zipfian_keys(n, key_space, theta=0.99):
    """Generate n keys following Zipfian distribution"""
    # Pre-compute Zipfian probabilities
    zeta = sum(1.0 / (i ** theta) for i in range(1, key_space + 1))
    keys = []
    for _ in range(n):
        u = random.random()
        cum_prob = 0
        for k in range(1, key_space + 1):
            cum_prob += (1.0 / (k ** theta)) / zeta
            if u <= cum_prob:
                keys.append(k)
                break
        else:
            keys.append(key_space)
    return keys

def generate_workload(workload_type, num_records, num_ops, key_type, output_dir):
    """Generate YCSB-style workload files"""
    
    # Workload configurations (read_ratio, update_ratio, insert_ratio, scan_ratio)
    workloads = {
        'a': (0.5, 0.5, 0, 0),      # 50% read, 50% update
        'b': (0.95, 0.05, 0, 0),    # 95% read, 5% update
        'c': (1.0, 0, 0, 0),        # 100% read
        'd': (0.95, 0, 0.05, 0),    # 95% read, 5% insert (latest)
        'e': (0, 0, 0.05, 0.95),    # 5% insert, 95% scan
        'la': (0, 0, 1.0, 0),       # Load only (100% insert)
    }
    
    if workload_type not in workloads:
        print(f"Unknown workload type: {workload_type}")
        return
    
    read_ratio, update_ratio, insert_ratio, scan_ratio = workloads[workload_type]
    
    os.makedirs(output_dir, exist_ok=True)
    
    load_file = os.path.join(output_dir, f"load_{key_type}_workload{workload_type}")
    txn_file = os.path.join(output_dir, f"txn_{key_type}_workload{workload_type}")
    
    print(f"Generating workload {workload_type}:")
    print(f"  Records: {num_records}, Operations: {num_ops}")
    print(f"  Read: {read_ratio*100}%, Update: {update_ratio*100}%, Insert: {insert_ratio*100}%, Scan: {scan_ratio*100}%")
    
    # Generate load file (initial inserts)
    print(f"  Writing load file: {load_file}")
    with open(load_file, 'w') as f:
        for i in range(num_records):
            f.write(f"INSERT {i}\n")
    
    # Generate transaction file
    print(f"  Writing transaction file: {txn_file}")
    with open(txn_file, 'w') as f:
        insert_key = num_records  # Start new inserts after loaded keys
        
        for i in range(num_ops):
            r = random.random()
            
            # Pick a random existing key for read/update
            key = random.randint(0, num_records - 1)
            
            if r < read_ratio:
                f.write(f"READ {key}\n")
            elif r < read_ratio + update_ratio:
                f.write(f"UPDATE {key}\n")
            elif r < read_ratio + update_ratio + insert_ratio:
                f.write(f"INSERT {insert_key}\n")
                insert_key += 1
            else:
                # Scan - key and range size
                range_size = random.randint(1, 100)
                f.write(f"SCAN {key} {range_size}\n")
    
    print(f"  Done!")

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 generate_workloads_simple.py [small|full]")
        print("  small: 60K records, 60K ops (for testing)")
        print("  full:  40M records, 40M ops (for benchmarking)")
        sys.exit(1)
    
    size = sys.argv[1]
    
    if size == "small":
        num_records = 60000
        num_ops = 60000
    elif size == "full":
        num_records = 40000000
        num_ops = 40000000
    else:
        print(f"Unknown size: {size}")
        sys.exit(1)
    
    output_dir = os.path.join(os.path.dirname(__file__), "workloads")
    
    for workload_type in ['la', 'a', 'b', 'c', 'd', 'e']:
        generate_workload(workload_type, num_records, num_ops, "randint", output_dir)
        print()

if __name__ == "__main__":
    main()
