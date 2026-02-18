#!/usr/bin/env python3
"""
QW2: Original CHIME vs Coherent Trie CHIME — Comparison Plots

Parses stdout log files from:
    results/chime_original/original_{label}_stdout.log
    results/chime_coherent/coherent_{label}_stdout.log

Generates comparison figures:
  1. Throughput comparison (bar chart)
  2. Read latency P50 comparison
  3. Read latency P99 comparison
  4. Cache hit rate comparison (if available)
  5. RDMA operations reduction
  6. Latency CDF overlay (zipf_0.99)

Usage:
  python plot_qw2_comparison.py [--results-dir ./results] [--output-dir ./plots]
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
# Constants
# ============================================================================

SKEW_LABELS = ["uniform", "zipf_0.6", "zipf_0.8", "zipf_0.9", "zipf_0.99"]
SKEW_DISPLAY = ["Uniform", "θ=0.6", "θ=0.8", "θ=0.9", "θ=0.99"]

COLORS = {
    "original": "#1f77b4",  # Blue
    "coherent": "#2ca02c",  # Green
}

# ============================================================================
# Log Parsing
# ============================================================================

def parse_chime_log(filepath):
    """Parse a CHIME stdout log file and extract metrics."""
    result = {}
    if not os.path.exists(filepath):
        print(f"  WARNING: {filepath} not found")
        return result
    
    with open(filepath, 'r', errors='replace') as f:
        content = f.read()
    
    # Throughput: "Throughput:     XXXX.XX ops/sec"
    m = re.search(r'Throughput:\s+([\d.]+)\s+ops/sec', content)
    if m:
        result['throughput_ops'] = float(m.group(1))
        result['throughput_mops'] = float(m.group(1)) / 1e6
    
    # Read latency percentiles
    read_pcts = re.findall(
        r'(?:Read|read)\s+Latency\s+Percentiles:.*?'
        r'P50[\.\d]*:\s+([\d.]+)\s+us.*?'
        r'P90[\.\d]*:\s+([\d.]+)\s+us.*?'
        r'P95[\.\d]*:\s+([\d.]+)\s+us.*?'
        r'P99[\.\d]*:\s+([\d.]+)\s+us.*?'
        r'P99\.9[\.\d]*:\s+([\d.]+)\s+us',
        content, re.DOTALL | re.IGNORECASE
    )
    if read_pcts:
        p = read_pcts[0]
        result['read_p50_us'] = float(p[0])
        result['read_p90_us'] = float(p[1])
        result['read_p95_us'] = float(p[2])
        result['read_p99_us'] = float(p[3])
        result['read_p999_us'] = float(p[4])
    
    # Alternative format: "P50 latency: XXX ns"
    if 'read_p50_us' not in result:
        read_section = re.search(
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
    
    # Cache statistics (Coherent Trie specific)
    # Trie cache hits
    m = re.search(r'Trie cache hits:\s+(\d+)', content)
    if m:
        result['trie_cache_hits'] = int(m.group(1))
    
    m = re.search(r'Trie cache misses:\s+(\d+)', content)
    if m:
        result['trie_cache_misses'] = int(m.group(1))
    
    # Bitmap cache hits
    m = re.search(r'Bitmap cache hits:\s+(\d+)', content)
    if m:
        result['bitmap_cache_hits'] = int(m.group(1))
    
    m = re.search(r'Bitmap cache misses:\s+(\d+)', content)
    if m:
        result['bitmap_cache_misses'] = int(m.group(1))
    
    # RDMA operations
    m = re.search(r'Total RDMA reads:\s+(\d+)', content)
    if m:
        result['rdma_reads'] = int(m.group(1))
    
    m = re.search(r'Total RDMA writes:\s+(\d+)', content)
    if m:
        result['rdma_writes'] = int(m.group(1))
    
    # Total ops
    m = re.search(r'Total (?:reads|ops):\s+(\d+)', content)
    if m:
        result['total_ops'] = int(m.group(1))
    
    return result


def load_histogram(filepath):
    """Load latency histogram file. Returns (latency_ns[], count[])."""
    lat, cnt = [], []
    if not os.path.exists(filepath):
        return np.array([]), np.array([])
    with open(filepath) as f:
        for line in f:
            parts = line.strip().split()
            if len(parts) >= 2:
                try:
                    lat.append(int(parts[0]))
                    cnt.append(int(parts[1]))
                except ValueError:
                    continue
    return np.array(lat), np.array(cnt)


def hist_to_cdf(lat, cnt):
    """Convert histogram to CDF (x=latency_us, y=fraction)."""
    if len(cnt) == 0:
        return np.array([]), np.array([])
    total = cnt.sum()
    if total == 0:
        return np.array([]), np.array([])
    cdf = np.cumsum(cnt).astype(float) / total
    return lat / 1000.0, cdf  # ns → µs


# ============================================================================
# Plotting Functions
# ============================================================================

def plot_throughput_comparison(orig_data, coh_data, output_dir):
    """Bar chart comparing throughput across skew points."""
    fig, ax = plt.subplots(figsize=(10, 6))
    
    x = np.arange(len(SKEW_LABELS))
    width = 0.35
    
    orig_tput = [orig_data.get(l, {}).get('throughput_mops', 0) for l in SKEW_LABELS]
    coh_tput = [coh_data.get(l, {}).get('throughput_mops', 0) for l in SKEW_LABELS]
    
    bars1 = ax.bar(x - width/2, orig_tput, width, label='Original CHIME', 
                   color=COLORS['original'], alpha=0.85)
    bars2 = ax.bar(x + width/2, coh_tput, width, label='Coherent Trie CHIME', 
                   color=COLORS['coherent'], alpha=0.85)
    
    ax.set_ylabel('Throughput (Mops/s)', fontsize=12)
    ax.set_xlabel('Workload Skew', fontsize=12)
    ax.set_title('QW2: Throughput Comparison — Original vs Coherent Trie CHIME', fontsize=14)
    ax.set_xticks(x)
    ax.set_xticklabels(SKEW_DISPLAY)
    ax.legend(fontsize=10)
    ax.grid(True, alpha=0.3, axis='y')
    
    # Add value labels on bars
    for bar in bars1 + bars2:
        height = bar.get_height()
        if height > 0:
            ax.annotate(f'{height:.2f}',
                        xy=(bar.get_x() + bar.get_width() / 2, height),
                        xytext=(0, 3), textcoords="offset points",
                        ha='center', va='bottom', fontsize=8)
    
    fig.tight_layout()
    outpath = os.path.join(output_dir, "qw2_throughput_comparison.pdf")
    fig.savefig(outpath, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  Saved {outpath}")


def plot_latency_comparison(orig_data, coh_data, output_dir, metric='p50'):
    """Bar chart comparing latency percentile across skew points."""
    fig, ax = plt.subplots(figsize=(10, 6))
    
    x = np.arange(len(SKEW_LABELS))
    width = 0.35
    
    key = f'read_{metric}_us'
    orig_lat = [orig_data.get(l, {}).get(key, 0) for l in SKEW_LABELS]
    coh_lat = [coh_data.get(l, {}).get(key, 0) for l in SKEW_LABELS]
    
    bars1 = ax.bar(x - width/2, orig_lat, width, label='Original CHIME', 
                   color=COLORS['original'], alpha=0.85)
    bars2 = ax.bar(x + width/2, coh_lat, width, label='Coherent Trie CHIME', 
                   color=COLORS['coherent'], alpha=0.85)
    
    metric_display = metric.upper().replace('P', 'P')
    ax.set_ylabel(f'{metric_display} Read Latency (µs)', fontsize=12)
    ax.set_xlabel('Workload Skew', fontsize=12)
    ax.set_title(f'QW2: {metric_display} Read Latency — Original vs Coherent Trie', fontsize=14)
    ax.set_xticks(x)
    ax.set_xticklabels(SKEW_DISPLAY)
    ax.legend(fontsize=10)
    ax.grid(True, alpha=0.3, axis='y')
    
    for bar in bars1 + bars2:
        height = bar.get_height()
        if height > 0:
            ax.annotate(f'{height:.1f}',
                        xy=(bar.get_x() + bar.get_width() / 2, height),
                        xytext=(0, 3), textcoords="offset points",
                        ha='center', va='bottom', fontsize=8)
    
    fig.tight_layout()
    outpath = os.path.join(output_dir, f"qw2_read_latency_{metric}.pdf")
    fig.savefig(outpath, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  Saved {outpath}")


def plot_cache_hit_rates(coh_data, output_dir):
    """Bar chart of cache hit rates for Coherent Trie."""
    fig, ax = plt.subplots(figsize=(10, 6))
    
    x = np.arange(len(SKEW_LABELS))
    width = 0.35
    
    trie_hits = []
    bitmap_hits = []
    
    for label in SKEW_LABELS:
        data = coh_data.get(label, {})
        
        # Trie cache hit rate
        trie_h = data.get('trie_cache_hits', 0)
        trie_m = data.get('trie_cache_misses', 0)
        trie_rate = 100.0 * trie_h / (trie_h + trie_m) if (trie_h + trie_m) > 0 else 0
        trie_hits.append(trie_rate)
        
        # Bitmap cache hit rate
        bm_h = data.get('bitmap_cache_hits', 0)
        bm_m = data.get('bitmap_cache_misses', 0)
        bm_rate = 100.0 * bm_h / (bm_h + bm_m) if (bm_h + bm_m) > 0 else 0
        bitmap_hits.append(bm_rate)
    
    bars1 = ax.bar(x - width/2, trie_hits, width, label='Trie Cache Hit Rate', 
                   color='#17becf', alpha=0.85)
    bars2 = ax.bar(x + width/2, bitmap_hits, width, label='Bitmap Cache Hit Rate', 
                   color='#bcbd22', alpha=0.85)
    
    ax.set_ylabel('Hit Rate (%)', fontsize=12)
    ax.set_xlabel('Workload Skew', fontsize=12)
    ax.set_title('QW2: Cache Hit Rates — Coherent Trie CHIME', fontsize=14)
    ax.set_xticks(x)
    ax.set_xticklabels(SKEW_DISPLAY)
    ax.set_ylim(0, 105)
    ax.legend(fontsize=10)
    ax.grid(True, alpha=0.3, axis='y')
    
    fig.tight_layout()
    outpath = os.path.join(output_dir, "qw2_cache_hit_rates.pdf")
    fig.savefig(outpath, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  Saved {outpath}")


def plot_improvement_summary(orig_data, coh_data, output_dir):
    """Summary chart showing improvement percentages."""
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(14, 6))
    
    x = np.arange(len(SKEW_LABELS))
    
    # Throughput improvement
    tput_improv = []
    for label in SKEW_LABELS:
        orig = orig_data.get(label, {}).get('throughput_ops', 0)
        coh = coh_data.get(label, {}).get('throughput_ops', 0)
        if orig > 0:
            improv = 100.0 * (coh - orig) / orig
        else:
            improv = 0
        tput_improv.append(improv)
    
    colors1 = ['green' if v >= 0 else 'red' for v in tput_improv]
    ax1.bar(x, tput_improv, color=colors1, alpha=0.85)
    ax1.set_ylabel('Throughput Improvement (%)', fontsize=12)
    ax1.set_xlabel('Workload Skew', fontsize=12)
    ax1.set_title('Throughput: Coherent Trie vs Original', fontsize=13)
    ax1.set_xticks(x)
    ax1.set_xticklabels(SKEW_DISPLAY)
    ax1.axhline(y=0, color='black', linestyle='-', linewidth=0.5)
    ax1.grid(True, alpha=0.3, axis='y')
    
    # P99 latency reduction
    lat_reduction = []
    for label in SKEW_LABELS:
        orig = orig_data.get(label, {}).get('read_p99_us', 0)
        coh = coh_data.get(label, {}).get('read_p99_us', 0)
        if orig > 0:
            reduction = 100.0 * (orig - coh) / orig
        else:
            reduction = 0
        lat_reduction.append(reduction)
    
    colors2 = ['green' if v >= 0 else 'red' for v in lat_reduction]
    ax2.bar(x, lat_reduction, color=colors2, alpha=0.85)
    ax2.set_ylabel('P99 Latency Reduction (%)', fontsize=12)
    ax2.set_xlabel('Workload Skew', fontsize=12)
    ax2.set_title('P99 Latency: Coherent Trie vs Original', fontsize=13)
    ax2.set_xticks(x)
    ax2.set_xticklabels(SKEW_DISPLAY)
    ax2.axhline(y=0, color='black', linestyle='-', linewidth=0.5)
    ax2.grid(True, alpha=0.3, axis='y')
    
    fig.suptitle('QW2: Performance Improvement Summary', fontsize=14, y=1.02)
    fig.tight_layout()
    outpath = os.path.join(output_dir, "qw2_improvement_summary.pdf")
    fig.savefig(outpath, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  Saved {outpath}")


def plot_latency_cdf_overlay(results_dir, output_dir, label="zipf_0.99"):
    """CDF overlay comparing Original vs Coherent Trie for a single skew point."""
    fig, ax = plt.subplots(figsize=(10, 6))
    
    # Original CHIME latency histogram
    orig_hist_file = os.path.join(results_dir, "chime_original", 
                                   f"original_{label}_chime_read_latency.dat")
    lat_o, cnt_o = load_histogram(orig_hist_file)
    x_o, y_o = hist_to_cdf(lat_o, cnt_o)
    
    if len(x_o) > 0:
        ax.plot(x_o, y_o, label="Original CHIME", color=COLORS['original'], 
                linewidth=2)
    else:
        print(f"  WARNING: No histogram data for Original CHIME ({label})")
    
    # Coherent Trie histogram (may have different filename patterns)
    for prefix in ["coherent", "chime"]:
        coh_hist_file = os.path.join(results_dir, "chime_coherent",
                                      f"coherent_{label}_{prefix}_read_latency.dat")
        lat_c, cnt_c = load_histogram(coh_hist_file)
        if len(lat_c) > 0:
            break
    
    x_c, y_c = hist_to_cdf(lat_c, cnt_c)
    
    if len(x_c) > 0:
        ax.plot(x_c, y_c, label="Coherent Trie CHIME", color=COLORS['coherent'], 
                linewidth=2)
    else:
        print(f"  WARNING: No histogram data for Coherent Trie ({label})")
    
    label_display = label.replace("_", " ").replace("zipf", "Zipf θ=")
    ax.set_xlabel('Read Latency (µs)', fontsize=12)
    ax.set_ylabel('CDF', fontsize=12)
    ax.set_title(f'QW2: Read Latency CDF — {label_display}', fontsize=14)
    ax.set_xlim(0, None)
    ax.set_ylim(0, 1.02)
    ax.legend(fontsize=11, loc='lower right')
    ax.grid(True, alpha=0.3)
    
    # Mark P50 and P99
    for name, xs, ys, color in [("Original", x_o, y_o, COLORS['original']), 
                                  ("Coherent", x_c, y_c, COLORS['coherent'])]:
        if len(xs) > 0:
            p50_idx = np.searchsorted(ys, 0.5)
            p99_idx = np.searchsorted(ys, 0.99)
            if p50_idx < len(xs):
                ax.axvline(xs[p50_idx], color=color, linestyle='--', alpha=0.5)
            if p99_idx < len(xs):
                ax.axvline(xs[p99_idx], color=color, linestyle=':', alpha=0.5)
    
    fig.tight_layout()
    outpath = os.path.join(output_dir, f"qw2_latency_cdf_{label}.pdf")
    fig.savefig(outpath, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  Saved {outpath}")


def generate_summary_table(orig_data, coh_data, output_dir):
    """Generate a text summary table of all results."""
    outpath = os.path.join(output_dir, "qw2_summary.txt")
    
    with open(outpath, 'w') as f:
        f.write("=" * 80 + "\n")
        f.write("QW2: Original CHIME vs Coherent Trie CHIME — Performance Summary\n")
        f.write("=" * 80 + "\n\n")
        
        f.write(f"{'Skew':<12} | {'Orig Tput':>12} | {'Coh Tput':>12} | {'Improv':>8} | "
                f"{'Orig P99':>10} | {'Coh P99':>10} | {'Reduction':>10}\n")
        f.write("-" * 80 + "\n")
        
        for label, display in zip(SKEW_LABELS, SKEW_DISPLAY):
            orig = orig_data.get(label, {})
            coh = coh_data.get(label, {})
            
            orig_tput = orig.get('throughput_mops', 0)
            coh_tput = coh.get('throughput_mops', 0)
            tput_improv = 100.0 * (coh_tput - orig_tput) / orig_tput if orig_tput > 0 else 0
            
            orig_p99 = orig.get('read_p99_us', 0)
            coh_p99 = coh.get('read_p99_us', 0)
            lat_reduction = 100.0 * (orig_p99 - coh_p99) / orig_p99 if orig_p99 > 0 else 0
            
            f.write(f"{display:<12} | {orig_tput:>10.2f} M | {coh_tput:>10.2f} M | "
                    f"{tput_improv:>+7.1f}% | {orig_p99:>8.1f} µs | {coh_p99:>8.1f} µs | "
                    f"{lat_reduction:>+9.1f}%\n")
        
        f.write("\n")
        f.write("Notes:\n")
        f.write("  - Tput = Throughput in Million ops/sec\n")
        f.write("  - P99 = 99th percentile read latency\n")
        f.write("  - Positive improvement/reduction = Coherent Trie is better\n")
    
    print(f"  Saved {outpath}")


# ============================================================================
# Main
# ============================================================================

def main():
    parser = argparse.ArgumentParser(description="QW2 Comparison Plots")
    parser.add_argument("--results-dir", default="./results", help="Results directory")
    parser.add_argument("--output-dir", default="./plots", help="Output directory")
    args = parser.parse_args()
    
    results_dir = os.path.abspath(args.results_dir)
    output_dir = os.path.abspath(args.output_dir)
    os.makedirs(output_dir, exist_ok=True)
    
    print(f"Loading results from: {results_dir}")
    print(f"Saving plots to: {output_dir}")
    
    # Load all results
    orig_data = {}
    coh_data = {}
    
    for label in SKEW_LABELS:
        # Original CHIME
        orig_log = os.path.join(results_dir, "chime_original", f"original_{label}_stdout.log")
        orig_data[label] = parse_chime_log(orig_log)
        
        # Coherent Trie CHIME
        coh_log = os.path.join(results_dir, "chime_coherent", f"coherent_{label}_stdout.log")
        coh_data[label] = parse_chime_log(coh_log)
    
    print("\nParsed data summary:")
    for label in SKEW_LABELS:
        print(f"  {label}:")
        print(f"    Original: tput={orig_data[label].get('throughput_mops', 'N/A'):.2f} Mops, "
              f"P99={orig_data[label].get('read_p99_us', 'N/A')} µs")
        print(f"    Coherent: tput={coh_data[label].get('throughput_mops', 'N/A'):.2f} Mops, "
              f"P99={coh_data[label].get('read_p99_us', 'N/A')} µs")
    
    print("\nGenerating plots...")
    
    # Generate all plots
    plot_throughput_comparison(orig_data, coh_data, output_dir)
    plot_latency_comparison(orig_data, coh_data, output_dir, 'p50')
    plot_latency_comparison(orig_data, coh_data, output_dir, 'p99')
    plot_cache_hit_rates(coh_data, output_dir)
    plot_improvement_summary(orig_data, coh_data, output_dir)
    plot_latency_cdf_overlay(results_dir, output_dir, "zipf_0.99")
    generate_summary_table(orig_data, coh_data, output_dir)
    
    print("\nDone! All plots saved to:", output_dir)


if __name__ == "__main__":
    main()
