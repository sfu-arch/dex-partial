#!/usr/bin/env python3
"""
QW1 Zipfian Skew Sweep — Automated DEX vs CHIME Comparison Plots

Parses stdout log files directly from results/ directory.
Generates 6 publication-quality figures:
  1. Throughput comparison (bar chart)
  2. Read latency P50 comparison
  3. Read latency P99 comparison
  4. Range latency P50 comparison
  5. Range latency P99 comparison
  6. Latency CDF overlay (zipf_0.99 only)

Usage:
  python plot_qw1_comparison.py [--results-dir ./results] [--output-dir ./plots]
"""

import os
import re
import sys
import argparse
import numpy as np

try:
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    import matplotlib.ticker as ticker
except ImportError:
    print("ERROR: matplotlib not installed. Run: pip install matplotlib")
    sys.exit(1)

# ============================================================================
# Log Parsing
# ============================================================================

SKEW_LABELS = ["uniform", "zipf_0.6", "zipf_0.8", "zipf_0.9", "zipf_0.99"]
SKEW_DISPLAY = ["Uniform", "θ=0.6", "θ=0.8", "θ=0.9", "θ=0.99"]

def parse_dex_log(filepath):
    """Parse a DEX stdout log file and extract throughput + latency percentiles."""
    result = {}
    with open(filepath, 'r', errors='replace') as f:
        content = f.read()
    
    # Throughput: "Final cluster throughput: X.XXX Mops/s"
    m = re.search(r'Final cluster throughput:\s+([\d.]+)\s+Mops/s', content)
    if m:
        result['throughput_mops'] = float(m.group(1))
        result['throughput_ops'] = float(m.group(1)) * 1e6
    
    # Read latency percentiles
    read_section = re.search(
        r'DEX Read LATENCY STATISTICS.*?'
        r'P50 latency:\s+(\d+)\s+ns.*?'
        r'P90 latency:\s+(\d+)\s+ns.*?'
        r'P95 latency:\s+(\d+)\s+ns.*?'
        r'P99 latency:\s+(\d+)\s+ns.*?'
        r'P99\.9 latency:\s+(\d+)\s+ns',
        content, re.DOTALL
    )
    if read_section:
        result['read_p50_us'] = int(read_section.group(1)) / 1000.0
        result['read_p90_us'] = int(read_section.group(2)) / 1000.0
        result['read_p95_us'] = int(read_section.group(3)) / 1000.0
        result['read_p99_us'] = int(read_section.group(4)) / 1000.0
        result['read_p999_us'] = int(read_section.group(5)) / 1000.0
    
    # Range latency percentiles
    range_section = re.search(
        r'DEX Range LATENCY STATISTICS.*?'
        r'P50 latency:\s+(\d+)\s+ns.*?'
        r'P90 latency:\s+(\d+)\s+ns.*?'
        r'P95 latency:\s+(\d+)\s+ns.*?'
        r'P99 latency:\s+(\d+)\s+ns.*?'
        r'P99\.9 latency:\s+(\d+)\s+ns',
        content, re.DOTALL
    )
    if range_section:
        result['range_p50_us'] = int(range_section.group(1)) / 1000.0
        result['range_p90_us'] = int(range_section.group(2)) / 1000.0
        result['range_p95_us'] = int(range_section.group(3)) / 1000.0
        result['range_p99_us'] = int(range_section.group(4)) / 1000.0
        result['range_p999_us'] = int(range_section.group(5)) / 1000.0
    
    # Total reads and ranges
    m = re.search(r'Total reads:\s+(\d+),\s+Total ranges:\s+(\d+)', content)
    if m:
        result['total_reads'] = int(m.group(1))
        result['total_ranges'] = int(m.group(2))
    
    return result


