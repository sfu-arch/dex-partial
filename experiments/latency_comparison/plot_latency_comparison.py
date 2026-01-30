#!/usr/bin/env python3
"""
DEX vs CHIME Latency Comparison Plot

This script reads latency histogram data files from both DEX and CHIME
and creates superimposed histogram plots for comparison.

Usage:
    python plot_latency_comparison.py [dex_latency.dat] [chime_latency.dat] [output.png]

If no arguments provided, uses default filenames in current directory.
"""

import sys
import os
import numpy as np
import matplotlib.pyplot as plt
from matplotlib.ticker import FuncFormatter
import argparse

# Set style for publication-quality plots
plt.style.use('seaborn-v0_8-whitegrid')
plt.rcParams.update({
    'font.size': 12,
    'axes.labelsize': 14,
    'axes.titlesize': 16,
    'legend.fontsize': 12,
    'figure.figsize': (12, 8),
    'figure.dpi': 150,
})

def parse_latency_file(filepath):
    """
    Parse a latency histogram data file.
    
    Returns:
        latencies: numpy array of latency values (in nanoseconds or microseconds)
        counts: numpy array of occurrence counts
        stats: dictionary with statistics from header
        unit: 'ns' or 'us' depending on file format
    """
    latencies = []
    counts = []
    stats = {}
    unit = 'us'  # default to microseconds
    
    with open(filepath, 'r') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            
            # Parse header comments for statistics
            if line.startswith('#'):
                # Detect unit from header
                if 'nanoseconds' in line.lower() or 'latency_ns' in line.lower():
                    unit = 'ns'
                if 'Total ops:' in line:
                    stats['total_ops'] = int(line.split(':')[1].strip())
                elif 'Avg:' in line:
                    parts = line.split(',')
                    for part in parts:
                        part = part.replace('ns', '').replace('us', '')
                        if 'Avg:' in part:
                            stats['avg'] = float(part.split(':')[1].strip())
                        elif 'P50:' in part:
                            stats['p50'] = int(float(part.split(':')[1].strip()))
                        elif 'P90:' in part:
                            stats['p90'] = int(float(part.split(':')[1].strip()))
                        elif 'P95:' in part:
                            stats['p95'] = int(float(part.split(':')[1].strip()))
                        elif 'P99:' in part:
                            stats['p99'] = int(float(part.split(':')[1].strip()))
                        elif 'P99.9:' in part:
                            stats['p999'] = int(float(part.split(':')[1].strip()))
                continue
            
            # Parse data lines (latency \t count)
            parts = line.split()
            if len(parts) >= 2:
                try:
                    lat = int(parts[0])
                    cnt = int(parts[1])
                    latencies.append(lat)
                    counts.append(cnt)
                except ValueError:
                    continue
    
    stats['unit'] = unit
    return np.array(latencies), np.array(counts), stats


def calculate_percentiles(latencies, counts, percentiles=[50, 90, 95, 99, 99.9]):
    """Calculate percentiles from histogram data."""
    total = np.sum(counts)
    cumulative = np.cumsum(counts)
    
    results = {}
    for p in percentiles:
        target = total * (p / 100.0)
        idx = np.searchsorted(cumulative, target)
        if idx < len(latencies):
            results[f'p{p}'] = latencies[idx]
        else:
            results[f'p{p}'] = latencies[-1]
    
    return results


def expand_histogram(latencies, counts, max_samples=100000):
    """
    Expand histogram data into individual samples for CDF/violin plots.
    Limits samples to max_samples by downsampling if necessary.
    """
    total = np.sum(counts)
    if total <= max_samples:
        samples = np.repeat(latencies, counts)
    else:
        # Downsample
        factor = total / max_samples
        scaled_counts = np.maximum(1, (counts / factor).astype(int))
        samples = np.repeat(latencies, scaled_counts)
    
    return samples


