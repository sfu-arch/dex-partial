"""
Radar chart comparing DEX vs DART across seven crossover-relevant dimensions.

Axes (higher = better on all):
  High-cache throughput, Low-cache throughput, Latency stability,
  Range scan support, Skew benefit, Small memory footprint, RTT efficiency

Scores derived from DEX_DART_CROSSOVER.md analysis and benchmark expectations:

[0] High-cache throughput  (256MB cache, uniform, 30 threads)
    DEX  ~7 Mops/s   → score 0.90   (cache hit rate high, RDMA avoided)
    DART ~1.67 Mops/s → score 0.21  (always 2 RTTs, no caching)

[1] Low-cache throughput   (32MB cache, uniform, 30 threads)
    DEX  ~0.8 Mops/s → score 0.20   (3-4 RDMA RTTs per lookup, thrashing)
    DART ~1.67 Mops/s → score 0.85  (same 2 RTTs regardless of "cache")

[2] Latency stability      (P99/P50 ratio, lower ratio = more stable = higher score)
    DEX  P99 can be 10-50x P50 under cache pressure → score 0.20
    DART P99 ≈ 2x P50 always                        → score 0.85

[3] Range scan support     (mechanism-based: B+-tree vs hash)
    DEX  full B+-tree range scans, efficient leaf chaining → score 0.90
    DART hash table, no range scan support                  → score 0.00

[4] Skew benefit           (zipf=0.99 speedup over uniform)
    DEX  hot pages stay cached → 1.5-2x throughput gain    → score 0.85
    DART hash scatter, no locality benefit, flat            → score 0.10

[5] Small memory footprint (working set size, log-normalised, smaller = better)
    DEX  256MB cache (default)  → score 0.15
    DART ~1MB local directory   → score 0.95

[6] RTT efficiency         (min RTTs per lookup when conditions are best)
    DEX  1 RTT when root+path fully cached                  → score 0.90
    DART always 2 RTTs (bucket + leaf, no way to skip)      → score 0.40

Output: dex_dart_radar.pdf / dex_dart_radar.png
"""

import numpy as np
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches

# ── Data ─────────────────────────────────────────────────────────────────────
AXES = [
    "High-cache\nthroughput",
    "Low-cache\nthroughput",
    "Latency\nstability",
    "Range scan\nsupport",
    "Skew\nbenefit",
    "Small memory\nfootprint",
    "RTT\nefficiency",
]
N = len(AXES)

# Scores in [0, 1], higher = better on every axis.
SYSTEMS = {
    #          HiTP   LoTP   Stab   Rng    Skew   Mem    RTT
    "DEX":  [0.90,  0.20,  0.20,  0.90,  0.85,  0.15,  0.90],
    "DART": [0.21,  0.85,  0.85,  0.00,  0.10,  0.95,  0.40],
}

COLORS = {
    "DEX":  "#4472C4",   # blue
    "DART": "#ED7D31",   # orange
}

# ── Plot ──────────────────────────────────────────────────────────────────────
fig = plt.figure(figsize=(8, 9.5), facecolor="#F7F3EE")
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

# spoke lines
for angle in angles:
    ax.plot([angle, angle], [0, 1],
            color="#CCCCCC", linewidth=0.7, zorder=1)

# filled regions (draw both, DEX on top so neither is hidden)
for name, vals in SYSTEMS.items():
    ax.fill(angles_closed, vals + [vals[0]],
            color=COLORS[name], alpha=0.15, zorder=2)

# outlines + markers
for name, vals in SYSTEMS.items():
    color = COLORS[name]
    ax.plot(angles_closed, vals + [vals[0]],
            color=color, linewidth=2.2, zorder=4)
    ax.scatter(angles, vals,
               color=color, s=36,
               zorder=5, clip_on=False)

# ── Axis labels ───────────────────────────────────────────────────────────────
ax.set_xticks(angles)
ax.set_xticklabels([])

label_pad = 1.13
for angle, label in zip(angles, AXES):
    x_unit = np.sin(angle)
    y_unit = np.cos(angle)

    ha = "left" if x_unit > 0.1 else ("right" if x_unit < -0.1 else "center")
    va = "bottom" if y_unit > 0.1 else ("top" if y_unit < -0.1 else "center")

    ax.text(angle, label_pad, label,
            ha=ha, va=va,
            fontsize=12, color="#333333",
            transform=ax.transData)

ax.set_yticklabels([])
ax.set_ylim(0, 1)
ax.spines["polar"].set_visible(False)
ax.grid(False)

# ── Score annotations (values at each vertex) ─────────────────────────────────
for name, vals in SYSTEMS.items():
    color = COLORS[name]
    for angle, val in zip(angles, vals):
        offset = 0.11
        x_unit = np.sin(angle)
        y_unit = np.cos(angle)
        # nudge label slightly away from center to avoid overlap
        r_label = val + offset if val > 0.05 else val + 0.06
        ax.text(angle, r_label, f"{val:.2f}",
                ha="center", va="center",
                fontsize=7.5, color=color,
                transform=ax.transData)

# ── Legend ────────────────────────────────────────────────────────────────────
legend_handles = [
    mpatches.Patch(facecolor=COLORS[s], edgecolor=COLORS[s],
                   linewidth=1.5, label=s)
    for s in SYSTEMS
]
ax.legend(
    handles=legend_handles,
    loc="lower center",
    bbox_to_anchor=(0.5, -0.18),
    ncol=2,
    frameon=False,
    fontsize=13,
    handlelength=1.2,
    handletextpad=0.5,
    columnspacing=1.5,
)

# title + subtitle
fig.text(0.5, 0.97,
         "DEX vs DART — Performance Profile",
         ha="center", va="top",
         fontsize=14, color="#222222", weight="bold")

fig.text(0.5, 0.02,
         "Higher = better on all axes.  Scores derived from DEX_DART_CROSSOVER.md analysis.",
         ha="center", va="center",
         fontsize=9.5, color="#666666", style="italic")

plt.subplots_adjust(left=0.05, right=0.95, top=0.93, bottom=0.18)

# ── Save ──────────────────────────────────────────────────────────────────────
out_base = "dex_dart_radar"
plt.savefig(f"{out_base}.pdf", bbox_inches="tight", dpi=150)
plt.savefig(f"{out_base}.png", bbox_inches="tight", dpi=150)
print(f"Saved {out_base}.pdf and {out_base}.png")
plt.close()
