#!/usr/bin/env python3
"""
DEX Experiment Results — Radar Plots & Analysis
================================================
Visualizes throughput, latency, and RDMA metrics from DEX B+ tree sweep experiments.
Data source: dex_memnode_output.txt / dex_compnode_output.txt

Cluster: cs-dis-srv09s (compute) + 10.30.1.9 (memory)
Tree:    inner=256B (fanout=11), leaf=512B (cap=26), depth=10, 50M keys
Threads: 36 app + 4 mem, 8GB DSM pool, admission_rate=0.1, rpc_rate=0
"""

import numpy as np
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch
from math import pi
import os

# ──────────────────────────────────────────────────────────
# 1.  RAW DATA  (extracted from experiment output files)
# ──────────────────────────────────────────────────────────

# Point-lookup results: {(distribution, cache_MB): metrics_dict}
point_data = {
    # uniform — 128MB came from compnode (it got node-0 for that run)
    ("uniform", 128): {"tp": 2.392, "rdma_read_op": 2.134, "avg_lat": 14691, "p50": 14500, "p99": 30500, "p999": 37000, "rdma_size_op": 1092.43},
    ("uniform", 256): {"tp": 2.920, "rdma_read_op": 1.617, "avg_lat": 11958, "p50": 11500, "p99": 27000, "p999": 34000, "rdma_size_op": 827.48},
    ("uniform", 512): {"tp": 4.054, "rdma_read_op": 1.076, "avg_lat":  8510, "p50":  7500, "p99": 24000, "p999": 31000, "rdma_size_op": 550.91},

    ("zipf0.30", 128): {"tp": 2.131, "rdma_read_op": 2.132, "avg_lat": 16521, "p50": 16000, "p99": 34000, "p999": 42000, "rdma_size_op": 1091.80},
    ("zipf0.30", 256): {"tp": 2.794, "rdma_read_op": 1.614, "avg_lat": 12512, "p50": 12000, "p99": 28500, "p999": 36000, "rdma_size_op": 826.09},
    ("zipf0.30", 512): {"tp": 4.088, "rdma_read_op": 1.072, "avg_lat":  8438, "p50":  7500, "p99": 24500, "p999": 31500, "rdma_size_op": 549.05},

    ("zipf0.50", 128): {"tp": 1.996, "rdma_read_op": 2.126, "avg_lat": 17660, "p50": 17500, "p99": 36500, "p999": 45000, "rdma_size_op": 1088.73},
    ("zipf0.50", 256): {"tp": 2.895, "rdma_read_op": 1.607, "avg_lat": 12078, "p50": 11500, "p99": 29500, "p999": 37500, "rdma_size_op": 822.25},
    ("zipf0.50", 512): {"tp": 4.061, "rdma_read_op": 1.063, "avg_lat":  8491, "p50":  7500, "p99": 24000, "p999": 31000, "rdma_size_op": 544.22},

    ("zipf0.60", 128): {"tp": 2.200, "rdma_read_op": 2.107, "avg_lat": 16010, "p50": 15500, "p99": 35000, "p999": 43000, "rdma_size_op": 1078.78},
    ("zipf0.60", 256): {"tp": 2.898, "rdma_read_op": 1.583, "avg_lat": 12053, "p50": 11500, "p99": 27500, "p999": 34500, "rdma_size_op": 810.11},
    ("zipf0.60", 512): {"tp": 4.400, "rdma_read_op": 1.036, "avg_lat":  7813, "p50":  7000, "p99": 23000, "p999": 29500, "rdma_size_op": 530.43},

    ("zipf0.99", 128): {"tp": 3.872, "rdma_read_op": 1.102, "avg_lat":  8953, "p50":  6500, "p99": 32500, "p999": 40500, "rdma_size_op": 564.22},
    ("zipf0.99", 256): {"tp": 6.111, "rdma_read_op": 0.744, "avg_lat":  5557, "p50":  2500, "p99": 25000, "p999": 33000, "rdma_size_op": 380.90},
    ("zipf0.99", 512): {"tp": 9.695, "rdma_read_op": 0.421, "avg_lat":  3356, "p50":   500, "p99": 19000, "p999": 25500, "rdma_size_op": 215.64},
}