def plot_histogram_comparison(dex_data, chime_data, output_path='latency_comparison.png'):
    """
    Create comprehensive latency comparison plots with operation counts.
    """
    dex_lat, dex_cnt, dex_stats = dex_data
    chime_lat, chime_cnt, chime_stats = chime_data
    
    # Get total operations
    dex_total = np.sum(dex_cnt)
    chime_total = np.sum(chime_cnt)
    
    # Determine units from file
    dex_unit = dex_stats.get('unit', 'us')
    chime_unit = chime_stats.get('unit', 'us')
    
    # Convert to microseconds if nanoseconds
    if dex_unit == 'ns':
        dex_lat_us = dex_lat / 1000.0
    else:
        dex_lat_us = dex_lat.astype(float)
    
    if chime_unit == 'ns':
        chime_lat_us = chime_lat / 1000.0
    else:
        chime_lat_us = chime_lat.astype(float)
    
    # Create figure with multiple subplots
    fig = plt.figure(figsize=(16, 14))
    
    # Add main title with operation counts
    fig.suptitle(
        f'DEX vs CHIME Latency Comparison (100% Reads)\n'
        f'DEX: {dex_total:,} ops | CHIME: {chime_total:,} ops',
        fontsize=18, fontweight='bold', y=0.98
    )
    
    # 1. Superimposed Histogram (main plot)
    ax1 = fig.add_subplot(2, 2, 1)
    
    # Determine reasonable x-axis range (up to P99.9)
    dex_p999 = dex_stats.get('p999', np.max(dex_lat_us))
    chime_p999 = chime_stats.get('p999', np.max(chime_lat_us))
    max_lat = max(dex_p999, chime_p999) * 1.1
    
    # Filter data for plotting
    dex_mask = dex_lat_us <= max_lat
    chime_mask = chime_lat_us <= max_lat
    
    # Normalize to probability density
    ax1.bar(dex_lat_us[dex_mask], dex_cnt[dex_mask] / dex_total * 100, 
            width=0.5, alpha=0.6, label=f'DEX ({dex_total:,} ops)', color='#2ecc71', edgecolor='none')
    ax1.bar(chime_lat_us[chime_mask], chime_cnt[chime_mask] / chime_total * 100, 
            width=0.5, alpha=0.6, label=f'CHIME ({chime_total:,} ops)', color='#3498db', edgecolor='none')
    
    ax1.set_xlabel('Latency (μs)')
    ax1.set_ylabel('Percentage of Operations (%)')
    ax1.set_title('Latency Distribution')
    ax1.legend(loc='upper right')
    ax1.set_xlim(0, max_lat)
    
    # Add percentile lines
    for name, stats, color, ls in [('DEX', dex_stats, '#27ae60', '--'), ('CHIME', chime_stats, '#2980b9', ':')]:
        p50 = stats.get('p50', 0)
        p99 = stats.get('p99', 0)
        if stats.get('unit', 'us') == 'ns':
            p50 /= 1000.0
            p99 /= 1000.0
        if p50 > 0:
            ax1.axvline(x=p50, color=color, linestyle=ls, alpha=0.7, linewidth=1.5, label=f'{name} P50')
        if p99 > 0:
            ax1.axvline(x=p99, color=color, linestyle=ls, alpha=0.5, linewidth=1.5)
    
    # 2. CDF Comparison
    ax2 = fig.add_subplot(2, 2, 2)
    
    # Calculate CDFs
    dex_cdf = np.cumsum(dex_cnt) / dex_total * 100
    chime_cdf = np.cumsum(chime_cnt) / chime_total * 100
    
    ax2.plot(dex_lat_us[dex_mask], dex_cdf[dex_mask], 
             label=f'DEX ({dex_total:,} ops)', color='#2ecc71', linewidth=2)
    ax2.plot(chime_lat_us[chime_mask], chime_cdf[chime_mask], 
             label=f'CHIME ({chime_total:,} ops)', color='#3498db', linewidth=2)
    
    # Add horizontal lines for common percentiles
    for p in [50, 90, 95, 99, 99.9]:
        ax2.axhline(y=p, color='gray', linestyle='--', alpha=0.3, linewidth=0.5)
    
    ax2.set_xlabel('Latency (μs)')
    ax2.set_ylabel('Cumulative Percentage (%)')
    ax2.set_title('CDF Comparison')
    ax2.legend(loc='lower right')
    ax2.set_xlim(0, max_lat)
    ax2.set_ylim(0, 100)
    ax2.grid(True, alpha=0.3)
    
    # 3. Log-scale histogram for tail latency
    ax3 = fig.add_subplot(2, 2, 3)
    
    ax3.bar(dex_lat_us, dex_cnt, width=0.5, alpha=0.6, label=f'DEX', color='#2ecc71', edgecolor='none')
    ax3.bar(chime_lat_us, chime_cnt, width=0.5, alpha=0.6, label=f'CHIME', color='#3498db', edgecolor='none')
    
    ax3.set_xlabel('Latency (μs)')
    ax3.set_ylabel('Count (log scale)')
    ax3.set_title('Tail Latency Distribution (Log Scale)')
    ax3.set_yscale('log')
    ax3.legend(loc='upper right')
    ax3.set_xlim(0, min(max(np.max(dex_lat_us), np.max(chime_lat_us)), 10000))
    
    # 4. Statistics comparison bar chart
    ax4 = fig.add_subplot(2, 2, 4)
    
    metrics = ['avg', 'p50', 'p90', 'p95', 'p99', 'p999']
    metric_labels = ['Average', 'P50', 'P90', 'P95', 'P99', 'P99.9']
    
    # Get values and convert ns to us if needed
    dex_values = []
    chime_values = []
    for m in metrics:
        dex_v = dex_stats.get(m, 0)
        chime_v = chime_stats.get(m, 0)
        if dex_stats.get('unit', 'us') == 'ns':
            dex_v /= 1000.0
        if chime_stats.get('unit', 'us') == 'ns':
            chime_v /= 1000.0
        dex_values.append(dex_v)
        chime_values.append(chime_v)
    
    x = np.arange(len(metrics))
    width = 0.35
    
    bars1 = ax4.bar(x - width/2, dex_values, width, label=f'DEX ({dex_total:,} ops)', color='#2ecc71', alpha=0.8)
    bars2 = ax4.bar(x + width/2, chime_values, width, label=f'CHIME ({chime_total:,} ops)', color='#3498db', alpha=0.8)
    
    ax4.set_ylabel('Latency (μs)')
    ax4.set_title('Latency Statistics Comparison')
    ax4.set_xticks(x)
    ax4.set_xticklabels(metric_labels)
    ax4.legend()
    
    # Add value labels on bars
    def autolabel(bars):
        for bar in bars:
            height = bar.get_height()
            if height > 0:
                ax4.annotate(f'{height:.1f}',
                            xy=(bar.get_x() + bar.get_width() / 2, height),
                            xytext=(0, 3),
                            textcoords="offset points",
                            ha='center', va='bottom', fontsize=9)
    
    autolabel(bars1)
    autolabel(bars2)
    
    plt.tight_layout(rect=[0, 0, 1, 0.95])  # Leave room for suptitle
    
    # Save figure
    plt.savefig(output_path, dpi=300, bbox_inches='tight')
    print(f"Plot saved to: {output_path}")
    
    # Also save individual plots
    base_name = output_path.rsplit('.', 1)[0]
    
    # Save histogram only with operation counts
    fig_hist, ax_hist = plt.subplots(figsize=(12, 7))
    ax_hist.bar(dex_lat_us[dex_mask], dex_cnt[dex_mask] / dex_total * 100, 
                width=0.5, alpha=0.7, label=f'DEX ({dex_total:,} ops)', color='#2ecc71', edgecolor='none')
    ax_hist.bar(chime_lat_us[chime_mask], chime_cnt[chime_mask] / chime_total * 100, 
                width=0.5, alpha=0.7, label=f'CHIME ({chime_total:,} ops)', color='#3498db', edgecolor='none')
    ax_hist.set_xlabel('Latency (μs)', fontsize=14)
    ax_hist.set_ylabel('Percentage of Operations (%)', fontsize=14)
    ax_hist.set_title(f'Latency Distribution: DEX vs CHIME (100% Reads)\n'
                      f'DEX: {dex_total:,} ops | CHIME: {chime_total:,} ops', fontsize=16)
    ax_hist.legend(loc='upper right', fontsize=12)
    ax_hist.set_xlim(0, max_lat)
    
    # Add text box with summary stats
    dex_avg = dex_stats.get('avg', 0)
    chime_avg = chime_stats.get('avg', 0)
    if dex_stats.get('unit', 'us') == 'ns':
        dex_avg /= 1000.0
    if chime_stats.get('unit', 'us') == 'ns':
        chime_avg /= 1000.0
    
    textstr = f'DEX avg: {dex_avg:.2f}μs\nCHIME avg: {chime_avg:.2f}μs'
    if chime_avg > 0:
        speedup = (chime_avg - dex_avg) / chime_avg * 100
        textstr += f'\nDEX is {speedup:.1f}% faster'
    props = dict(boxstyle='round', facecolor='wheat', alpha=0.5)
    ax_hist.text(0.98, 0.95, textstr, transform=ax_hist.transAxes, fontsize=12,
                 verticalalignment='top', horizontalalignment='right', bbox=props)
    
    plt.tight_layout()
    plt.savefig(f"{base_name}_histogram.png", dpi=300, bbox_inches='tight')
    
    # Save CDF only with operation counts
    fig_cdf, ax_cdf = plt.subplots(figsize=(12, 7))
    ax_cdf.plot(dex_lat_us[dex_mask], dex_cdf[dex_mask], 
                label=f'DEX ({dex_total:,} ops)', color='#2ecc71', linewidth=2.5)
    ax_cdf.plot(chime_lat_us[chime_mask], chime_cdf[chime_mask], 
                label=f'CHIME ({chime_total:,} ops)', color='#3498db', linewidth=2.5)
    for p in [50, 90, 95, 99, 99.9]:
        ax_cdf.axhline(y=p, color='gray', linestyle='--', alpha=0.3, linewidth=0.5)
    ax_cdf.set_xlabel('Latency (μs)', fontsize=14)
    ax_cdf.set_ylabel('Cumulative Percentage (%)', fontsize=14)
    ax_cdf.set_title(f'CDF Comparison: DEX vs CHIME\n'
                     f'DEX: {dex_total:,} ops | CHIME: {chime_total:,} ops', fontsize=16)
    ax_cdf.legend(loc='lower right', fontsize=12)
    ax_cdf.set_xlim(0, max_lat)
    ax_cdf.set_ylim(0, 100)
    ax_cdf.grid(True, alpha=0.3)
    plt.tight_layout()
    plt.savefig(f"{base_name}_cdf.png", dpi=300, bbox_inches='tight')
    
    plt.close('all')
    
    return fig