def parse_chime_log(filepath):
    """Parse a CHIME stdout log file and extract throughput + latency percentiles."""
    result = {}
    with open(filepath, 'r', errors='replace') as f:
        content = f.read()
    
    # Throughput: "Throughput:     XXXX.XX ops/sec"
    m = re.search(r'Throughput:\s+([\d.]+)\s+ops/sec', content)
    if m:
        result['throughput_ops'] = float(m.group(1))
        result['throughput_mops'] = float(m.group(1)) / 1e6
    
    # Read latency percentiles (in microseconds already)
    read_pcts = re.findall(
        r'Read Latency Percentiles:.*?'
        r'P50\.0:\s+([\d.]+)\s+us.*?'
        r'P90\.0:\s+([\d.]+)\s+us.*?'
        r'P95\.0:\s+([\d.]+)\s+us.*?'
        r'P99\.0:\s+([\d.]+)\s+us.*?'
        r'P99\.9:\s+([\d.]+)\s+us',
        content, re.DOTALL
    )
    if read_pcts:
        p = read_pcts[0]
        result['read_p50_us'] = float(p[0])
        result['read_p90_us'] = float(p[1])
        result['read_p95_us'] = float(p[2])
        result['read_p99_us'] = float(p[3])
        result['read_p999_us'] = float(p[4])
    
    # Range latency percentiles
    range_pcts = re.findall(
        r'Range Latency Percentiles:.*?'
        r'P50\.0:\s+([\d.]+)\s+us.*?'
        r'P90\.0:\s+([\d.]+)\s+us.*?'
        r'P95\.0:\s+([\d.]+)\s+us.*?'
        r'P99\.0:\s+([\d.]+)\s+us.*?'
        r'P99\.9:\s+([\d.]+)\s+us',
        content, re.DOTALL
    )
    if range_pcts:
        p = range_pcts[0]
        result['range_p50_us'] = float(p[0])
        result['range_p90_us'] = float(p[1])
        result['range_p95_us'] = float(p[2])
        result['range_p99_us'] = float(p[3])
        result['range_p999_us'] = float(p[4])
    
    # Total ops
    m = re.search(r'Total reads:\s+(\d+)', content)
    if m:
        result['total_reads'] = int(m.group(1))
    m = re.search(r'Total ranges:\s+(\d+)', content)
    if m:
        result['total_ranges'] = int(m.group(1))
    
    # CHIME diagnostic stats
    m = re.search(r'Cache hit rate:\s+([\d.]+)', content)
    if m:
        result['cache_hit_rate'] = float(m.group(1))
    m = re.search(r'Read delegation rate:\s+([\d.]+)', content)
    if m:
        result['delegation_rate'] = float(m.group(1))
    m = re.search(r'Speculative read rate:\s+([\d.]+)', content)
    if m:
        result['speculative_rate'] = float(m.group(1))
    
    # Count "Failed status" errors
    result['rdma_errors'] = len(re.findall(r'Failed status', content))
    
    return result


def parse_latency_histogram(filepath):
    """Parse a latency histogram .dat file → dict of {latency_us: count}."""
    hist = {}
    if not os.path.exists(filepath):
        return hist
    with open(filepath, 'r') as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            parts = line.split('\t')
            if len(parts) == 2:
                latency_ns = int(parts[0])
                count = int(parts[1])
                latency_us = latency_ns / 1000.0
                hist[latency_us] = count
    return hist


def load_all_results(results_dir):
    """Load all DEX and CHIME results from the results directory."""
    dex_results = {}
    chime_results = {}
    
    for label in SKEW_LABELS:
        # DEX
        dex_path = os.path.join(results_dir, 'dex', f'dex_{label}_stdout.log')
        if os.path.exists(dex_path):
            dex_results[label] = parse_dex_log(dex_path)
            print(f"  [DEX]  {label}: {dex_results[label].get('throughput_mops', 'N/A'):.3f} Mops/s")
        else:
            print(f"  [DEX]  {label}: NOT FOUND ({dex_path})")
        
        # CHIME
        chime_path = os.path.join(results_dir, 'chime', f'chime_{label}_stdout.log')
        if os.path.exists(chime_path):
            chime_results[label] = parse_chime_log(chime_path)
            print(f"  [CHIME] {label}: {chime_results[label].get('throughput_mops', 'N/A'):.3f} Mops/s")
        else:
            print(f"  [CHIME] {label}: NOT FOUND ({chime_path})")
    
    return dex_results, chime_results


# ============================================================================
# Plotting
# ============================================================================