# Range-query results: all 15 experiments completed
range_data = {
    ("uniform", 128): {"tp": 0.425, "rdma_read_op": 11.304, "avg_lat": 84267, "p50": 83500, "p99": 124000, "p999": 146000, "rdma_size_op": 5787.83},
    ("uniform", 256): {"tp": 0.436, "rdma_read_op": 10.225, "avg_lat": 82151, "p50": 81000, "p99": 125000, "p999": 145500, "rdma_size_op": 5235.03},
    ("uniform", 512): {"tp": 0.520, "rdma_read_op":  8.631, "avg_lat": 68828, "p50": 68000, "p99": 106500, "p999": 124000, "rdma_size_op": 4419.26},

    ("zipf0.30", 128): {"tp": 0.418, "rdma_read_op": 11.303, "avg_lat": 85777, "p50": 85500, "p99": 124000, "p999": 147000, "rdma_size_op": 5787.04},
    ("zipf0.30", 256): {"tp": 0.434, "rdma_read_op": 10.221, "avg_lat": 82655, "p50": 81500, "p99": 126500, "p999": 145500, "rdma_size_op": 5233.19},
    ("zipf0.30", 512): {"tp": 0.508, "rdma_read_op":  8.625, "avg_lat": 70441, "p50": 69500, "p99": 111500, "p999": 128500, "rdma_size_op": 4416.05},

    ("zipf0.50", 128): {"tp": 0.398, "rdma_read_op": 11.294, "avg_lat": 90060, "p50": 89000, "p99": 135000, "p999": 150000, "rdma_size_op": 5782.70},
    ("zipf0.50", 256): {"tp": 0.460, "rdma_read_op": 10.207, "avg_lat": 77838, "p50": 77000, "p99": 118500, "p999": 133500, "rdma_size_op": 5225.97},
    ("zipf0.50", 512): {"tp": 0.530, "rdma_read_op":  8.600, "avg_lat": 67533, "p50": 66500, "p99": 104500, "p999": 119000, "rdma_size_op": 4403.40},

    ("zipf0.60", 128): {"tp": 0.411, "rdma_read_op": 11.252, "avg_lat": 87292, "p50": 87000, "p99": 127000, "p999": 142500, "rdma_size_op": 5761.06},
    ("zipf0.60", 256): {"tp": 0.442, "rdma_read_op": 10.146, "avg_lat": 80978, "p50": 80500, "p99": 123000, "p999": 139000, "rdma_size_op": 5194.48},
    ("zipf0.60", 512): {"tp": 0.542, "rdma_read_op":  8.508, "avg_lat": 66086, "p50": 65000, "p99": 106500, "p999": 122000, "rdma_size_op": 4355.94},

    ("zipf0.99", 128): {"tp": 0.692, "rdma_read_op":  6.770, "avg_lat": 51628, "p50": 62500, "p99": 117500, "p999": 133000, "rdma_size_op": 3465.99},
    ("zipf0.99", 256): {"tp": 0.786, "rdma_read_op":  5.598, "avg_lat": 45412, "p50": 52000, "p99": 109500, "p999": 125500, "rdma_size_op": 2866.40},
    ("zipf0.99", 512): {"tp": 0.991, "rdma_read_op":  4.196, "avg_lat": 35983, "p50": 27000, "p99": 102500, "p999": 120000, "rdma_size_op": 2148.28},
}

DISTRIBUTIONS = ["uniform", "zipf0.30", "zipf0.50", "zipf0.60", "zipf0.99"]
DIST_LABELS   = ["Uniform", "Zipf θ=0.30", "Zipf θ=0.50", "Zipf θ=0.60", "Zipf θ=0.99"]
CACHES = [128, 256, 512]
CACHE_COLORS = {128: "#e74c3c", 256: "#3498db", 512: "#2ecc71"}
DIST_COLORS  = {
    "uniform":  "#e74c3c",
    "zipf0.30": "#e67e22",
    "zipf0.50": "#f1c40f",
    "zipf0.60": "#3498db",
    "zipf0.99": "#9b59b6",
}

OUT_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "plots")
os.makedirs(OUT_DIR, exist_ok=True)


# ──────────────────────────────────────────────────────────
# 2.  RADAR PLOT HELPERS
# ──────────────────────────────────────────────────────────

def make_radar(ax, categories, values_dict, title, colors, fill_alpha=0.15,
               higher_is_better=None):
    """
    Draw a radar chart.

    Parameters
    ----------
    categories : list[str]  — axis labels
    values_dict : dict[str, list[float]]  — label → normalized values (0-1)
    title : str
    colors : dict[str, str]  — label → colour
    higher_is_better : list[bool]|None  — per-axis annotation
    """
    N = len(categories)
    angles = [n / float(N) * 2 * pi for n in range(N)]
    angles += angles[:1]  # close polygon

    ax.set_theta_offset(pi / 2)
    ax.set_theta_direction(-1)
    ax.set_rlabel_position(30)

    ax.set_xticks(angles[:-1])
    ax.set_xticklabels(categories, size=8, weight="bold")
    ax.set_yticks([0.25, 0.50, 0.75, 1.0])
    ax.set_yticklabels(["25%", "50%", "75%", "100%"], size=6, color="grey")
    ax.set_ylim(0, 1.1)

    for label, vals in values_dict.items():
        data = vals + vals[:1]
        color = colors.get(label, "#333333")
        ax.plot(angles, data, "o-", linewidth=1.8, label=label, color=color, markersize=4)
        ax.fill(angles, data, alpha=fill_alpha, color=color)

    ax.set_title(title, size=11, weight="bold", pad=18)

    # Add ↑better / ↓better annotations
    if higher_is_better is not None:
        for i, (cat, hib) in enumerate(zip(categories, higher_is_better)):
            arrow = "↑" if hib else "↓"
            word  = "better" if hib else "better"
            ax.annotate(f"{arrow}{word}", xy=(angles[i], 1.08),
                        ha="center", va="center", fontsize=6,
                        color="#27ae60" if hib else "#c0392b",
                        fontweight="bold")


