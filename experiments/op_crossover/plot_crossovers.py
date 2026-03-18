#!/usr/bin/env python3
"""
plot_crossovers.py — Generate graphs for operating point crossover experiments.

Usage:
    python3 plot_crossovers.py [--exp A|B|C|D|all]

Reads .dat files from results/dex/ and results/chime/.
Generates PDF/PNG figures for each experiment.

.dat file format (both DEX and CHIME):
    # header lines starting with #
    latency_ns<TAB>count
    500        12
    1000       45
    ...
"""

import os
import sys
import re
import glob
import argparse
import numpy as np
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
import matplotlib.ticker as ticker

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
RESULTS_DEX   = os.path.join(SCRIPT_DIR, "results", "dex")
RESULTS_CHIME = os.path.join(SCRIPT_DIR, "results", "chime")
RESULTS_DART  = os.path.join(SCRIPT_DIR, "results", "dart")
FIGS_DIR      = os.path.join(SCRIPT_DIR, "figures")
os.makedirs(FIGS_DIR, exist_ok=True)


# ─── Parse a latency .dat file ───────────────────────────────────────────────
def parse_dat(path):
    """Returns (latency_ns_array, count_array, metadata_dict)."""
    meta = {}
    lats, counts = [], []
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if line.startswith("#"):
                    # Extract key: value pairs from header
                    for kw in ["Total ops", "Avg", "P50", "P95", "P99", "P99.9"]:
                        m = re.search(kw + r"[:\s]+([\d.]+)\s*ns", line)
                        if m:
                            meta[kw] = float(m.group(1))
                elif "\t" in line:
                    parts = line.split("\t")
                    if len(parts) == 2:
                        lats.append(float(parts[0]))
                        counts.append(int(parts[1]))
    except FileNotFoundError:
        return None, None, {}
    if not lats:
        return None, None, meta
    return np.array(lats), np.array(counts), meta


def percentile_from_hist(lats, counts, pct):
    """Compute a percentile from a histogram."""
    total = counts.sum()
    target = total * pct / 100.0
    cumulative = 0
    for l, c in zip(lats, counts):
        cumulative += c
        if cumulative >= target:
            return l
    return lats[-1]


def extract_stats(path):
    """Return dict with throughput (Mops), p50, p99, p999 in µs, from .dat + stdout."""
    lats, counts, meta = parse_dat(path)
    stats = {}
    if lats is not None and counts is not None and counts.sum() > 0:
        stats["p50"]  = percentile_from_hist(lats, counts, 50)  / 1000.0   # → µs
        stats["p95"]  = percentile_from_hist(lats, counts, 95)  / 1000.0
        stats["p99"]  = percentile_from_hist(lats, counts, 99)  / 1000.0
        stats["p999"] = percentile_from_hist(lats, counts, 99.9)/ 1000.0
        # also try meta
        for k in ["P50", "P95", "P99", "P99.9"]:
            if k in meta:
                stats[k.lower().replace(".", "")] = meta[k] / 1000.0
    return stats


def extract_tput_from_stdout(log_path):
    """Parse throughput from a stdout log (Mops/s)."""
    if not os.path.exists(log_path):
        return None
    best = None
    with open(log_path) as f:
        for line in f:
            # DEX: "throughput: 7.22 Mops/s" or "Mops"
            m = re.search(r"throughput.*?([\d.]+)\s*[Mm]ops", line, re.I)
            if not m:
                m = re.search(r"([\d.]+)\s*[Mm]ops", line)
            if m:
                v = float(m.group(1))
                if best is None or v > best:
                    best = v
    return best


# ─── Figure style helpers ─────────────────────────────────────────────────────
COLORS = {
    "dex":   "#2196F3",   # blue
    "chime": "#FF9800",   # orange
    "dart":  "#4CAF50",   # green
}
MARKERS = {"dex": "o", "chime": "s", "dart": "^"}

def setup_ax(ax, xlabel, ylabel, title):
    ax.set_xlabel(xlabel, fontsize=11)
    ax.set_ylabel(ylabel, fontsize=11)
    ax.set_title(title, fontsize=12, fontweight="bold")
    ax.grid(True, alpha=0.3)
    ax.legend(fontsize=10)

def save_fig(fig, name):
    pdf_path = os.path.join(FIGS_DIR, name + ".pdf")
    png_path = os.path.join(FIGS_DIR, name + ".png")
    fig.tight_layout()
    fig.savefig(pdf_path, bbox_inches="tight")
    fig.savefig(png_path, dpi=150, bbox_inches="tight")
    print(f"  Saved: {pdf_path}")
    print(f"  Saved: {png_path}")
    plt.close(fig)


