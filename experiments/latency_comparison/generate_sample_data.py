#!/usr/bin/env python3
"""
Generate sample latency data for testing the plotting script.
This creates realistic-looking latency histograms without running actual benchmarks.

Usage: python generate_sample_data.py
"""

import numpy as np
import os

def generate_latency_distribution(name, total_ops, mean, std, tail_factor=3, tail_probability=0.01):
    """
    Generate a realistic latency distribution combining normal and tail distributions.
    """
    # Main distribution (log-normal for realistic latency)
    main_samples = int(total_ops * (1 - tail_probability))
    main_latencies = np.random.lognormal(mean=np.log(mean), sigma=std/mean, size=main_samples)
    
    # Tail distribution (higher latencies)
    tail_samples = total_ops - main_samples
    tail_latencies = np.random.lognormal(mean=np.log(mean * tail_factor), sigma=std/mean * 2, size=tail_samples)
    
    all_latencies = np.concatenate([main_latencies, tail_latencies])
    all_latencies = np.maximum(1, all_latencies).astype(int)  # Minimum 1us
    all_latencies = np.minimum(99999, all_latencies)  # Maximum 99999us
    
    # Create histogram
    histogram = np.zeros(100000, dtype=np.int64)
    for lat in all_latencies:
        histogram[lat] += 1
    
    return histogram


def calculate_stats(histogram):
    """Calculate statistics from histogram."""
    latencies = np.arange(len(histogram))
    total_ops = np.sum(histogram)
    
    # Average
    avg = np.sum(latencies * histogram) / total_ops
    
    # Percentiles
    cumsum = np.cumsum(histogram)
    
    def find_percentile(p):
        target = total_ops * p
        idx = np.searchsorted(cumsum, target)
        return idx if idx < len(histogram) else len(histogram) - 1
    
    p50 = find_percentile(0.50)
    p90 = find_percentile(0.90)
    p95 = find_percentile(0.95)
    p99 = find_percentile(0.99)
    p999 = find_percentile(0.999)
    
    return {
        'total_ops': int(total_ops),
        'avg': avg,
        'p50': p50,
        'p90': p90,
        'p95': p95,
        'p99': p99,
        'p999': p999
    }


def save_histogram(filename, histogram, system_name):
    """Save histogram to file."""
    stats = calculate_stats(histogram)
    
    with open(filename, 'w') as f:
        f.write(f"# {system_name} Latency Histogram (SAMPLE DATA)\n")
        f.write(f"# Total ops: {stats['total_ops']}\n")
        f.write(f"# Avg: {stats['avg']:.2f} us\n")
        f.write(f"# P50: {stats['p50']} us, P90: {stats['p90']} us, P95: {stats['p95']} us, P99: {stats['p99']} us, P99.9: {stats['p999']} us\n")
        f.write("# latency_us\tcount\n")
        
        for i, count in enumerate(histogram):
            if count > 0:
                f.write(f"{i}\t{count}\n")
    
    return stats


def main():
    print("Generating sample latency data for DEX vs CHIME comparison...")
    print("(This is simulated data for testing the plotting script)\n")
    
    total_ops = 5000000
    
    # DEX: Lower average latency, tighter distribution
    print("Generating DEX sample data...")
    dex_histogram = generate_latency_distribution(
        name="DEX",
        total_ops=total_ops,
        mean=12,      # Lower mean
        std=8,        # Tighter distribution
        tail_factor=4,
        tail_probability=0.005
    )
    dex_stats = save_histogram("dex_latency.dat", dex_histogram, "DEX")
    
    # CHIME: Slightly higher average, wider distribution
    print("Generating CHIME sample data...")
    chime_histogram = generate_latency_distribution(
        name="CHIME",
        total_ops=total_ops,
        mean=18,      # Higher mean
        std=12,       # Wider distribution
        tail_factor=5,
        tail_probability=0.01
    )
    chime_stats = save_histogram("chime_latency.dat", chime_histogram, "CHIME")
    
    # Print comparison
    print("\n" + "="*60)
    print("SAMPLE DATA STATISTICS")
    print("="*60)
    print(f"\n{'Metric':<15} {'DEX':>15} {'CHIME':>15}")
    print("-"*60)
    print(f"{'Total Ops':<15} {dex_stats['total_ops']:>15,} {chime_stats['total_ops']:>15,}")
    print(f"{'Average (μs)':<15} {dex_stats['avg']:>15.2f} {chime_stats['avg']:>15.2f}")
    print(f"{'P50 (μs)':<15} {dex_stats['p50']:>15} {chime_stats['p50']:>15}")
    print(f"{'P90 (μs)':<15} {dex_stats['p90']:>15} {chime_stats['p90']:>15}")
    print(f"{'P95 (μs)':<15} {dex_stats['p95']:>15} {chime_stats['p95']:>15}")
    print(f"{'P99 (μs)':<15} {dex_stats['p99']:>15} {chime_stats['p99']:>15}")
    print(f"{'P99.9 (μs)':<15} {dex_stats['p999']:>15} {chime_stats['p999']:>15}")
    print("="*60)
    
    print("\nSample data files created:")
    print("  - dex_latency.dat")
    print("  - chime_latency.dat")
    print("\nYou can now test the plotting script:")
    print("  python plot_latency_comparison.py")


if __name__ == '__main__':
    main()