def normalize(raw_values, higher_is_better=True):
    """Min-max normalize a list to [0.05, 1.0]. Flips if lower is better."""
    arr = np.array(raw_values, dtype=float)
    lo, hi = arr.min(), arr.max()
    if hi == lo:
        return [0.5] * len(raw_values)
    if higher_is_better:
        return list(0.05 + 0.95 * (arr - lo) / (hi - lo))
    else:
        return list(0.05 + 0.95 * (hi - arr) / (hi - lo))


# ──────────────────────────────────────────────────────────
# 3.  PLOT A — Per distribution: compare cache sizes
# ──────────────────────────────────────────────────────────

def plot_per_distribution():
    """One radar per distribution — lines = cache sizes."""
    categories = [
        "Throughput\n(Mops/s)",
        "Median Latency\n(P50, ns)",
        "Tail Latency\n(P99, ns)",
        "RDMA Reads\n/ Operation",
        "RDMA Bandwidth\n/ Op (B)",
    ]
    hib = [True, False, False, False, False]  # higher-is-better per axis

    fig, axes = plt.subplots(1, 5, figsize=(26, 5.5),
                             subplot_kw=dict(polar=True))
    fig.suptitle("DEX Point Lookups — Effect of Cache Size per Distribution",
                 fontsize=14, weight="bold", y=1.02)

    # Collect ALL raw values across all configs for global normalization
    all_tp   = [point_data[(d, c)]["tp"]           for d in DISTRIBUTIONS for c in CACHES]
    all_p50  = [point_data[(d, c)]["p50"]          for d in DISTRIBUTIONS for c in CACHES]
    all_p99  = [point_data[(d, c)]["p99"]          for d in DISTRIBUTIONS for c in CACHES]
    all_rdma = [point_data[(d, c)]["rdma_read_op"] for d in DISTRIBUTIONS for c in CACHES]
    all_bw   = [point_data[(d, c)]["rdma_size_op"] for d in DISTRIBUTIONS for c in CACHES]

    for idx, (dist, dlabel) in enumerate(zip(DISTRIBUTIONS, DIST_LABELS)):
        vals = {}
        for c in CACHES:
            d = point_data[(dist, c)]
            raw = [d["tp"], d["p50"], d["p99"], d["rdma_read_op"], d["rdma_size_op"]]
            normed = []
            for rv, pool, higher in zip(raw,
                                        [all_tp, all_p50, all_p99, all_rdma, all_bw],
                                        hib):
                arr = np.array(pool, dtype=float)
                lo, hi = arr.min(), arr.max()
                if hi == lo:
                    normed.append(0.5)
                elif higher:
                    normed.append(0.05 + 0.95 * (rv - lo) / (hi - lo))
                else:
                    normed.append(0.05 + 0.95 * (hi - rv) / (hi - lo))
            vals[f"{c} MB"] = normed

        make_radar(axes[idx], categories, vals, dlabel,
                   {f"{c} MB": CACHE_COLORS[c] for c in CACHES},
                   higher_is_better=hib)

    # Shared legend
    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", ncol=3,
               fontsize=10, frameon=True, bbox_to_anchor=(0.5, -0.06))
    fig.tight_layout()
    path = os.path.join(OUT_DIR, "radar_per_distribution.png")
    fig.savefig(path, dpi=200, bbox_inches="tight")
    plt.close(fig)
    print(f"Saved: {path}")


# ──────────────────────────────────────────────────────────
# 4.  PLOT B — Per cache size: compare distributions
# ──────────────────────────────────────────────────────────