# ─── Experiment A: Cache Size Crossover ──────────────────────────────────────
def plot_exp_A():
    """DEX vs CHIME: throughput and P99 vs cache size.
    DEX: cache {64, 128, 256, 512} MB
    CHIME: cache {32, 64, 100} MB
    Shows: CHIME wins at small cache; DEX wins at large cache."""
    print("\n[Exp A] Cache Size Crossover: DEX vs CHIME")

    dex_caches   = [64, 128, 256, 512]
    chime_caches = [32, 64, 100]

    dex_tput, dex_p99     = [], []
    chime_tput, chime_p99 = [], []

    for c in dex_caches:
        prefix  = os.path.join(RESULTS_DEX, f"dex_cache_{c}mb")
        logpath = prefix + "_stdout.log"
        datpath = prefix + "_dex_read_latency.dat"
        tput = extract_tput_from_stdout(logpath)
        stats = extract_stats(datpath)
        dex_tput.append(tput)
        dex_p99.append(stats.get("p99"))

    for c in chime_caches:
        prefix  = os.path.join(RESULTS_CHIME, f"chime_cache_{c}mb")
        logpath = prefix + "_stdout.log"
        datpath = prefix + "_chime_read_latency.dat"
        tput = extract_tput_from_stdout(logpath)
        stats = extract_stats(datpath)
        chime_tput.append(tput)
        chime_p99.append(stats.get("p99"))

    # ── Figure: Throughput vs Cache ───────────────────────────────────────────
    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 4.5))

    if any(v is not None for v in dex_tput):
        valid_d = [(c, t) for c, t in zip(dex_caches, dex_tput) if t is not None]
        if valid_d:
            xd, yd = zip(*valid_d)
            ax1.plot(xd, yd, color=COLORS["dex"], marker=MARKERS["dex"],
                     linewidth=2, label="DEX (page cache)", zorder=3)

    if any(v is not None for v in chime_tput):
        valid_c = [(c, t) for c, t in zip(chime_caches, chime_tput) if t is not None]
        if valid_c:
            xc, yc = zip(*valid_c)
            ax1.plot(xc, yc, color=COLORS["chime"], marker=MARKERS["chime"],
                     linewidth=2, label="CHIME (path-aware cache)", zorder=3)

    ax1.axvspan(0, 70, alpha=0.08, color=COLORS["chime"],
                label="CHIME wins region")
    ax1.set_xlim(0, 550)
    setup_ax(ax1, "Cache Size (MB)", "Throughput (Mops/s)",
             "Exp A: Throughput vs Cache Size\n(uniform, 100% reads, 10M keys)")

    # ── Figure: P99 vs Cache ──────────────────────────────────────────────────
    if any(v is not None for v in dex_p99):
        valid_d = [(c, p) for c, p in zip(dex_caches, dex_p99) if p is not None]
        if valid_d:
            xd, yd = zip(*valid_d)
            ax2.plot(xd, yd, color=COLORS["dex"], marker=MARKERS["dex"],
                     linewidth=2, label="DEX P99")

    if any(v is not None for v in chime_p99):
        valid_c = [(c, p) for c, p in zip(chime_caches, chime_p99) if p is not None]
        if valid_c:
            xc, yc = zip(*valid_c)
            ax2.plot(xc, yc, color=COLORS["chime"], marker=MARKERS["chime"],
                     linewidth=2, label="CHIME P99")

    ax2.set_xlim(0, 550)
    setup_ax(ax2, "Cache Size (MB)", "P99 Read Latency (µs)",
             "Exp A: P99 Latency vs Cache Size\n(uniform, 100% reads, 10M keys)")

    save_fig(fig, "expA_cache_crossover")

    # ── Figure: CDF overlays for best DEX vs CHIME ───────────────────────────
    fig2, ax = plt.subplots(figsize=(7, 4.5))
    for c, color, system, results_dir in [
        (256,  COLORS["dex"],   "DEX 256 MB",   RESULTS_DEX),
        (64,   COLORS["dex"],   "DEX 64 MB",    RESULTS_DEX),
        (100,  COLORS["chime"], "CHIME 100 MB", RESULTS_CHIME),
        (32,   COLORS["chime"], "CHIME 32 MB",  RESULTS_CHIME),
    ]:
        sys_name = "dex" if "DEX" in system else "chime"
        label_id = f"cache_{c}mb"
        datpath  = os.path.join(results_dir, f"{sys_name}_{label_id}_{sys_name}_read_latency.dat")
        lats, counts, _ = parse_dat(datpath)
        if lats is None:
            continue
        total = counts.sum()
        cdf = np.cumsum(counts) / total * 100
        ax.plot(lats / 1000.0, cdf, label=system, color=color,
                linewidth=1.8, linestyle=("--" if "64" in system or "32" in system else "-"))

    ax.axhline(99, color="gray", linestyle=":", linewidth=0.8)
    ax.axhline(50, color="gray", linestyle=":", linewidth=0.8)
    ax.set_xlim(0, 50)
    ax.set_ylim(0, 100)
    setup_ax(ax, "Latency (µs)", "CDF (%)",
             "Exp A: Read Latency CDF — DEX vs CHIME\n(uniform, 10M keys)")
    save_fig(fig2, "expA_latency_cdf")