def print_comparison_stats(dex_stats, chime_stats):
    """Print comparison statistics."""
    print("\n" + "="*60)
    print("LATENCY COMPARISON: DEX vs CHIME (100% Reads)")
    print("="*60)
    
    print(f"\n{'Metric':<15} {'DEX':>15} {'CHIME':>15} {'Diff':>15}")
    print("-"*60)
    
    metrics = [
        ('Total Ops', 'total_ops', ''),
         ('Average', 'avg', 'μs'),
        ('P50', 'p50', 'μs'),
        ('P90', 'p90', 'μs'),
        ('P95', 'p95', 'μs'),
        ('P99', 'p99', 'μs'),
        ('P99.9', 'p999', 'μs'),
    ]
    
    for label, key, unit in metrics:
        dex_val = dex_stats.get(key, 0)
        chime_val = chime_stats.get(key, 0)
        
        if key == 'total_ops':
            print(f"{label:<15} {dex_val:>15,} {chime_val:>15,}")
        else:
            diff = dex_val - chime_val
            diff_pct = (diff / chime_val * 100) if chime_val > 0 else 0
            diff_str = f"{diff:+.1f}{unit} ({diff_pct:+.1f}%)"
            print(f"{label:<15} {dex_val:>12.1f} {unit:<2} {chime_val:>12.1f} {unit:<2} {diff_str}")
    
    print("="*60)
    
    # Summary
    if dex_stats.get('avg', 0) < chime_stats.get('avg', 0):
        winner = "DEX"
        improvement = (1 - dex_stats['avg'] / chime_stats['avg']) * 100
    else:
        winner = "CHIME"
        improvement = (1 - chime_stats.get('avg', 1) / dex_stats.get('avg', 1)) * 100
    
    print(f"\nSummary: {winner} has {improvement:.1f}% lower average latency")
    print("="*60 + "\n")