# Color scheme
DEX_COLOR = '#2196F3'      # Blue
CHIME_COLOR = '#FF5722'    # Orange-red
DEX_LIGHT = '#90CAF9'
CHIME_LIGHT = '#FFAB91'

def setup_style():
    """Set up matplotlib style for publication-quality plots."""
    plt.rcParams.update({
        'font.size': 12,
        'axes.titlesize': 14,
        'axes.labelsize': 13,
        'xtick.labelsize': 11,
        'ytick.labelsize': 11,
        'legend.fontsize': 11,
        'figure.figsize': (8, 5),
        'axes.grid': True,
        'grid.alpha': 0.3,
        'axes.spines.top': False,
        'axes.spines.right': False,
    })


def plot_throughput(dex, chime, output_dir):
    """Fig 1: Throughput comparison (grouped bar chart)."""
    fig, ax = plt.subplots(figsize=(10, 6))
    
    labels = []
    dex_vals = []
    chime_vals = []
    
    for i, label in enumerate(SKEW_LABELS):
        if label in dex and label in chime:
            labels.append(SKEW_DISPLAY[i])
            dex_vals.append(dex[label].get('throughput_mops', 0))
            chime_vals.append(chime[label].get('throughput_mops', 0))
    
    x = np.arange(len(labels))
    width = 0.35
    
    bars1 = ax.bar(x - width/2, dex_vals, width, label='DEX', color=DEX_COLOR, edgecolor='white')
    bars2 = ax.bar(x + width/2, chime_vals, width, label='CHIME', color=CHIME_COLOR, edgecolor='white')
    
    # Add value labels on bars
    for bar, val in zip(bars1, dex_vals):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.15,
                f'{val:.1f}M', ha='center', va='bottom', fontsize=9, fontweight='bold')
    for bar, val in zip(bars2, chime_vals):
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.15,
                f'{val*1000:.0f}K', ha='center', va='bottom', fontsize=9, fontweight='bold')
    
    # Add ratio annotations
    for i, (d, c) in enumerate(zip(dex_vals, chime_vals)):
        if c > 0:
            ratio = d / c
            ax.annotate(f'{ratio:.0f}×', xy=(x[i], max(d, c) + 0.8),
                       ha='center', fontsize=10, color='#333', fontstyle='italic')
    
    ax.set_xlabel('Zipfian Skew')
    ax.set_ylabel('Throughput (Mops/s)')
    ax.set_title('QW1: DEX vs CHIME Throughput (70% Read + 30% Range Scan, 30 Threads)')
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.legend(loc='upper left')
    ax.set_ylim(0, max(dex_vals) * 1.25)
    
    plt.tight_layout()
    path = os.path.join(output_dir, 'qw1_throughput_comparison.png')
    fig.savefig(path, dpi=200)
    plt.close(fig)
    print(f"  Saved: {path}")