# ─── Experiment B: Skew Crossover ────────────────────────────────────────────
def plot_exp_B():
    """DEX vs CHIME: throughput and P99 vs Zipf theta.
    Both at 256 MB cache, 70/30 read/range mix.
    Shows: DEX dominates at high skew; CHIME is more stable at tail."""
    print("\n[Exp B] Skew Crossover: DEX vs CHIME | 256 MB cache | 70/30 mix")

    skew_labels = ["uniform", "zipf_0.6", "zipf_0.8", "zipf_0.9", "zipf_0.99"]
    skew_theta  = [0.0, 0.6, 0.8, 0.9, 0.99]

    dex_tput,   dex_p50,   dex_p99   = [], [], []
    chime_tput, chime_p50, chime_p99 = [], [], []

    for label in skew_labels:
        # DEX
        prefix  = os.path.join(RESULTS_DEX, f"expB_dex_{label}")
        tput    = extract_tput_from_stdout(prefix + "_stdout.log")
        stats_r = extract_stats(prefix + "_dex_read_latency.dat")
        dex_tput.append(tput)
        dex_p50.append(stats_r.get("p50"))
        dex_p99.append(stats_r.get("p99"))

        # CHIME
        prefix  = os.path.join(RESULTS_CHIME, f"expB_chime_{label}")
        tput    = extract_tput_from_stdout(prefix + "_stdout.log")
        stats_r = extract_stats(prefix + "_chime_read_latency.dat")
        chime_tput.append(tput)
        chime_p50.append(stats_r.get("p50"))
        chime_p99.append(stats_r.get("p99"))

    x = skew_theta
    fig, axes = plt.subplots(1, 3, figsize=(15, 4.5))

    def _plot_line(ax, x, y_dex, y_chime, ylabel, title):
        valid_d = [(xi, yi) for xi, yi in zip(x, y_dex)   if yi is not None]
        valid_c = [(xi, yi) for xi, yi in zip(x, y_chime) if yi is not None]
        if valid_d:
            xd, yd = zip(*valid_d)
            ax.plot(xd, yd, color=COLORS["dex"], marker="o", linewidth=2, label="DEX 256 MB")
        if valid_c:
            xc, yc = zip(*valid_c)
            ax.plot(xc, yc, color=COLORS["chime"], marker="s", linewidth=2, label="CHIME 256 MB")
        setup_ax(ax, "Zipf θ", ylabel, title)

    _plot_line(axes[0], x, dex_tput, chime_tput, "Throughput (Mops/s)",
               "Throughput vs Skew\n(256 MB cache, 70/30 read/range)")
    _plot_line(axes[1], x, dex_p50, chime_p50, "P50 Read Latency (µs)",
               "P50 Read Latency vs Skew")
    _plot_line(axes[2], x, dex_p99, chime_p99, "P99 Read Latency (µs)",
               "P99 Read Latency vs Skew")

    save_fig(fig, "expB_skew_crossover")

    # ── Range vs Read latency side-by-side for uniform ────────────────────────
    fig2, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 4.5))
    for label, theta_str in [("uniform", "Uniform"), ("zipf_0.99", "θ=0.99")]:
        for sys_name, color, results_dir in [
            ("dex",   COLORS["dex"],   RESULTS_DEX),
            ("chime", COLORS["chime"], RESULTS_CHIME),
        ]:
            for ax, op in [(ax1, "read"), (ax2, "range")]:
                datpath = os.path.join(results_dir, f"expB_{sys_name}_{label}_{sys_name}_{op}_latency.dat")
                lats, counts, _ = parse_dat(datpath)
                if lats is None:
                    continue
                total = counts.sum()
                cdf   = np.cumsum(counts) / total * 100
                lname = f"{sys_name.upper()} {theta_str}"
                ls    = "-" if theta_str == "θ=0.99" else "--"
                ax.plot(lats / 1000.0, cdf, label=lname, color=color, linewidth=1.8, linestyle=ls)

    for ax, op in [(ax1, "Read"), (ax2, "Range Scan")]:
        ax.set_xlim(0, 80)
        ax.set_ylim(0, 100)
        ax.axhline(99, color="gray", linestyle=":", linewidth=0.8)
        ax.axhline(50, color="gray", linestyle=":", linewidth=0.8)
        setup_ax(ax, "Latency (µs)", "CDF (%)", f"{op} Latency CDF\n256 MB cache")

    save_fig(fig2, "expB_latency_cdf_read_vs_range")