def plot_per_cache():
    """One radar per cache size — lines = distributions."""
    categories = [
        "Throughput\n(Mops/s)",
        "Median Latency\n(P50, ns)",
        "Tail Latency\n(P99, ns)",
        "RDMA Reads\n/ Operation",
        "RDMA Bandwidth\n/ Op (B)",
    ]
    hib = [True, False, False, False, False]

    fig, axes = plt.subplots(1, 3, figsize=(18, 6),
                             subplot_kw=dict(polar=True))
    fig.suptitle("DEX Point Lookups — Distribution Comparison per Cache Size",
                 fontsize=14, weight="bold", y=1.02)

    all_tp   = [point_data[(d, c)]["tp"]           for d in DISTRIBUTIONS for c in CACHES]
    all_p50  = [point_data[(d, c)]["p50"]          for d in DISTRIBUTIONS for c in CACHES]
    all_p99  = [point_data[(d, c)]["p99"]          for d in DISTRIBUTIONS for c in CACHES]
    all_rdma = [point_data[(d, c)]["rdma_read_op"] for d in DISTRIBUTIONS for c in CACHES]
    all_bw   = [point_data[(d, c)]["rdma_size_op"] for d in DISTRIBUTIONS for c in CACHES]

    for idx, c in enumerate(CACHES):
        vals = {}
        for dist, dlabel in zip(DISTRIBUTIONS, DIST_LABELS):
            d = point_data[(dist, c)]
            raw = [d["tp"], d["p50"], d["p99"], d["rdma_read_op"], d["rdma_size_op"]]
            normed = []
            for rv, pool, higher in zip(raw,
                                        [all_tp, all_p50, all_p99, all_rdma, all_bw],
                                        hib):
                arr = np.array(pool, dtype=float)
                lo, hi = arr.min(), arr.max()
                if hi == lo:
                    normed.append(0.5)
                elif higher:
                    normed.append(0.05 + 0.95 * (rv - lo) / (hi - lo))
                else:
                    normed.append(0.05 + 0.95 * (hi - rv) / (hi - lo))
            vals[dlabel] = normed

        make_radar(axes[idx], categories, vals, f"Cache = {c} MB",
                   {dl: DIST_COLORS[d] for d, dl in zip(DISTRIBUTIONS, DIST_LABELS)},
                   higher_is_better=hib)

    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", ncol=5,
               fontsize=9, frameon=True, bbox_to_anchor=(0.5, -0.08))
    fig.tight_layout()
    path = os.path.join(OUT_DIR, "radar_per_cache.png")
    fig.savefig(path, dpi=200, bbox_inches="tight")
    plt.close(fig)
    print(f"Saved: {path}")


# ──────────────────────────────────────────────────────────
# 5.  PLOT C — Point vs Range (uniform only, all 3 caches)
# ──────────────────────────────────────────────────────────

def plot_point_vs_range():
    """Radar comparing point lookups vs range queries (uniform distribution)."""
    categories = [
        "Throughput\n(Mops/s)",
        "Median Latency\n(P50, ns)",
        "Tail Latency\n(P99, ns)",
        "RDMA Reads\n/ Operation",
        "RDMA Bandwidth\n/ Op (B)",
    ]
    hib = [True, False, False, False, False]

    fig, axes = plt.subplots(1, 3, figsize=(18, 6),
                             subplot_kw=dict(polar=True))
    fig.suptitle("DEX Uniform — Point Lookups vs Range Queries by Cache Size",
                 fontsize=14, weight="bold", y=1.02)

    for idx, c in enumerate(CACHES):
        p = point_data[("uniform", c)]
        r = range_data[("uniform", c)]

        # Combine both for normalization
        raw_point = [p["tp"], p["p50"], p["p99"], p["rdma_read_op"], p["rdma_size_op"]]
        raw_range = [r["tp"], r["p50"], r["p99"], r["rdma_read_op"], r["rdma_size_op"]]

        normed_p, normed_r = [], []
        for rp, rr, higher in zip(raw_point, raw_range, hib):
            lo = min(rp, rr)
            hi = max(rp, rr)
            if hi == lo:
                normed_p.append(0.5)
                normed_r.append(0.5)
            elif higher:
                normed_p.append(0.05 + 0.95 * (rp - lo) / (hi - lo))
                normed_r.append(0.05 + 0.95 * (rr - lo) / (hi - lo))
            else:
                normed_p.append(0.05 + 0.95 * (hi - rp) / (hi - lo))
                normed_r.append(0.05 + 0.95 * (hi - rr) / (hi - lo))

        vals = {
            "Point Lookup": normed_p,
            "Range Query":  normed_r,
        }
        colors = {"Point Lookup": "#2980b9", "Range Query": "#e67e22"}
        make_radar(axes[idx], categories, vals, f"Cache = {c} MB", colors,
                   fill_alpha=0.12, higher_is_better=hib)

    handles, labels = axes[0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", ncol=2,
               fontsize=10, frameon=True, bbox_to_anchor=(0.5, -0.06))
    fig.tight_layout()
    path = os.path.join(OUT_DIR, "radar_point_vs_range.png")
    fig.savefig(path, dpi=200, bbox_inches="tight")
    plt.close(fig)
    print(f"Saved: {path}")


# ──────────────────────────────────────────────────────────
# 6.  PLOT D — Zipf θ=0.99 "hot-key" spotlight
# ──────────────────────────────────────────────────────────