def plot_latency_comparison(dex, chime, output_dir, op_type, percentile, ylabel_suffix=''):
    """Generic latency comparison bar chart."""
    key = f'{op_type}_p{percentile}_us'
    fig_name = f'qw1_{op_type}_p{percentile}_comparison.png'
    pct_label = f'P{percentile}' if percentile != '999' else 'P99.9'
    op_label = 'Read' if op_type == 'read' else 'Range Scan'
    
    fig, ax = plt.subplots(figsize=(10, 6))
    
    labels = []
    dex_vals = []
    chime_vals = []
    
    for i, label in enumerate(SKEW_LABELS):
        if label in dex and label in chime:
            d_val = dex[label].get(key, 0)
            c_val = chime[label].get(key, 0)
            if d_val > 0 or c_val > 0:
                labels.append(SKEW_DISPLAY[i])
                dex_vals.append(d_val)
                chime_vals.append(c_val)
    
    if not labels:
        print(f"  No data for {fig_name}, skipping.")
        return
    
    x = np.arange(len(labels))
    width = 0.35
    
    bars1 = ax.bar(x - width/2, dex_vals, width, label='DEX', color=DEX_COLOR, edgecolor='white')
    bars2 = ax.bar(x + width/2, chime_vals, width, label='CHIME', color=CHIME_COLOR, edgecolor='white')
    
    # Value labels
    for bar, val in zip(bars1, dex_vals):
        label_text = f'{val:.1f}' if val < 100 else f'{val:.0f}'
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height(),
                label_text, ha='center', va='bottom', fontsize=8)
    for bar, val in zip(bars2, chime_vals):
        label_text = f'{val:.1f}' if val < 100 else f'{val:.0f}'
        ax.text(bar.get_x() + bar.get_width()/2, bar.get_height(),
                label_text, ha='center', va='bottom', fontsize=8)
    
    ax.set_xlabel('Zipfian Skew')
    ax.set_ylabel(f'{pct_label} Latency (μs)')
    ax.set_title(f'QW1: {op_label} {pct_label} Latency — DEX vs CHIME (30 Threads)')
    ax.set_xticks(x)
    ax.set_xticklabels(labels)
    ax.legend(loc='upper right')
    
    # Use log scale if ratio > 10x
    max_val = max(max(chime_vals, default=1), max(dex_vals, default=1))
    min_val = min(min(dex_vals, default=1), min(chime_vals, default=1))
    if max_val / max(min_val, 0.01) > 10:
        ax.set_yscale('log')
        ax.yaxis.set_major_formatter(ticker.ScalarFormatter())
        ax.yaxis.get_major_formatter().set_scientific(False)
    
    plt.tight_layout()
    path = os.path.join(output_dir, fig_name)
    fig.savefig(path, dpi=200)
    plt.close(fig)
    print(f"  Saved: {path}")


def plot_latency_breakdown(dex, chime, output_dir):
    """Fig 6: Multi-percentile latency breakdown for reads and ranges at zipf_0.99."""
    config = 'zipf_0.99'
    if config not in dex or config not in chime:
        print("  Skipping latency breakdown (no zipf_0.99 data).")
        return
    
    fig, axes = plt.subplots(1, 2, figsize=(14, 6))
    
    percentiles = ['P50', 'P90', 'P95', 'P99', 'P99.9']
    pct_keys = ['p50', 'p90', 'p95', 'p99', 'p999']
    
    for ax_idx, (op_type, op_label) in enumerate([('read', 'Read'), ('range', 'Range Scan')]):
        ax = axes[ax_idx]
        dex_vals = [dex[config].get(f'{op_type}_{k}_us', 0) for k in pct_keys]
        chime_vals = [chime[config].get(f'{op_type}_{k}_us', 0) for k in pct_keys]
        
        x = np.arange(len(percentiles))
        width = 0.35
        
        ax.bar(x - width/2, dex_vals, width, label='DEX', color=DEX_COLOR, edgecolor='white')
        ax.bar(x + width/2, chime_vals, width, label='CHIME', color=CHIME_COLOR, edgecolor='white')
        
        for i, (d, c) in enumerate(zip(dex_vals, chime_vals)):
            if d > 0:
                ratio = c / d
                y_pos = max(d, c)
                ax.annotate(f'{ratio:.0f}×', xy=(x[i], y_pos),
                           ha='center', va='bottom', fontsize=9, color='#666')
        
        ax.set_xlabel('Percentile')
        ax.set_ylabel('Latency (μs)')
        ax.set_title(f'{op_label} Latency @ θ=0.99')
        ax.set_xticks(x)
        ax.set_xticklabels(percentiles)
        ax.legend(loc='upper left')
        ax.set_yscale('log')
        ax.yaxis.set_major_formatter(ticker.ScalarFormatter())
        ax.yaxis.get_major_formatter().set_scientific(False)
    
    fig.suptitle('QW1: Latency Distribution at Zipf θ=0.99 (30 Threads)', fontsize=14, y=1.02)
    plt.tight_layout()
    path = os.path.join(output_dir, 'qw1_latency_breakdown_zipf099.png')
    fig.savefig(path, dpi=200, bbox_inches='tight')
    plt.close(fig)
    print(f"  Saved: {path}")