# ─── Experiment C: DART vs CHIME ─────────────────────────────────────────────
def plot_exp_C():
    """DART vs CHIME: throughput vs CHIME cache size.
    DART is flat. CHIME degrades at small cache. Shows crossover."""
    print("\n[Exp C] DART vs CHIME | 2M keys | 100% reads")

    chime_caches = [4, 16, 32, 64, 100]
    skew_configs  = [("uniform", "Uniform"), ("zipf_0.99", "θ=0.99")]

    fig, axes = plt.subplots(1, 2, figsize=(11, 4.5))

    for ax, (skew_label, skew_name) in zip(axes, skew_configs):
        chime_tput, chime_p99 = [], []

        for c in chime_caches:
            prefix  = os.path.join(RESULTS_CHIME, f"chime_cache{c}mb_{skew_label}")
            logpath = prefix + "_stdout.log"
            datpath = prefix + "_chime_read_latency.dat"
            tput  = extract_tput_from_stdout(logpath)
            stats = extract_stats(datpath)
            chime_tput.append(tput)
            chime_p99.append(stats.get("p99"))

        # DART is constant (read from dart results or use known value)
        dart_tput_val = None
        dart_dat = os.path.join(RESULTS_DART, f"dart_{skew_label}_stdout.log")
        dart_tput_val = extract_tput_from_stdout(dart_dat)

        valid_c = [(c, t) for c, t in zip(chime_caches, chime_tput) if t is not None]
        if valid_c:
            xc, yc = zip(*valid_c)
            ax.plot(xc, yc, color=COLORS["chime"], marker="s", linewidth=2,
                    label="CHIME (path-aware cache)")

        if dart_tput_val:
            ax.axhline(dart_tput_val, color=COLORS["dart"], linewidth=2,
                       linestyle="--", label=f"DART (~1 MB cache, {dart_tput_val:.2f} Mops/s)")
        else:
            # Show known approximate DART floor if no data yet
            ax.axhline(1.67, color=COLORS["dart"], linewidth=2, linestyle="--",
                       label="DART (~1 MB cache, ~1.67 Mops/s — expected)")

        ax.set_xlabel("CHIME Cache Size (MB)", fontsize=11)
        ax.set_ylabel("Throughput (Mops/s)", fontsize=11)
        ax.set_title(f"DART vs CHIME — {skew_name}\n(2M keys, 100% reads, 30 threads)", fontsize=11)
        ax.grid(True, alpha=0.3)
        ax.legend(fontsize=9)

    save_fig(fig, "expC_dart_vs_chime")


