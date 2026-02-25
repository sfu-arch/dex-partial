#!/usr/bin/env python3
"""
QW3: Tree Height / Key Count Crossover Analysis

Plots DEX vs CHIME performance across varying key counts (tree heights)
with a fixed 64MB cache to find the crossover point.

Usage:
    python plot_qw3.py
"""

import os
import re
import numpy as np
import matplotlib.pyplot as plt
from pathlib import Path
from collections import defaultdict

# Configuration
SCRIPT_DIR = Path(__file__).parent
RESULTS_DIR = SCRIPT_DIR / "results"
PLOTS_DIR = SCRIPT_DIR / "plots"
PLOTS_DIR.mkdir(exist_ok=True)

KEY_COUNTS = [1, 5, 10, 20, 50, 100]  # millions
DISTRIBUTIONS = ["uniform", "zipf_0.6", "zipf_0.99"]
DIST_LABELS = {"uniform": "Uniform", "zipf_0.6": "Zipfian θ=0.6", "zipf_0.99": "Zipfian θ=0.99"}

# Plot styling
plt.rcParams.update({
    'font.size': 12,
    'axes.labelsize': 14,
    'axes.titlesize': 16,
    'legend.fontsize': 11,
    'xtick.labelsize': 12,
    'ytick.labelsize': 12,
    'figure.figsize': (10, 6),
    'lines.linewidth': 2,
    'lines.markersize': 8,
})

DEX_COLOR = '#2E86AB'
CHIME_COLOR = '#E94F37'


def parse_latency_stats_from_log(log_path):
    """Extract latency statistics from stdout log."""
    stats = {}
    try:
        with open(log_path, 'r') as f:
            content = f.read()
        
        # Parse average latency
        avg_match = re.search(r'Average latency:\s*([\d.]+)\s*ns', content)
        if avg_match:
            stats['avg_latency_ns'] = float(avg_match.group(1))
        
        # Parse P50 latency
        p50_match = re.search(r'P50 latency:\s*([\d.]+)\s*ns', content)
        if p50_match:
            stats['p50_latency_ns'] = float(p50_match.group(1))
        
        # Parse P99 latency
        p99_match = re.search(r'P99 latency:\s*([\d.]+)\s*ns', content)
        if p99_match:
            stats['p99_latency_ns'] = float(p99_match.group(1))
        
        # Parse tree height
        height_match = re.search(r'Tree height\s*=\s*(\d+)', content)
        if height_match:
            stats['tree_height'] = int(height_match.group(1))
        
        # Parse leaf nodes
        leaf_match = re.search(r'#leaf nodes\s*=\s*(\d+)', content)
        if leaf_match:
            stats['leaf_nodes'] = int(leaf_match.group(1))
        
        # Parse throughput (if available)
        tp_match = re.search(r'Throughput:\s*([\d.]+)\s*(M?ops/sec|Mops/s)', content)
        if tp_match:
            tp = float(tp_match.group(1))
            if 'M' in tp_match.group(2):
                tp *= 1e6
            stats['throughput'] = tp
        
    except FileNotFoundError:
        pass
    
    return stats


def load_results():
    """Load all experiment results."""
    results = {
        'dex': defaultdict(dict),
        'chime': defaultdict(dict)
    }
    
    for system in ['dex', 'chime']:
        system_dir = RESULTS_DIR / system
        if not system_dir.exists():
            print(f"Warning: {system_dir} not found")
            continue
        
        for key_m in KEY_COUNTS:
            for dist in DISTRIBUTIONS:
                log_pattern = f"{system}_keys_{key_m}M_{dist}_stdout.log"
                log_path = system_dir / log_pattern
                
                # Also try node1 logs for CHIME
                if not log_path.exists() and system == 'chime':
                    log_pattern = f"chime_keys_{key_m}M_{dist}_node1_stdout.log"
                    log_path = system_dir / log_pattern
                
                stats = parse_latency_stats_from_log(log_path)
                if stats:
                    results[system][(key_m, dist)] = stats
                    print(f"Loaded: {system} keys={key_m}M dist={dist} -> {stats}")
    
    return results