def plot_zipf099_spotlight():
    """Single radar: zipf0.99 across cache sizes — shows cache amplification."""
    categories = [
        "Throughput\n(Mops/s)",
        "Median Latency\n(P50, ns)",
        "Tail Latency\n(P99, ns)",
        "RDMA Reads\n/ Operation",
        "Avg Latency\n(ns)",
    ]
    hib = [True, False, False, False, False]

    fig, ax = plt.subplots(1, 1, figsize=(7, 7), subplot_kw=dict(polar=True))
    fig.suptitle("Zipf θ=0.99 (Hot Keys) — Cache Scaling",
                 fontsize=13, weight="bold", y=1.02)

    all_raw = []
    for c in CACHES:
        d = point_data[("zipf0.99", c)]
        all_raw.append([d["tp"], d["p50"], d["p99"], d["rdma_read_op"], d["avg_lat"]])

    vals = {}
    for i, c in enumerate(CACHES):
        normed = []
        for j, higher in enumerate(hib):
            pool = [all_raw[k][j] for k in range(len(CACHES))]
            arr = np.array(pool, dtype=float)
            lo, hi = arr.min(), arr.max()
            if hi == lo:
                normed.append(0.5)
            elif higher:
                normed.append(0.05 + 0.95 * (all_raw[i][j] - lo) / (hi - lo))
            else:
                normed.append(0.05 + 0.95 * (hi - all_raw[i][j]) / (hi - lo))
        vals[f"{c} MB"] = normed

    make_radar(ax, categories, vals, "",
               {f"{c} MB": CACHE_COLORS[c] for c in CACHES},
               higher_is_better=hib)
    ax.legend(loc="upper right", bbox_to_anchor=(1.3, 1.15), fontsize=9)

    fig.tight_layout()
    path = os.path.join(OUT_DIR, "radar_zipf099_spotlight.png")
    fig.savefig(path, dpi=200, bbox_inches="tight")
    plt.close(fig)
    print(f"Saved: {path}")


# ──────────────────────────────────────────────────────────
# 7.  BAR CHARTS — Throughput & Latency summary
# ──────────────────────────────────────────────────────────

def plot_bar_summary():
    """Grouped bar charts: throughput, P50, P99 across all configs."""
    fig, axes = plt.subplots(1, 3, figsize=(20, 6))
    fig.suptitle("DEX Point Lookups — Throughput & Latency Summary",
                 fontsize=14, weight="bold")

    x = np.arange(len(DISTRIBUTIONS))
    width = 0.22

    metric_info = [
        ("tp",  "Throughput (Mops/s)", True),
        ("p50", "Median Latency P50 (μs)", False),
        ("p99", "Tail Latency P99 (μs)", False),
    ]

    for ax, (metric, ylabel, higher) in zip(axes, metric_info):
        for i, c in enumerate(CACHES):
            vals = []
            for dist in DISTRIBUTIONS:
                v = point_data[(dist, c)][metric]
                if metric in ("p50", "p99", "avg_lat"):
                    v /= 1000.0  # ns → μs
                vals.append(v)
            bars = ax.bar(x + (i - 1) * width, vals, width,
                          label=f"{c} MB", color=CACHE_COLORS[c], edgecolor="white")
            # Value labels on top
            for bar, v in zip(bars, vals):
                ax.annotate(f"{v:.1f}", xy=(bar.get_x() + bar.get_width() / 2, bar.get_height()),
                            xytext=(0, 3), textcoords="offset points",
                            ha="center", va="bottom", fontsize=6)

        ax.set_xticks(x)
        ax.set_xticklabels(DIST_LABELS, fontsize=8, rotation=15, ha="right")
        ax.set_ylabel(ylabel, fontsize=10)
        arrow = "↑ Higher = Better" if higher else "↓ Lower = Better"
        color = "#27ae60" if higher else "#c0392b"
        ax.set_title(f"{ylabel}\n({arrow})", fontsize=10, color=color)
        ax.grid(axis="y", alpha=0.3)
        ax.legend(fontsize=8)

    fig.tight_layout()
    path = os.path.join(OUT_DIR, "bar_throughput_latency.png")
    fig.savefig(path, dpi=200, bbox_inches="tight")
    plt.close(fig)
    print(f"Saved: {path}")


# ──────────────────────────────────────────────────────────
# 8.  RDMA EFFICIENCY — bar chart
# ──────────────────────────────────────────────────────────

def plot_rdma_efficiency():
    """Grouped bar chart: RDMA reads/op and bandwidth/op."""
    fig, axes = plt.subplots(1, 2, figsize=(14, 6))
    fig.suptitle("DEX Point Lookups — RDMA Efficiency (↓ Lower = Better)",
                 fontsize=14, weight="bold", color="#c0392b")

    x = np.arange(len(DISTRIBUTIONS))
    width = 0.22

    metric_info = [
        ("rdma_read_op", "RDMA Reads / Operation"),
        ("rdma_size_op", "RDMA Bandwidth / Op (Bytes)"),
    ]

    for ax, (metric, ylabel) in zip(axes, metric_info):
        for i, c in enumerate(CACHES):
            vals = [point_data[(dist, c)][metric] for dist in DISTRIBUTIONS]
            bars = ax.bar(x + (i - 1) * width, vals, width,
                          label=f"{c} MB", color=CACHE_COLORS[c], edgecolor="white")
            for bar, v in zip(bars, vals):
                ax.annotate(f"{v:.2f}", xy=(bar.get_x() + bar.get_width() / 2, bar.get_height()),
                            xytext=(0, 3), textcoords="offset points",
                            ha="center", va="bottom", fontsize=6)

        ax.set_xticks(x)
        ax.set_xticklabels(DIST_LABELS, fontsize=8, rotation=15, ha="right")
        ax.set_ylabel(ylabel, fontsize=10)
        ax.grid(axis="y", alpha=0.3)
        ax.legend(fontsize=8)

    fig.tight_layout()
    path = os.path.join(OUT_DIR, "bar_rdma_efficiency.png")
    fig.savefig(path, dpi=200, bbox_inches="tight")
    plt.close(fig)
    print(f"Saved: {path}")


