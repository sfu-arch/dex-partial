#!/usr/bin/env python3
"""
QW1 Zipfian Skew Sweep — Latency Comparison Plot (DEX vs CHIME)

Reads latency histograms from:
    results/dex/dex_{label}_{read,range}_latency.dat
    results/chime/chime_{label}_{read,range}_latency.dat

Generates:
    1. CDF of read latency per skew point (DEX vs CHIME overlay)
    2. CDF of range latency per skew point (DEX vs CHIME overlay)
    3. Median + P99 read latency bar chart across skew points
    4. Median + P99 range latency bar chart across skew points
"""

import os
import sys
import numpy as np

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    print("ERROR: matplotlib required. Install with: pip install matplotlib")
    sys.exit(1)

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
DEX_DIR = os.path.join(SCRIPT_DIR, "results", "dex")
CHIME_DIR = os.path.join(SCRIPT_DIR, "results", "chime")
OUT_DIR = os.path.join(SCRIPT_DIR, "plots")

LABELS = ["uniform", "zipf_0.6", "zipf_0.8", "zipf_0.9", "zipf_0.99"]
DISPLAY_LABELS = ["Uniform", r"Zipf $\theta$=0.6", r"Zipf $\theta$=0.8",
                  r"Zipf $\theta$=0.9", r"Zipf $\theta$=0.99"]

# ─── Load histogram ──────────────────────────────────────────────────────────

def load_histogram(filepath):
    """Load latency_ns\\tcount file. Returns (latency_ns[], count[])."""
    lat, cnt = [], []
    if not os.path.exists(filepath):
        print(f"  WARNING: {filepath} not found")
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


def percentile_from_hist(lat, cnt, pct):
    """Get the pct-th percentile latency (µs) from histogram."""
    if len(cnt) == 0 or cnt.sum() == 0:
        return float('nan')
    total = cnt.sum()
    cumsum = np.cumsum(cnt)
    idx = np.searchsorted(cumsum, total * pct / 100.0)
    idx = min(idx, len(lat) - 1)
    return lat[idx] / 1000.0  # ns → µs


# ─── Plot CDFs ───────────────────────────────────────────────────────────────

def plot_cdf(op_type, title_suffix):
    """Plot CDF overlay for all skew points, DEX vs CHIME."""
    fig, axes = plt.subplots(1, len(LABELS), figsize=(4 * len(LABELS), 4),
                              sharey=True)
    if len(LABELS) == 1:
        axes = [axes]

    for i, (label, disp) in enumerate(zip(LABELS, DISPLAY_LABELS)):
        ax = axes[i]

        # DEX
        dex_file = os.path.join(DEX_DIR, f"dex_{label}_dex_{op_type}_latency.dat")
        lat_d, cnt_d = load_histogram(dex_file)
        x_d, y_d = hist_to_cdf(lat_d, cnt_d)
        if len(x_d) > 0:
            ax.plot(x_d, y_d, label="DEX", color="tab:blue", linewidth=1.5)

        # CHIME
        chime_file = os.path.join(CHIME_DIR, f"chime_{label}_chime_{op_type}_latency.dat")
        lat_c, cnt_c = load_histogram(chime_file)
        x_c, y_c = hist_to_cdf(lat_c, cnt_c)
        if len(x_c) > 0:
            ax.plot(x_c, y_c, label="CHIME", color="tab:red", linewidth=1.5)

        ax.set_title(disp, fontsize=11)
        ax.set_xlabel("Latency (µs)")
        if i == 0:
            ax.set_ylabel("CDF")
        ax.set_xlim(0, None)
        ax.set_ylim(0, 1.02)
        ax.legend(fontsize=8, loc="lower right")
        ax.grid(True, alpha=0.3)

    fig.suptitle(f"QW1: {title_suffix} Latency CDF — DEX vs CHIME", fontsize=13)
    fig.tight_layout(rect=[0, 0, 1, 0.94])
    outpath = os.path.join(OUT_DIR, f"qw1_{op_type}_cdf.pdf")
    fig.savefig(outpath, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  Saved {outpath}")


# ─── Bar chart: median + P99 ──────────────────────────────────────────────────

def plot_bar_summary(op_type, title_suffix):
    """Bar chart of median and P99 latency across skew points."""
    dex_med, dex_p99 = [], []
    chime_med, chime_p99 = [], []

    for label in LABELS:
        # DEX
        dex_file = os.path.join(DEX_DIR, f"dex_{label}_dex_{op_type}_latency.dat")
        lat, cnt = load_histogram(dex_file)
        dex_med.append(percentile_from_hist(lat, cnt, 50))
        dex_p99.append(percentile_from_hist(lat, cnt, 99))

        # CHIME
        chime_file = os.path.join(CHIME_DIR, f"chime_{label}_chime_{op_type}_latency.dat")
        lat, cnt = load_histogram(chime_file)
        chime_med.append(percentile_from_hist(lat, cnt, 50))
        chime_p99.append(percentile_from_hist(lat, cnt, 99))

    x = np.arange(len(LABELS))
    width = 0.35

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))

    # Median
    ax1.bar(x - width / 2, dex_med, width, label="DEX", color="tab:blue", alpha=0.85)
    ax1.bar(x + width / 2, chime_med, width, label="CHIME", color="tab:red", alpha=0.85)
    ax1.set_ylabel("Median Latency (µs)")
    ax1.set_title(f"{title_suffix} — Median (P50)")
    ax1.set_xticks(x)
    ax1.set_xticklabels(DISPLAY_LABELS, fontsize=9, rotation=15)
    ax1.legend()
    ax1.grid(axis="y", alpha=0.3)

    # P99
    ax2.bar(x - width / 2, dex_p99, width, label="DEX", color="tab:blue", alpha=0.85)
    ax2.bar(x + width / 2, chime_p99, width, label="CHIME", color="tab:red", alpha=0.85)
    ax2.set_ylabel("P99 Latency (µs)")
    ax2.set_title(f"{title_suffix} — P99")
    ax2.set_xticks(x)
    ax2.set_xticklabels(DISPLAY_LABELS, fontsize=9, rotation=15)
    ax2.legend()
    ax2.grid(axis="y", alpha=0.3)

    fig.suptitle(f"QW1: {title_suffix} Latency Summary — DEX vs CHIME", fontsize=13)
    fig.tight_layout(rect=[0, 0, 1, 0.94])
    outpath = os.path.join(OUT_DIR, f"qw1_{op_type}_bar.pdf")
    fig.savefig(outpath, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"  Saved {outpath}")


