#!/usr/bin/env python3
"""
QW1 Experiment: Parse DEX and CHIME results and generate comparison plots.

Plots generated:
1. Line plot: X = Zipfian skew parameter, Y = Average end-to-end query latency
2. Line plot: X = Zipfian skew parameter, Y = Remote bytes transferred per query
3. Histogram: X = RDMA read size (bytes), Y = frequency (DEX only)
4. Line plot: X = Zipfian skew parameter, Y = RDMA operation count per query
"""

import os
import re
import json
import argparse
from pathlib import Path
from typing import Dict, List, Tuple

# Check for matplotlib availability
try:
    import matplotlib.pyplot as plt
    import numpy as np
    HAS_MATPLOTLIB = True
except ImportError:
    HAS_MATPLOTLIB = False
    print("Warning: matplotlib not available. Install with: pip install matplotlib numpy")


def parse_dex_log(log_path: str) -> Dict:
    """Parse DEX benchmark output log file."""
    metrics = {}
    
    with open(log_path, 'r') as f:
        content = f.read()
    
    # Extract RDMA statistics
    patterns = {
        'rdma_read_per_op': r'Avg\. rdma read / op = ([\d.]+)',
        'rdma_write_per_op': r'Avg\. rdma write / op = ([\d.]+)',
        'rdma_cas_per_op': r'Avg\. rdma cas / op = ([\d.]+)',
        'rdma_total_per_op': r'Avg\. all rdma / op = ([\d.]+)',
        'rdma_read_size_per_op': r'Avg\. rdma read size/ op = ([\d.]+)',
        'rdma_write_size_per_op': r'Avg\. rdma write size / op = ([\d.]+)',
        'rdma_total_size_per_op': r'Avg\. rdma RW size / op = ([\d.]+)',
        'throughput_mops': r'Final throughput = ([\d.]+)',
    }
    
    for key, pattern in patterns.items():
        match = re.search(pattern, content)
        if match:
            metrics[key] = float(match.group(1))
    
    return metrics


def parse_chime_log(log_path: str) -> Dict:
    """Parse CHIME ycsb_test output log file."""
    metrics = {}
    
    with open(log_path, 'r') as f:
        content = f.read()
    
    # Extract metrics from CHIME output
    patterns = {
        'cache_hit_rate': r'cache hit rate: ([\d.]+)',
        'throughput_mops': r'cluster throughput ([\d.]+) Mops',
        'lock_fail_avg': r'avg\. lock/cas fail cnt: ([\d.]+)',
        'write_combining_rate': r'write combining rate: ([\d.]+)',
        'read_delegation_rate': r'read delegation rate: ([\d.]+)',
        'speculative_read_rate': r'speculative read rate: ([\d.]+)',
        'speculative_accuracy': r'correct ratio of speculative read: ([\d.]+)',
    }
    
    for key, pattern in patterns.items():
        match = re.search(pattern, content)
        if match:
            metrics[key] = float(match.group(1))
    
    return metrics


def parse_latency_file(lat_path: str) -> Dict:
    """Parse CHIME latency histogram file."""
    latencies = []
    counts = []
    
    with open(lat_path, 'r') as f:
        for line in f:
            parts = line.strip().split('\t')
            if len(parts) == 2:
                lat_us = float(parts[0])
                count = int(parts[1])
                if count > 0:
                    latencies.extend([lat_us] * count)
    
    if not latencies:
        return {}
    
    latencies.sort()
    total = len(latencies)
    
    def percentile(p):
        idx = int(total * p / 100)
        return latencies[min(idx, total - 1)]
    
    return {
        'p50': percentile(50),
        'p90': percentile(90),
        'p99': percentile(99),
        'p999': percentile(99.9),
        'avg': sum(latencies) / total,
    }


def collect_dex_results(results_dir: str) -> Dict:
    """Collect DEX results from all log files."""
    results = {}
    results_path = Path(results_dir)
    
    # Parse uniform
    uniform_log = results_path / 'uniform.log'
    if uniform_log.exists():
        results['uniform'] = parse_dex_log(str(uniform_log))
    
    # Parse Zipfian logs
    for theta in [0.6, 0.8, 0.9, 0.99]:
        log_file = results_path / f'zipf_{theta}.log'
        if log_file.exists():
            results[f'zipf_{theta}'] = parse_dex_log(str(log_file))
    
    return results


def collect_chime_results(results_dir: str) -> Dict:
    """Collect CHIME results from log and latency files."""
    results = {}
    results_path = Path(results_dir)
    
    # Parse main log
    for log_file in results_path.glob('chime_*.log'):
        results['metrics'] = parse_chime_log(str(log_file))
        break
    
    # Parse latency files
    latency_files = list(results_path.glob('epoch_*.lat'))
    if latency_files:
        # Use the last epoch for steady-state latency
        latency_files.sort(key=lambda x: int(re.search(r'epoch_(\d+)', x.name).group(1)))
        results['latency'] = parse_latency_file(str(latency_files[-1]))
    
    return results