# ──────────────────────────────────────────────────────────
# 8b. RADAR — Point vs Range across ALL distributions
# ──────────────────────────────────────────────────────────

def plot_point_vs_range_all():
    """Radar: point lookups vs range queries for every distribution, per cache size."""
    categories = [
        "Throughput\n(Mops/s)",
        "Median Latency\n(P50)",
        "Tail Latency\n(P99)",
        "RDMA Reads\n/ Operation",
        "RDMA Bandwidth\n/ Op (B)",
        "Avg Latency\n(ns)",
    ]
    hib = [True, False, False, False, False, False]

    fig, axes = plt.subplots(3, 5, figsize=(28, 17),
                             subplot_kw=dict(polar=True))
    fig.suptitle("DEX Point Lookups vs Range Queries — All Distributions & Cache Sizes\n"
                 "(outer polygon = better performance)",
                 fontsize=15, weight="bold", y=0.99)

    # Collect ALL raw values across point + range for GLOBAL normalisation
    all_pools = {m: [] for m in range(6)}  # index → list of raw values
    for dist in DISTRIBUTIONS:
        for c in CACHES:
            p = point_data[(dist, c)]
            r = range_data[(dist, c)]
            for vals_src in (p, r):
                all_pools[0].append(vals_src["tp"])
                all_pools[1].append(vals_src["p50"])
                all_pools[2].append(vals_src["p99"])
                all_pools[3].append(vals_src["rdma_read_op"])
                all_pools[4].append(vals_src["rdma_size_op"])
                all_pools[5].append(vals_src["avg_lat"])

    pool_arrays = {k: np.array(v, dtype=float) for k, v in all_pools.items()}

    def global_norm(raw_val, axis_idx, higher):
        arr = pool_arrays[axis_idx]
        lo, hi = arr.min(), arr.max()
        if hi == lo:
            return 0.5
        if higher:
            return 0.05 + 0.95 * (raw_val - lo) / (hi - lo)
        else:
            return 0.05 + 0.95 * (hi - raw_val) / (hi - lo)

    for row_idx, c in enumerate(CACHES):
        for col_idx, (dist, dlabel) in enumerate(zip(DISTRIBUTIONS, DIST_LABELS)):
            ax = axes[row_idx][col_idx]
            p = point_data[(dist, c)]
            r = range_data[(dist, c)]

            raw_p = [p["tp"], p["p50"], p["p99"], p["rdma_read_op"], p["rdma_size_op"], p["avg_lat"]]
            raw_r = [r["tp"], r["p50"], r["p99"], r["rdma_read_op"], r["rdma_size_op"], r["avg_lat"]]

            normed_p = [global_norm(v, i, hib[i]) for i, v in enumerate(raw_p)]
            normed_r = [global_norm(v, i, hib[i]) for i, v in enumerate(raw_r)]

            vals = {"Point Lookup": normed_p, "Range Query": normed_r}
            colors = {"Point Lookup": "#2980b9", "Range Query": "#e67e22"}
            title = f"{dlabel}\n{c} MB" if row_idx == 0 else f"{c} MB"
            make_radar(ax, categories, vals, title, colors,
                       fill_alpha=0.10,
                       higher_is_better=hib if row_idx == 0 else None)

    # Row labels on the left
    for row_idx, c in enumerate(CACHES):
        axes[row_idx][0].annotate(
            f"{c} MB", xy=(-0.35, 0.5), xycoords="axes fraction",
            fontsize=12, weight="bold", ha="center", va="center", rotation=90)

    # Shared legend
    handles, labels = axes[0][0].get_legend_handles_labels()
    fig.legend(handles, labels, loc="lower center", ncol=2,
               fontsize=11, frameon=True, bbox_to_anchor=(0.5, -0.01))
    fig.tight_layout(rect=[0.02, 0.02, 1, 0.96])
    path = os.path.join(OUT_DIR, "radar_point_vs_range_all.png")
    fig.savefig(path, dpi=200, bbox_inches="tight")
    plt.close(fig)
    print(f"Saved: {path}")


# ──────────────────────────────────────────────────────────
# 9.  RANGE QUERIES — bar chart (all distributions, 3 caches)
# ──────────────────────────────────────────────────────────