# ─── Print summary table ─────────────────────────────────────────────────────

def print_summary_table():
    """Print a text summary of median / P99 for all configs."""
    print("\n" + "=" * 90)
    print(f"{'Config':<15} {'DEX Read P50':>14} {'DEX Read P99':>14} "
          f"{'CHIME Read P50':>15} {'CHIME Read P99':>15}")
    print("-" * 90)
    for label, disp in zip(LABELS, DISPLAY_LABELS):
        dex_file = os.path.join(DEX_DIR, f"dex_{label}_dex_read_latency.dat")
        lat, cnt = load_histogram(dex_file)
        dm = percentile_from_hist(lat, cnt, 50)
        dp = percentile_from_hist(lat, cnt, 99)

        chime_file = os.path.join(CHIME_DIR, f"chime_{label}_chime_read_latency.dat")
        lat, cnt = load_histogram(chime_file)
        cm = percentile_from_hist(lat, cnt, 50)
        cp = percentile_from_hist(lat, cnt, 99)

        print(f"{disp:<15} {dm:>13.1f}µs {dp:>13.1f}µs {cm:>14.1f}µs {cp:>14.1f}µs")

    print("=" * 90)
    print(f"\n{'Config':<15} {'DEX Range P50':>14} {'DEX Range P99':>14} "
          f"{'CHIME Range P50':>16} {'CHIME Range P99':>16}")
    print("-" * 90)
    for label, disp in zip(LABELS, DISPLAY_LABELS):
        dex_file = os.path.join(DEX_DIR, f"dex_{label}_dex_range_latency.dat")
        lat, cnt = load_histogram(dex_file)
        dm = percentile_from_hist(lat, cnt, 50)
        dp = percentile_from_hist(lat, cnt, 99)

        chime_file = os.path.join(CHIME_DIR, f"chime_{label}_chime_range_latency.dat")
        lat, cnt = load_histogram(chime_file)
        cm = percentile_from_hist(lat, cnt, 50)
        cp = percentile_from_hist(lat, cnt, 99)

        print(f"{disp:<15} {dm:>13.1f}µs {dp:>13.1f}µs {cm:>15.1f}µs {cp:>15.1f}µs")
    print("=" * 90)


# ─── Main ─────────────────────────────────────────────────────────────────────

if __name__ == "__main__":
    os.makedirs(OUT_DIR, exist_ok=True)

    print(">>> Generating QW1 Zipfian Skew plots...")
    plot_cdf("read", "Read")
    plot_cdf("range", "Range Scan")
    plot_bar_summary("read", "Read")
    plot_bar_summary("range", "Range Scan")
    print_summary_table()
    print(f"\n>>> All plots saved to: {OUT_DIR}")