def find_crossover_point(dex_latencies, chime_latencies, key_counts):
    """Find the key count where CHIME becomes faster than DEX."""
    for i, key_m in enumerate(key_counts):
        if i >= len(dex_latencies) or i >= len(chime_latencies):
            continue
        if dex_latencies[i] > chime_latencies[i]:
            if i == 0:
                return key_m, "from start"
            else:
                # Linear interpolation
                prev_key = key_counts[i-1]
                dex_prev, dex_curr = dex_latencies[i-1], dex_latencies[i]
                chime_prev, chime_curr = chime_latencies[i-1], chime_latencies[i]
                
                # Solve for crossover
                # dex_prev + (x - prev_key) * slope_dex = chime_prev + (x - prev_key) * slope_chime
                slope_dex = (dex_curr - dex_prev) / (key_m - prev_key)
                slope_chime = (chime_curr - chime_prev) / (key_m - prev_key)
                
                if slope_dex != slope_chime:
                    x = prev_key + (chime_prev - dex_prev) / (slope_dex - slope_chime)
                    return x, "interpolated"
                return key_m, "exact"
    
    return None, "no crossover"


def plot_latency_vs_keys(results, metric='avg_latency_ns'):
    """Plot latency vs key count for each distribution."""
    fig, axes = plt.subplots(1, 3, figsize=(15, 5))
    
    for ax, dist in zip(axes, DISTRIBUTIONS):
        dex_latencies = []
        chime_latencies = []
        valid_keys = []
        
        for key_m in KEY_COUNTS:
            dex_stats = results['dex'].get((key_m, dist), {})
            chime_stats = results['chime'].get((key_m, dist), {})
            
            if metric in dex_stats and metric in chime_stats:
                valid_keys.append(key_m)
                dex_latencies.append(dex_stats[metric] / 1000)  # Convert to μs
                chime_latencies.append(chime_stats[metric] / 1000)
        
        if valid_keys:
            ax.plot(valid_keys, dex_latencies, 'o-', color=DEX_COLOR, label='DEX')
            ax.plot(valid_keys, chime_latencies, 's-', color=CHIME_COLOR, label='CHIME')
            
            # Find and mark crossover
            crossover, how = find_crossover_point(dex_latencies, chime_latencies, valid_keys)
            if crossover:
                ax.axvline(x=crossover, color='gray', linestyle='--', alpha=0.7)
                ax.annotate(f'Crossover\n~{crossover:.1f}M', 
                           xy=(crossover, ax.get_ylim()[1]*0.8),
                           fontsize=10, ha='center')
        
        ax.set_xlabel('Key Count (millions)')
        ax.set_ylabel('Latency (μs)')
        ax.set_title(f'{DIST_LABELS[dist]}')
        ax.legend()
        ax.grid(True, alpha=0.3)
        ax.set_xscale('log')
    
    fig.suptitle('QW3: Latency vs Key Count (64MB Cache)', fontsize=16, y=1.02)
    plt.tight_layout()
    plt.savefig(PLOTS_DIR / 'qw3_latency_vs_keys.png', dpi=150, bbox_inches='tight')
    plt.savefig(PLOTS_DIR / 'qw3_latency_vs_keys.pdf', bbox_inches='tight')
    print(f"Saved: {PLOTS_DIR / 'qw3_latency_vs_keys.png'}")
    plt.close()


def plot_speedup(results):
    """Plot CHIME speedup over DEX."""
    fig, ax = plt.subplots(figsize=(10, 6))
    
    markers = ['o', 's', '^']
    colors = ['#2E86AB', '#E94F37', '#4CAF50']
    
    for dist, marker, color in zip(DISTRIBUTIONS, markers, colors):
        speedups = []
        valid_keys = []
        
        for key_m in KEY_COUNTS:
            dex_stats = results['dex'].get((key_m, dist), {})
            chime_stats = results['chime'].get((key_m, dist), {})
            
            if 'avg_latency_ns' in dex_stats and 'avg_latency_ns' in chime_stats:
                valid_keys.append(key_m)
                speedup = dex_stats['avg_latency_ns'] / chime_stats['avg_latency_ns']
                speedups.append(speedup)
        
        if valid_keys:
            ax.plot(valid_keys, speedups, f'{marker}-', color=color, 
                   label=DIST_LABELS[dist], linewidth=2, markersize=8)
    
    ax.axhline(y=1.0, color='gray', linestyle='--', linewidth=1.5, label='Equal performance')
    ax.fill_between(KEY_COUNTS, 1.0, 0, alpha=0.1, color='red', label='DEX faster')
    ax.fill_between(KEY_COUNTS, 1.0, 3, alpha=0.1, color='green', label='CHIME faster')
    
    ax.set_xlabel('Key Count (millions)')
    ax.set_ylabel('Speedup (DEX latency / CHIME latency)')
    ax.set_title('QW3: CHIME Speedup over DEX (64MB Cache)')
    ax.legend(loc='best')
    ax.grid(True, alpha=0.3)
    ax.set_xscale('log')
    ax.set_ylim(0.5, 2.5)
    
    plt.tight_layout()
    plt.savefig(PLOTS_DIR / 'qw3_speedup.png', dpi=150, bbox_inches='tight')
    plt.savefig(PLOTS_DIR / 'qw3_speedup.pdf', bbox_inches='tight')
    print(f"Saved: {PLOTS_DIR / 'qw3_speedup.png'}")
    plt.close()