def plot_latency_cdf(results_dir, output_dir):
    """Fig 7: Read and Range latency CDF overlay at zipf_0.99 (from histogram .dat files)."""
    dex_read_path = os.path.join(results_dir, 'dex', 'dex_zipf_0.99_dex_read_latency.dat')
    dex_range_path = os.path.join(results_dir, 'dex', 'dex_zipf_0.99_dex_range_latency.dat')
    chime_read_path = os.path.join(results_dir, 'chime', 'chime_zipf_0.99_chime_read_latency.dat')
    chime_range_path = os.path.join(results_dir, 'chime', 'chime_zipf_0.99_chime_range_latency.dat')
    
    datasets = [
        ('DEX Read', dex_read_path, DEX_COLOR, '-'),
        ('CHIME Read', chime_read_path, CHIME_COLOR, '-'),
        ('DEX Range', dex_range_path, DEX_COLOR, '--'),
        ('CHIME Range', chime_range_path, CHIME_COLOR, '--'),
    ]
    
    fig, ax = plt.subplots(figsize=(10, 6))
    
    any_data = False
    for label, path, color, ls in datasets:
        hist = parse_latency_histogram(path)
        if not hist:
            print(f"  WARNING: No histogram data at {path}")
            continue
        
        any_data = True
        latencies = sorted(hist.keys())
        counts = [hist[l] for l in latencies]
        total = sum(counts)
        cdf = np.cumsum(counts) / total
        
        ax.plot(latencies, cdf, label=label, color=color, linestyle=ls, linewidth=2)
    
    if not any_data:
        print("  Skipping CDF plot (no histogram data).")
        plt.close(fig)
        return
    
    ax.set_xlabel('Latency (μs)')
    ax.set_ylabel('CDF')
    ax.set_title('QW1: Latency CDF — DEX vs CHIME @ Zipf θ=0.99 (30 Threads)')
    ax.set_xscale('log')
    ax.set_xlim(0.1, 10000)
    ax.set_ylim(0, 1.05)
    ax.axhline(y=0.50, color='gray', linestyle=':', alpha=0.5, label='_nolegend_')
    ax.axhline(y=0.99, color='gray', linestyle=':', alpha=0.5, label='_nolegend_')
    ax.legend(loc='lower right')
    
    plt.tight_layout()
    path = os.path.join(output_dir, 'qw1_latency_cdf_zipf099.png')
    fig.savefig(path, dpi=200)
    plt.close(fig)
    print(f"  Saved: {path}")


def plot_chime_diagnostics(chime, output_dir):
    """Fig 8: CHIME internal stats across skew levels."""
    fig, axes = plt.subplots(1, 3, figsize=(15, 5))
    
    labels = []
    delegation = []
    speculative = []
    rdma_errors = []
    
    for i, label in enumerate(SKEW_LABELS):
        if label in chime:
            labels.append(SKEW_DISPLAY[i])
            delegation.append(chime[label].get('delegation_rate', 0) * 100)
            speculative.append(chime[label].get('speculative_rate', 0) * 100)
            rdma_errors.append(chime[label].get('rdma_errors', 0))
    
    x = np.arange(len(labels))
    
    # Delegation rate
    axes[0].bar(x, delegation, color=CHIME_COLOR, edgecolor='white')
    axes[0].set_ylabel('Rate (%)')
    axes[0].set_title('RDWC Delegation Rate')
    axes[0].set_xticks(x)
    axes[0].set_xticklabels(labels, rotation=30)
    for i, v in enumerate(delegation):
        axes[0].text(i, v + 0.01, f'{v:.2f}%', ha='center', fontsize=9)
    
    # Speculative read rate
    axes[1].bar(x, speculative, color='#FF9800', edgecolor='white')
    axes[1].set_ylabel('Rate (%)')
    axes[1].set_title('Speculative Read Rate')
    axes[1].set_xticks(x)
    axes[1].set_xticklabels(labels, rotation=30)
    for i, v in enumerate(speculative):
        axes[1].text(i, v + 0.5, f'{v:.1f}%', ha='center', fontsize=9)
    
    # RDMA errors
    axes[2].bar(x, rdma_errors, color='#F44336', edgecolor='white')
    axes[2].set_ylabel('Count')
    axes[2].set_title('RDMA "Failed Status" Errors')
    axes[2].set_xticks(x)
    axes[2].set_xticklabels(labels, rotation=30)
    for i, v in enumerate(rdma_errors):
        axes[2].text(i, v + 0.05, str(int(v)), ha='center', fontsize=9)
    
    fig.suptitle('QW1: CHIME Internal Diagnostics Across Skew Levels', fontsize=14, y=1.02)
    plt.tight_layout()
    path = os.path.join(output_dir, 'qw1_chime_diagnostics.png')
    fig.savefig(path, dpi=200, bbox_inches='tight')
    plt.close(fig)
    print(f"  Saved: {path}")


