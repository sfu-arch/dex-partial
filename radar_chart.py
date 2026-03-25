"""
Radar chart comparing DEX vs DART across seven dimensions.
Latency is treated as a first-class metric alongside throughput.

Axes (higher = better on all):

[0] Peak P50 latency      (best median, when conditions are ideal)
    DEX  P50=0.5μs at 256MB cache, zipf=0.99 (cache hit → 0 RDMA)    → 0.95
    DART P50≈17μs always (4.5 RDMA RTTs, irreducible)                 → 0.08

[1] Tail predictability   (1 - P99/P50 normalised; higher = tighter tail)
    DEX  P99/P50 = 6.5μs/0.5μs = 13x at warm cache; 22.5/16.5=1.36x at cold → 0.22
         (bimodal: fast cache hits vs slow full-traversal misses → fat tail)
    DART P99/P50 ≈ 19μs/17μs = 1.12x always (fixed-path, no bimodal)        → 0.88

[2] High-cache throughput (256MB cache, uniform, 1KB pages, 50M keys)
    DEX  7.22 Mops/s                                                   → 0.90
    DART 1.67 Mops/s                                                   → 0.21

[3] Low-cache throughput  (4MB cache, 256B pages, height=11, uniform)
    DEX  1.814 Mops/s  (barely above crossover; P50=16.5μs, P99=22.5μs) → 0.48
    DART 1.67 Mops/s   (same as always; P50=17μs, P99=19μs)             → 0.44

[4] Range scan support    (mechanism: B+-tree leaf chaining vs hash ART)
    DEX  full ordered range scans, efficient leaf chaining             → 0.90
    DART hash ART, no ordered range scan support                       → 0.05

[5] Small memory footprint (client-side cache needed to be functional)
    DEX  ≥32MB needed (256MB for full performance); 5–500MB range      → 0.15
    DART ~1MB RACE directory; fully functional regardless of cache     → 0.95

[6] Skew benefit          (zipf=0.99 throughput gain over uniform)
    DEX  7.22 → 11.33 Mops/s = +57% gain (hot pages stay cached)      → 0.85
    DART 1.67 → 1.67 Mops/s  = 0% gain  (no data cache)               → 0.10

Key crossover insight (measured, 50M keys, rpc=0, admit=0.1):
  - 32MB cache, 1KB pages:    DEX 4.90 vs DART 1.67 → DEX 2.9x faster
  - 32MB cache, 256B pages:   DEX 2.68 vs DART 1.67 → DEX 1.6x faster
  - 4MB  cache, 256B pages:   DEX 1.81 vs DART 1.67 → DEX barely faster
  - ~2MB cache, 256B pages:   DEX ≈ DART (crossover point)
  - But: even at crossover, DEX P99=22.5μs > DART P99≈19μs
    → DART has better tail latency even when throughput is equal

Output: radar_chart.pdf / radar_chart.png
"""

import numpy as np
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

# ---------------------------------------------------------------------------
# Data
# ---------------------------------------------------------------------------
AXES = [
    "Peak P50\nlatency",
    "Tail\npredictability",
    "High-cache\nthroughput",
    "Low-cache\nthroughput",
    "Range scan\nsupport",
    "Small memory\nfootprint",
    "Skew\nbenefit",
]
N = len(AXES)

SYSTEMS = {
    #        P50   Tail   HiTP   LoTP   Rng    Mem    Skew
    "DEX":  [0.95, 0.22,  0.90,  0.48,  0.90,  0.15,  0.85],
    "DART": [0.08, 0.88,  0.21,  0.44,  0.05,  0.95,  0.10],
}

COLORS = {
    "DEX":  "#4472C4",
    "DART": "#ED7D31",
}

# ---------------------------------------------------------------------------
# Plot
# ---------------------------------------------------------------------------
fig = plt.figure(figsize=(8.5, 10.0), facecolor="#F7F3EE")
ax = fig.add_subplot(111, polar=True)
ax.set_facecolor("#F7F3EE")

ax.set_theta_zero_location("N")
ax.set_theta_direction(-1)

angles = np.linspace(0, 2 * np.pi, N, endpoint=False)
angles_closed = np.append(angles, angles[0])