def generate_plots(dex_results: Dict, chime_results: Dict, output_dir: str):
    """Generate comparison plots."""
    if not HAS_MATPLOTLIB:
        print("Skipping plot generation (matplotlib not available)")
        return
    
    output_path = Path(output_dir)
    output_path.mkdir(exist_ok=True)
    
    # Prepare data
    skew_labels = ['uniform', '0.6', '0.8', '0.9', '0.99']
    skew_values = [0, 0.6, 0.8, 0.9, 0.99]
    
    dex_rdma_ops = []
    dex_rdma_bytes = []
    dex_throughput = []
    
    for label in skew_labels:
        key = label if label == 'uniform' else f'zipf_{label}'
        if key in dex_results:
            dex_rdma_ops.append(dex_results[key].get('rdma_total_per_op', 0))
            dex_rdma_bytes.append(dex_results[key].get('rdma_total_size_per_op', 0))
            dex_throughput.append(dex_results[key].get('throughput_mops', 0))
        else:
            dex_rdma_ops.append(0)
            dex_rdma_bytes.append(0)
            dex_throughput.append(0)
    
    # Plot 1: RDMA operations per query
    fig, ax = plt.subplots(figsize=(10, 6))
    ax.plot(skew_labels, dex_rdma_ops, 'o-', label='DEX', linewidth=2, markersize=8)
    ax.set_xlabel('Key Distribution (Zipfian theta)', fontsize=12)
    ax.set_ylabel('RDMA Operations per Query', fontsize=12)
    ax.set_title('QW1: RDMA Operations vs Key Access Distribution', fontsize=14)
    ax.legend()
    ax.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(output_path / 'qw1_rdma_ops.png', dpi=150)
    plt.close()
    
    # Plot 2: Remote bytes per query
    fig, ax = plt.subplots(figsize=(10, 6))
    ax.plot(skew_labels, dex_rdma_bytes, 'o-', label='DEX', linewidth=2, markersize=8)
    ax.set_xlabel('Key Distribution (Zipfian theta)', fontsize=12)
    ax.set_ylabel('Remote Bytes per Query', fontsize=12)
    ax.set_title('QW1: Remote Bytes Transferred vs Key Access Distribution', fontsize=14)
    ax.legend()
    ax.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(output_path / 'qw1_remote_bytes.png', dpi=150)
    plt.close()
    
    # Plot 3: Throughput comparison
    fig, ax = plt.subplots(figsize=(10, 6))
    ax.plot(skew_labels, dex_throughput, 'o-', label='DEX', linewidth=2, markersize=8)
    ax.set_xlabel('Key Distribution (Zipfian theta)', fontsize=12)
    ax.set_ylabel('Throughput (Mops/s)', fontsize=12)
    ax.set_title('QW1: Throughput vs Key Access Distribution', fontsize=14)
    ax.legend()
    ax.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(output_path / 'qw1_throughput.png', dpi=150)
    plt.close()
    
    print(f"Plots saved to: {output_path}")


def print_summary(dex_results: Dict, chime_results: Dict):
    """Print summary of collected results."""
    print("\n" + "="*60)
    print("QW1 Experiment Results Summary")
    print("="*60)
    
    print("\n--- DEX Results ---")
    for key, metrics in dex_results.items():
        print(f"\n{key}:")
        for metric, value in metrics.items():
            print(f"  {metric}: {value:.4f}")
    
    print("\n--- CHIME Results ---")
    if 'metrics' in chime_results:
        print("\nMetrics:")
        for metric, value in chime_results['metrics'].items():
            print(f"  {metric}: {value:.4f}")
    
    if 'latency' in chime_results:
        print("\nLatency (µs):")
        for metric, value in chime_results['latency'].items():
            print(f"  {metric}: {value:.2f}")


def main():
    parser = argparse.ArgumentParser(description='QW1 Experiment Results Parser')
    parser.add_argument('--dex-dir', default='./results_qw1_dex',
                        help='Directory containing DEX results')
    parser.add_argument('--chime-dir', default='./results_qw1_chime',
                        help='Directory containing CHIME results')
    parser.add_argument('--output-dir', default='./plots',
                        help='Directory for output plots')
    parser.add_argument('--json', action='store_true',
                        help='Output results as JSON')
    args = parser.parse_args()
    
    # Collect results
    dex_results = {}
    chime_results = {}
    
    if Path(args.dex_dir).exists():
        dex_results = collect_dex_results(args.dex_dir)
    else:
        print(f"Warning: DEX results directory not found: {args.dex_dir}")
    
    if Path(args.chime_dir).exists():
        chime_results = collect_chime_results(args.chime_dir)
    else:
        print(f"Warning: CHIME results directory not found: {args.chime_dir}")
    
    # Output results
    if args.json:
        output = {
            'dex': dex_results,
            'chime': chime_results
        }
        print(json.dumps(output, indent=2))
    else:
        print_summary(dex_results, chime_results)
        
        if dex_results or chime_results:
            generate_plots(dex_results, chime_results, args.output_dir)


if __name__ == '__main__':
    main()