def print_summary_table(dex, chime):
    """Print a formatted comparison table to stdout."""
    print("\n" + "=" * 100)
    print("QW1 RESULTS SUMMARY — DEX vs CHIME (30 Threads, 30M Ops, 70% Read + 30% Range)")
    print("=" * 100)
    
    header = f"{'Config':<12} | {'DEX Tput':>10} | {'CHIME Tput':>12} | {'Ratio':>6} | " \
             f"{'DEX Rd P50':>10} | {'CHIME Rd P50':>12} | {'DEX Rng P50':>11} | {'CHIME Rng P50':>13}"
    print(header)
    print("-" * len(header))
    
    for i, label in enumerate(SKEW_LABELS):
        if label in dex and label in chime:
            d = dex[label]
            c = chime[label]
            d_tput = d.get('throughput_mops', 0)
            c_tput = c.get('throughput_mops', 0)
            ratio = d_tput / c_tput if c_tput > 0 else float('inf')
            
            print(f"{SKEW_DISPLAY[i]:<12} | {d_tput:>8.3f} M | {c_tput*1000:>9.1f} K | {ratio:>5.0f}× | "
                  f"{d.get('read_p50_us', 0):>8.1f} μs | {c.get('read_p50_us', 0):>10.1f} μs | "
                  f"{d.get('range_p50_us', 0):>9.1f} μs | {c.get('range_p50_us', 0):>11.1f} μs")
    
    print("=" * 100)


# ============================================================================
# Main
# ============================================================================

def main():
    parser = argparse.ArgumentParser(description='QW1 DEX vs CHIME Comparison Plots')
    parser.add_argument('--results-dir', default=None,
                        help='Path to results/ directory (default: ./results or script_dir/results)')
    parser.add_argument('--output-dir', default=None,
                        help='Output directory for plots (default: ./plots)')
    args = parser.parse_args()
    
    # Resolve paths
    script_dir = os.path.dirname(os.path.abspath(__file__))
    results_dir = args.results_dir or os.path.join(script_dir, 'results')
    output_dir = args.output_dir or os.path.join(script_dir, 'plots')
    
    if not os.path.isdir(results_dir):
        print(f"ERROR: Results directory not found: {results_dir}")
        sys.exit(1)
    
    os.makedirs(output_dir, exist_ok=True)
    
    print(f"Results dir: {results_dir}")
    print(f"Output dir:  {output_dir}")
    print()
    
    # Load results
    print("Loading results...")
    dex, chime = load_all_results(results_dir)
    
    if not dex or not chime:
        print("ERROR: No results found!")
        sys.exit(1)
    
    # Print summary
    print_summary_table(dex, chime)
    
    # Generate plots
    setup_style()
    print("\nGenerating plots...")
    
    # 1. Throughput comparison
    plot_throughput(dex, chime, output_dir)
    
    # 2-5. Latency comparisons
    plot_latency_comparison(dex, chime, output_dir, 'read', '50')
    plot_latency_comparison(dex, chime, output_dir, 'read', '99')
    plot_latency_comparison(dex, chime, output_dir, 'range', '50')
    plot_latency_comparison(dex, chime, output_dir, 'range', '99')
    
    # 6. Latency breakdown at zipf_0.99
    plot_latency_breakdown(dex, chime, output_dir)
    
    # 7. CDF overlay
    plot_latency_cdf(results_dir, output_dir)
    
    # 8. CHIME diagnostics
    plot_chime_diagnostics(chime, output_dir)
    
    print(f"\nDone! {len(os.listdir(output_dir))} plots saved to {output_dir}")


if __name__ == '__main__':
    main()