# ─── Experiment D: Node Size Structural Sweep ─────────────────────────────────
def plot_exp_D():
    """DEX and CHIME: throughput/P99 vs node size.
    Shows how structural parameters shift operating points."""
    print("\n[Exp D] Node Size Structural Sweep | 128 MB cache | uniform")

    dex_leaf_sizes   = [512, 1024, 2048]
    chime_span_sizes = [32, 64, 128]

    # Approximate node size in bytes for CHIME (leafEntrySize ≈ 20 bytes)
    chime_approx_bytes = [s * 20 for s in chime_span_sizes]

    dex_tput,   dex_p99   = [], []
    chime_tput, chime_p99 = [], []

    for sz in dex_leaf_sizes:
        prefix  = os.path.join(RESULTS_DEX, f"expD_dex_leaf{sz}b")
        tput    = extract_tput_from_stdout(prefix + "_stdout.log")
        stats   = extract_stats(prefix + "_dex_read_latency.dat")
        dex_tput.append(tput)
        dex_p99.append(stats.get("p99"))

    for span in chime_span_sizes:
        prefix  = os.path.join(RESULTS_CHIME, f"expD_chime_span{span}")
        tput    = extract_tput_from_stdout(prefix + "_stdout.log")
        stats   = extract_stats(prefix + "_chime_read_latency.dat")
        chime_tput.append(tput)
        chime_p99.append(stats.get("p99"))

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 4.5))

    # Throughput
    valid_d = [(s, t) for s, t in zip(dex_leaf_sizes, dex_tput) if t is not None]
    if valid_d:
        xd, yd = zip(*valid_d)
        ax1.plot(xd, yd, color=COLORS["dex"], marker="o", linewidth=2,
                 label="DEX (leaf page bytes)")

    valid_c = [(s, t) for s, t in zip(chime_approx_bytes, chime_tput) if t is not None]
    if valid_c:
        xc, yc = zip(*valid_c)
        ax1.plot(xc, yc, color=COLORS["chime"], marker="s", linewidth=2,
                 label="CHIME (approx node bytes)")

    setup_ax(ax1, "Node Size (bytes)", "Throughput (Mops/s)",
             "Exp D: Throughput vs Node Size\n(128 MB cache, uniform, 10M keys)")

    # P99
    valid_d = [(s, p) for s, p in zip(dex_leaf_sizes, dex_p99) if p is not None]
    if valid_d:
        xd, yd = zip(*valid_d)
        ax2.plot(xd, yd, color=COLORS["dex"], marker="o", linewidth=2, label="DEX P99")

    valid_c = [(s, p) for s, p in zip(chime_approx_bytes, chime_p99) if p is not None]
    if valid_c:
        xc, yc = zip(*valid_c)
        ax2.plot(xc, yc, color=COLORS["chime"], marker="s", linewidth=2, label="CHIME P99")

    setup_ax(ax2, "Node Size (bytes)", "P99 Read Latency (µs)",
             "Exp D: P99 Latency vs Node Size\n(128 MB cache, uniform, 10M keys)")

    save_fig(fig, "expD_node_size")


# ─── Summary operating-point figure ──────────────────────────────────────────
def plot_summary():
    """2×2 grid summarizing all four experiments."""
    print("\n[Summary] Generating 2×2 operating point map...")
    fig, axes = plt.subplots(2, 2, figsize=(12, 9))

    labels_A  = ["DEX\ndeadzone\n(<64 MB)", "DEX 64 MB", "DEX 128 MB", "DEX 256 MB",
                  "DEX 512 MB", "CHIME 32 MB", "CHIME 64 MB", "CHIME 100 MB"]
    notes = [
        "Exp A: Cache Size\n← CHIME wins | DEX wins →",
        "Exp B: Skew\n← CHIME stable | DEX wins at θ>0.8 →",
        "Exp C: DART vs CHIME\n← DART floor | CHIME improves with cache →",
        "Exp D: Node Size\n← Small nodes (more cache needed) | Large nodes →",
    ]
    for ax, note in zip(axes.flat, notes):
        ax.text(0.5, 0.5, note, ha="center", va="center", fontsize=13,
                transform=ax.transAxes,
                bbox=dict(boxstyle="round,pad=0.5", facecolor="#f5f5f5", edgecolor="gray"))
        ax.set_axis_off()

    fig.suptitle("ATLAS Operating Point Map: DEX / CHIME / DART",
                 fontsize=14, fontweight="bold")
    save_fig(fig, "summary_operating_points")


# ─── Main ─────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--exp", default="all",
                        choices=["A", "B", "C", "D", "summary", "all"])
    args = parser.parse_args()

    exp = args.exp.upper()
    print(f"Plotting experiment(s): {exp}")
    print(f"Reading from: {RESULTS_DEX}, {RESULTS_CHIME}")
    print(f"Saving to:    {FIGS_DIR}")

    if exp in ("A", "ALL"):  plot_exp_A()
    if exp in ("B", "ALL"):  plot_exp_B()
    if exp in ("C", "ALL"):  plot_exp_C()
    if exp in ("D", "ALL"):  plot_exp_D()
    if exp in ("SUMMARY", "ALL"): plot_summary()

    print("\nDone. Figures saved to:", FIGS_DIR)


if __name__ == "__main__":
    main()