def main():
    parser = argparse.ArgumentParser(
        description='Compare DEX and CHIME latency histograms'
    )
    parser.add_argument('--dex', '-d', default='dex_latency.dat',
                        help='DEX latency data file (default: dex_latency.dat)')
    parser.add_argument('--chime', '-c', default='chime_latency.dat',
                        help='CHIME latency data file (default: chime_latency.dat)')
    parser.add_argument('--output', '-o', default='latency_comparison.png',
                        help='Output plot file (default: latency_comparison.png)')
    
    args = parser.parse_args()
    
    # Check if files exist
    if not os.path.exists(args.dex):
        print(f"Error: DEX latency file not found: {args.dex}")
        print("Run the DEX latency benchmark first using dex_node0.sh")
        sys.exit(1)
    
    if not os.path.exists(args.chime):
        print(f"Error: CHIME latency file not found: {args.chime}")
        print("Run the CHIME latency benchmark first using chime_node0.sh")
        sys.exit(1)
    
    print("Loading DEX latency data...")
    dex_data = parse_latency_file(args.dex)
    
    print("Loading CHIME latency data...")
    chime_data = parse_latency_file(args.chime)
    
    # Print statistics comparison
    print_comparison_stats(dex_data[2], chime_data[2])
    
    # Generate plots
    print("Generating comparison plots...")
    plot_histogram_comparison(dex_data, chime_data, args.output)
    
    print("\nDone! Generated files:")
    print(f"  - {args.output} (combined plots)")
    base_name = args.output.rsplit('.', 1)[0]
    print(f"  - {base_name}_histogram.png (histogram only)")
    print(f"  - {base_name}_cdf.png (CDF only)")


if __name__ == '__main__':
    main()