def plot_range_bars():
    """Bar chart for range queries across all distributions."""
    fig, axes = plt.subplots(1, 4, figsize=(22, 6))
    fig.suptitle("DEX Range Queries — All Distributions × Cache Sizes",
                 fontsize=14, weight="bold")

    x = np.arange(len(DISTRIBUTIONS))
    width = 0.22

    metrics = [
        ("tp",           "Throughput (Mops/s)",      True),
        ("p50",          "P50 Latency (μs)",         False),
        ("p99",          "P99 Latency (μs)",         False),
        ("rdma_read_op", "RDMA Reads / Operation",   False),
    ]

    for ax, (metric, ylabel, higher) in zip(axes, metrics):
        for i, c in enumerate(CACHES):
            vals = []
            for dist in DISTRIBUTIONS:
                v = range_data[(dist, c)][metric]
                if metric in ("p50", "p99"):
                    v /= 1000.0
                vals.append(v)
            bars = ax.bar(x + (i - 1) * width, vals, width,
                          label=f"{c} MB", color=CACHE_COLORS[c], edgecolor="white")
            for bar, v in zip(bars, vals):
                ax.annotate(f"{v:.2f}", xy=(bar.get_x() + bar.get_width() / 2, bar.get_height()),
                            xytext=(0, 3), textcoords="offset points",
                            ha="center", va="bottom", fontsize=5.5)

        ax.set_xticks(x)
        ax.set_xticklabels(DIST_LABELS, fontsize=7, rotation=15, ha="right")
        ax.set_ylabel(ylabel, fontsize=9)
        arrow = "↑ Higher = Better" if higher else "↓ Lower = Better"
        color = "#27ae60" if higher else "#c0392b"
        ax.set_title(f"{ylabel}\n({arrow})", fontsize=9, color=color)
        ax.grid(axis="y", alpha=0.3)
        ax.legend(fontsize=7)

    fig.tight_layout()
    path = os.path.join(OUT_DIR, "bar_range_queries.png")
    fig.savefig(path, dpi=200, bbox_inches="tight")
    plt.close(fig)
    print(f"Saved: {path}")


# ──────────────────────────────────────────────────────────
# 10.  HEATMAP — Throughput & RDMA reads (dist × cache)
# ──────────────────────────────────────────────────────────

def plot_heatmaps():
    """2-panel heatmap: throughput and RDMA reads/op."""
    fig, axes = plt.subplots(1, 2, figsize=(14, 5))
    fig.suptitle("DEX Point Lookups — Heatmaps", fontsize=14, weight="bold")

    for ax, (metric, title, cmap, fmt) in zip(axes, [
        ("tp",           "Throughput (Mops/s) — ↑ Higher = Better",  "YlGn",    ".2f"),
        ("rdma_read_op", "RDMA Reads/Op — ↓ Lower = Better",        "YlOrRd_r", ".3f"),
    ]):
        matrix = []
        for dist in DISTRIBUTIONS:
            row = [point_data[(dist, c)][metric] for c in CACHES]
            matrix.append(row)
        matrix = np.array(matrix)

        im = ax.imshow(matrix, cmap=cmap, aspect="auto")
        ax.set_xticks(range(len(CACHES)))
        ax.set_xticklabels([f"{c} MB" for c in CACHES], fontsize=9)
        ax.set_yticks(range(len(DISTRIBUTIONS)))
        ax.set_yticklabels(DIST_LABELS, fontsize=9)
        ax.set_xlabel("Cache Size")
        ax.set_title(title, fontsize=10, pad=10)

        for i in range(len(DISTRIBUTIONS)):
            for j in range(len(CACHES)):
                ax.text(j, i, format(matrix[i, j], fmt),
                        ha="center", va="center", fontsize=10, fontweight="bold",
                        color="white" if matrix[i, j] > matrix.mean() else "black")
        fig.colorbar(im, ax=ax, shrink=0.8)

    fig.tight_layout()
    path = os.path.join(OUT_DIR, "heatmap_tp_rdma.png")
    fig.savefig(path, dpi=200, bbox_inches="tight")
    plt.close(fig)
    print(f"Saved: {path}")


# ──────────────────────────────────────────────────────────
# 11.  LATENCY DISTRIBUTION — grouped P50/P90/P95/P99/P99.9
# ──────────────────────────────────────────────────────────

def plot_latency_breakdown():
    """Stacked-style latency percentile comparison for each cache size."""
    fig, axes = plt.subplots(1, 3, figsize=(20, 6))
    fig.suptitle("DEX Point Lookups — Latency Percentile Breakdown (↓ Lower = Better)",
                 fontsize=14, weight="bold", color="#c0392b")

    percentiles = ["p50", "p99", "p999"]
    pct_labels  = ["P50", "P99", "P99.9"]
    pct_colors  = ["#3498db", "#e67e22", "#e74c3c"]

    for ax, c in zip(axes, CACHES):
        x = np.arange(len(DISTRIBUTIONS))
        width = 0.25
        for i, (pct, plabel, pcol) in enumerate(zip(percentiles, pct_labels, pct_colors)):
            vals = [point_data[(d, c)][pct] / 1000.0 for d in DISTRIBUTIONS]  # ns → μs
            ax.bar(x + (i - 1) * width, vals, width, label=plabel, color=pcol,
                   edgecolor="white", alpha=0.85)

        ax.set_xticks(x)
        ax.set_xticklabels(DIST_LABELS, fontsize=7, rotation=20, ha="right")
        ax.set_ylabel("Latency (μs)")
        ax.set_title(f"Cache = {c} MB", fontsize=11)
        ax.grid(axis="y", alpha=0.3)
        ax.legend(fontsize=8)

    fig.tight_layout()
    path = os.path.join(OUT_DIR, "bar_latency_percentiles.png")
    fig.savefig(path, dpi=200, bbox_inches="tight")
    plt.close(fig)
    print(f"Saved: {path}")