def plot_tree_height(results):
    """Plot tree height vs key count."""
    fig, ax = plt.subplots(figsize=(8, 5))
    
    # Use DEX heights (should be same for CHIME)
    heights = []
    valid_keys = []
    
    for key_m in KEY_COUNTS:
        stats = results['dex'].get((key_m, 'uniform'), {})
        if 'tree_height' in stats:
            valid_keys.append(key_m)
            heights.append(stats['tree_height'])
    
    if valid_keys:
        ax.plot(valid_keys, heights, 'o-', color='#333', linewidth=2, markersize=10)
        for k, h in zip(valid_keys, heights):
            ax.annotate(f'H={h}', xy=(k, h), xytext=(0, 10), 
                       textcoords='offset points', ha='center', fontsize=11)
    
    ax.set_xlabel('Key Count (millions)')
    ax.set_ylabel('Tree Height')
    ax.set_title('B+ Tree Height vs Key Count')
    ax.grid(True, alpha=0.3)
    ax.set_xscale('log')
    
    plt.tight_layout()
    plt.savefig(PLOTS_DIR / 'qw3_tree_height.png', dpi=150, bbox_inches='tight')
    print(f"Saved: {PLOTS_DIR / 'qw3_tree_height.png'}")
    plt.close()


def print_summary(results):
    """Print summary statistics and crossover analysis."""
    print("\n" + "="*70)
    print("QW3: CROSSOVER ANALYSIS SUMMARY")
    print("="*70)
    
    for dist in DISTRIBUTIONS:
        print(f"\n{DIST_LABELS[dist]}:")
        print("-" * 40)
        
        dex_latencies = []
        chime_latencies = []
        valid_keys = []
        
        for key_m in KEY_COUNTS:
            dex_stats = results['dex'].get((key_m, dist), {})
            chime_stats = results['chime'].get((key_m, dist), {})
            
            if 'avg_latency_ns' in dex_stats and 'avg_latency_ns' in chime_stats:
                valid_keys.append(key_m)
                dex_lat = dex_stats['avg_latency_ns']
                chime_lat = chime_stats['avg_latency_ns']
                dex_latencies.append(dex_lat)
                chime_latencies.append(chime_lat)
                
                winner = "CHIME" if chime_lat < dex_lat else "DEX"
                speedup = dex_lat / chime_lat
                print(f"  {key_m:3}M keys: DEX={dex_lat:8.1f}ns  CHIME={chime_lat:8.1f}ns  "
                      f"Speedup={speedup:.2f}x  Winner={winner}")
        
        crossover, how = find_crossover_point(dex_latencies, chime_latencies, valid_keys)
        if crossover:
            print(f"\n  → CROSSOVER POINT: ~{crossover:.1f}M keys ({how})")
        else:
            print(f"\n  → No crossover detected in tested range")
    
    print("\n" + "="*70)


def main():
    print("QW3: Tree Height / Key Count Crossover Analysis")
    print("=" * 50)
    
    results = load_results()
    
    if not any(results['dex']) and not any(results['chime']):
        print("\nNo results found. Please run experiments first:")
        print("  ./qw3_dex_node0.sh   (on node 0)")
        print("  ./qw3_dex_node1.sh   (on node 1)")
        print("  ./qw3_chime_node0.sh (on node 0)")
        print("  ./qw3_chime_node1.sh (on node 1)")
        return
    
    print_summary(results)
    plot_latency_vs_keys(results)
    plot_speedup(results)
    plot_tree_height(results)
    
    print(f"\nPlots saved to: {PLOTS_DIR}")


if __name__ == "__main__":
    main()