# grid rings
for level in [0.2, 0.4, 0.6, 0.8, 1.0]:
    ax.plot(angles_closed, [level] * (N + 1),
            color="#CCCCCC", linewidth=0.7, linestyle="-", zorder=1)
for angle in angles:
    ax.plot([angle, angle], [0, 1],
            color="#CCCCCC", linewidth=0.7, zorder=1)

# filled regions — DART first so DEX sits on top
for name in ["DART", "DEX"]:
    vals = SYSTEMS[name]
    ax.fill(angles_closed, vals + [vals[0]],
            color=COLORS[name], alpha=0.15, zorder=2)

# outlines + markers
for name, vals in SYSTEMS.items():
    color = COLORS[name]
    ax.plot(angles_closed, vals + [vals[0]],
            color=color, linewidth=2.2, zorder=4)
    ax.scatter(angles, vals, color=color, s=36, zorder=5, clip_on=False)

# ---------------------------------------------------------------------------
# Axis labels
# ---------------------------------------------------------------------------
ax.set_xticks(angles)
ax.set_xticklabels([])

label_pad = 1.15
for angle, label in zip(angles, AXES):
    x_unit = np.sin(angle)
    y_unit = np.cos(angle)
    ha = "left" if x_unit > 0.1 else ("right" if x_unit < -0.1 else "center")
    va = "bottom" if y_unit > 0.1 else ("top" if y_unit < -0.1 else "center")
    ax.text(angle, label_pad, label,
            ha=ha, va=va, fontsize=11.5, color="#333333",
            transform=ax.transData)

ax.set_yticklabels([])
ax.set_ylim(0, 1)
ax.spines["polar"].set_visible(False)
ax.grid(False)

# ---------------------------------------------------------------------------
# Score annotations
# ---------------------------------------------------------------------------
for name, vals in SYSTEMS.items():
    color = COLORS[name]
    offset = {"DEX": 0.11, "DART": -0.14}[name]
    for angle, val in zip(angles, vals):
        r_label = val + offset if val > 0.1 else val + 0.08
        ax.text(angle, r_label, f"{val:.2f}",
                ha="center", va="center",
                fontsize=7.5, color=color,
                transform=ax.transData)

# ---------------------------------------------------------------------------
# Crossover callout annotation on "Low-cache throughput" axis
# ---------------------------------------------------------------------------
cross_angle = angles[3]  # Low-cache throughput
ax.annotate("~equal\nthroughput\nbut DART\nwins tail",
            xy=(cross_angle, 0.46),
            xytext=(cross_angle + 0.75, 0.68),
            fontsize=7, color="#666666",
            arrowprops=dict(arrowstyle="->", color="#999999", lw=0.8),
            transform=ax.transData)

# ---------------------------------------------------------------------------
# Legend
# ---------------------------------------------------------------------------
legend_handles = [
    mpatches.Patch(facecolor=COLORS[s], edgecolor=COLORS[s],
                   linewidth=1.5, label=s)
    for s in SYSTEMS
]
ax.legend(handles=legend_handles,
          loc="lower center",
          bbox_to_anchor=(0.5, -0.20),
          ncol=2, frameon=False, fontsize=13,
          handlelength=1.2, handletextpad=0.5, columnspacing=1.5)

# Title + measurement note
fig.text(0.5, 0.97, "DEX vs DART — Performance Profile",
         ha="center", va="top", fontsize=14, color="#222222", weight="bold")
fig.text(0.5, 0.93, "50M keys · 30 threads · rpc=0 · admit=0.1 · InfiniBand RDMA",
         ha="center", va="top", fontsize=9.5, color="#555555")
fig.text(0.5, 0.015,
         "Higher = better on all axes.  Latency axes: P50 and P99/P50 ratio from measured data.",
         ha="center", va="center", fontsize=9, color="#666666", style="italic")

plt.subplots_adjust(left=0.05, right=0.95, top=0.91, bottom=0.20)

out_base = "radar_chart"
plt.savefig(f"{out_base}.pdf", bbox_inches="tight", dpi=150)
plt.savefig(f"{out_base}.png", bbox_inches="tight", dpi=150)
print(f"Saved {out_base}.pdf and {out_base}.png")
plt.close()