# ──────────────────────────────────────────────────────────
# 12.  SUMMARY TABLE (printed to console)
# ──────────────────────────────────────────────────────────

def print_summary_table():
    """Pretty-print all results to console."""
    print("\n" + "=" * 100)
    print("  DEX EXPERIMENT RESULTS — POINT LOOKUPS")
    print("=" * 100)
    header = f"{'Distribution':<12} {'Cache':>6} {'Throughput':>12} {'Avg Lat':>10} {'P50':>8} {'P99':>8} {'P99.9':>8} {'RDMA R/op':>10} {'RDMA B/op':>10}"
    print(header)
    print("-" * 100)
    for dist in DISTRIBUTIONS:
        for c in CACHES:
            d = point_data[(dist, c)]
            print(f"{dist:<12} {c:>4} MB {d['tp']:>10.3f}  {d['avg_lat']/1000:>8.1f}μs {d['p50']/1000:>6.1f}μs {d['p99']/1000:>6.1f}μs {d['p999']/1000:>6.1f}μs {d['rdma_read_op']:>10.3f} {d['rdma_size_op']:>9.1f}B")
        print()

    print("\n" + "=" * 100)
    print("  DEX EXPERIMENT RESULTS — RANGE QUERIES")
    print("=" * 100)
    print(header)
    print("-" * 100)
    for dist in DISTRIBUTIONS:
        for c in CACHES:
            d = range_data[(dist, c)]
            print(f"{dist:<12} {c:>4} MB {d['tp']:>10.3f}  {d['avg_lat']/1000:>8.1f}μs {d['p50']/1000:>6.1f}μs {d['p99']/1000:>6.1f}μs {d['p999']/1000:>6.1f}μs {d['rdma_read_op']:>10.3f} {d['rdma_size_op']:>9.1f}B")
        print()

    print("\n" + "=" * 100)
    print("  KEY OBSERVATIONS")
    print("=" * 100)

    # Best / worst configs
    best_tp = max(point_data.items(), key=lambda x: x[1]["tp"])
    worst_tp = min(point_data.items(), key=lambda x: x[1]["tp"])
    best_lat = min(point_data.items(), key=lambda x: x[1]["p50"])
    worst_lat = max(point_data.items(), key=lambda x: x[1]["p50"])

    print(f"  Highest throughput : {best_tp[1]['tp']:.3f} Mops/s  ({best_tp[0][0]}, {best_tp[0][1]} MB)")
    print(f"  Lowest throughput  : {worst_tp[1]['tp']:.3f} Mops/s  ({worst_tp[0][0]}, {worst_tp[0][1]} MB)")
    print(f"  Best P50 latency   : {best_lat[1]['p50']/1000:.1f} μs      ({best_lat[0][0]}, {best_lat[0][1]} MB)")
    print(f"  Worst P50 latency  : {worst_lat[1]['p50']/1000:.1f} μs      ({worst_lat[0][0]}, {worst_lat[0][1]} MB)")

    # Cache scaling factor
    for dist in DISTRIBUTIONS:
        tp_128 = point_data[(dist, 128)]["tp"]
        tp_512 = point_data[(dist, 512)]["tp"]
        print(f"  {dist:<10} 128→512 MB speedup: {tp_512/tp_128:.2f}x")

    print(f"\n  Range queries are ~{point_data[('uniform', 256)]['tp'] / range_data[('uniform', 256)]['tp']:.1f}x slower than point lookups (uniform, 256 MB)")
    print(f"  Range queries use ~{range_data[('uniform', 256)]['rdma_read_op'] / point_data[('uniform', 256)]['rdma_read_op']:.1f}x more RDMA reads per operation")
    print("=" * 100)

    # Range scaling
    for dist in DISTRIBUTIONS:
        tp_128 = range_data[(dist, 128)]["tp"]
        tp_512 = range_data[(dist, 512)]["tp"]
        print(f"  Range {dist:<10} 128→512 MB speedup: {tp_512/tp_128:.2f}x")
    print()


# ──────────────────────────────────────────────────────────
# MAIN
# ──────────────────────────────────────────────────────────

if __name__ == "__main__":
    print_summary_table()
    plot_per_distribution()
    plot_per_cache()
    plot_point_vs_range()
    plot_zipf099_spotlight()
    plot_bar_summary()
    plot_rdma_efficiency()
    plot_point_vs_range_all()
    plot_range_bars()
    plot_heatmaps()
    plot_latency_breakdown()
    print(f"\nAll plots saved to: {OUT_DIR}/")
