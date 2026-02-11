#!/usr/bin/env python3
"""
QW1 Zipfian Skew Sweep — Analysis Plots
DEX vs CHIME: Point-read & range-scan latency, throughput across skew levels.
"""

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
import os

# ── Data ────────────────────────────────────────────────────────────────────
skew_labels = ['Uniform', 'Zipf 0.6', 'Zipf 0.8', 'Zipf 0.9', 'Zipf 0.99']
x = np.arange(len(skew_labels))

# All values in microseconds
dex_read  = dict(p50=[0.50, 0.50, 0.50, 0.50, 0.50],
                 p90=[4.50, 4.50, 4.00, 3.50, 3.00],
                 p99=[5.50, 5.50, 5.50, 5.50, 5.00],
                 p999=[6.00, 6.00, 6.00, 6.00, 6.00])

dex_range = dict(p50=[6.50, 6.50, 5.50, 3.50, 2.00],
                 p90=[13.00, 13.00, 12.00, 11.50, 10.00],
                 p99=[18.00, 18.50, 18.00, 17.50, 17.00],
                 p999=[22.00, 22.50, 22.00, 22.00, 21.50])

chime_read  = dict(p50=[7.50, 7.50, 7.50, 7.00, 6.50],
                   p90=[12.50, 12.00, 11.00, 9.50, 9.00],
                   p99=[30.50, 29.00, 26.50, 23.00, 19.00],
                   p999=[77.50, 77.50, 68.50, 55.50, 34.00])

chime_range = dict(p50=[337.0, 326.5, 299.0, 241.5, 163.0],
                   p90=[467.0, 461.0, 441.5, 387.5, 326.5],
                   p99=[586.0, 581.5, 562.0, 497.0, 428.0],
                   p999=[676.5, 672.0, 652.5, 580.5, 503.0])

# Throughput in Mops/s
dex_tput   = [4.430, 4.527, 5.201, 6.152, 7.678]
chime_tput = [0.1176, 0.1171, 0.1272, 0.1540, 0.1903]

outdir = os.path.join(os.path.dirname(__file__), 'plots')
os.makedirs(outdir, exist_ok=True)

# ── Style ───────────────────────────────────────────────────────────────────
plt.rcParams.update({
    'font.size': 12, 'axes.labelsize': 13, 'axes.titlesize': 14,
    'legend.fontsize': 10, 'figure.dpi': 150
})
DEX_COLOR   = '#2166ac'
CHIME_COLOR = '#b2182b'
BAR_W = 0.35

# ═════════════════════════════════════════════════════════════════════════════
# PLOT 1: Point-Read Latency vs Skew (P50, P99, P99.9 grouped bars)
# ═════════════════════════════════════════════════════════════════════════════
fig, axes = plt.subplots(1, 3, figsize=(16, 5), sharey=False)
for ax, pct, label in zip(axes, ['p50', 'p99', 'p999'], ['P50', 'P99', 'P99.9']):
    bars1 = ax.bar(x - BAR_W/2, dex_read[pct],  BAR_W, label='DEX',  color=DEX_COLOR, edgecolor='white')
    bars2 = ax.bar(x + BAR_W/2, chime_read[pct], BAR_W, label='CHIME', color=CHIME_COLOR, edgecolor='white')
    ax.set_xticks(x); ax.set_xticklabels(skew_labels, rotation=25, ha='right')
    ax.set_ylabel('Latency (us)')
    ax.set_title(f'Point-Read {label}')
    ax.legend()
    # annotate ratio on top of CHIME bars
    for i, (d, c) in enumerate(zip(dex_read[pct], chime_read[pct])):
        ax.annotate(f'{c/d:.0f}x', xy=(x[i]+BAR_W/2, c), ha='center', va='bottom', fontsize=8, color=CHIME_COLOR)
fig.suptitle('QW1: Point-Read Latency — DEX vs CHIME', fontsize=15, y=1.02)
fig.tight_layout()
fig.savefig(os.path.join(outdir, 'qw1_read_latency.png'), bbox_inches='tight')
print(f"Saved qw1_read_latency.png")

# ═════════════════════════════════════════════════════════════════════════════
# PLOT 2: Range-Scan Latency vs Skew (P50, P99, P99.9 grouped bars)
# ═════════════════════════════════════════════════════════════════════════════
fig, axes = plt.subplots(1, 3, figsize=(16, 5), sharey=False)
for ax, pct, label in zip(axes, ['p50', 'p99', 'p999'], ['P50', 'P99', 'P99.9']):
    bars1 = ax.bar(x - BAR_W/2, dex_range[pct],  BAR_W, label='DEX',  color=DEX_COLOR, edgecolor='white')
    bars2 = ax.bar(x + BAR_W/2, chime_range[pct], BAR_W, label='CHIME', color=CHIME_COLOR, edgecolor='white')
    ax.set_xticks(x); ax.set_xticklabels(skew_labels, rotation=25, ha='right')
    ax.set_ylabel('Latency (us)')
    ax.set_title(f'Range-Scan {label}')
    ax.legend()
    for i, (d, c) in enumerate(zip(dex_range[pct], chime_range[pct])):
        ax.annotate(f'{c/d:.0f}x', xy=(x[i]+BAR_W/2, c), ha='center', va='bottom', fontsize=8, color=CHIME_COLOR)
fig.suptitle('QW1: Range-Scan Latency — DEX vs CHIME', fontsize=15, y=1.02)
fig.tight_layout()
fig.savefig(os.path.join(outdir, 'qw1_range_latency.png'), bbox_inches='tight')
print(f"Saved qw1_range_latency.png")

# ═════════════════════════════════════════════════════════════════════════════
# PLOT 3: Throughput vs Skew (line chart)
# ═════════════════════════════════════════════════════════════════════════════
fig, ax = plt.subplots(figsize=(8, 5))
ax.plot(x, dex_tput,   'o-', color=DEX_COLOR,   linewidth=2, markersize=8, label='DEX')
ax.plot(x, chime_tput, 's-', color=CHIME_COLOR,  linewidth=2, markersize=8, label='CHIME')
ax.set_xticks(x); ax.set_xticklabels(skew_labels)
ax.set_ylabel('Throughput (Mops/s)')
ax.set_xlabel('Skew')
ax.set_title('QW1: Aggregate Throughput — DEX vs CHIME')
ax.legend()
ax.grid(axis='y', alpha=0.3)
# secondary y-axis for ratio
ax2 = ax.twinx()
ratios = [d/c for d, c in zip(dex_tput, chime_tput)]
ax2.plot(x, ratios, '--', color='gray', linewidth=1.5, alpha=0.6)
ax2.set_ylabel('DEX / CHIME ratio', color='gray')
ax2.tick_params(axis='y', labelcolor='gray')
fig.tight_layout()
fig.savefig(os.path.join(outdir, 'qw1_throughput.png'), bbox_inches='tight')
print(f"Saved qw1_throughput.png")

# ═════════════════════════════════════════════════════════════════════════════
# PLOT 4: Tail-Latency Comparison (P50 → P99.9 at extreme skew points)
# ═════════════════════════════════════════════════════════════════════════════
fig, axes = plt.subplots(1, 2, figsize=(14, 5))
percentiles = ['P50', 'P90', 'P99', 'P99.9']
pct_x = np.arange(len(percentiles))

# Left: Uniform (worst case)
for ax, skew_idx, title in zip(axes, [0, 4], ['Uniform (worst case)', 'Zipf 0.99 (best case)']):
    read_dex  = [dex_read['p50'][skew_idx], dex_read['p90'][skew_idx], dex_read['p99'][skew_idx], dex_read['p999'][skew_idx]]
    read_chime = [chime_read['p50'][skew_idx], chime_read['p90'][skew_idx], chime_read['p99'][skew_idx], chime_read['p999'][skew_idx]]
    range_dex  = [dex_range['p50'][skew_idx], dex_range['p90'][skew_idx], dex_range['p99'][skew_idx], dex_range['p999'][skew_idx]]
    range_chime = [chime_range['p50'][skew_idx], chime_range['p90'][skew_idx], chime_range['p99'][skew_idx], chime_range['p999'][skew_idx]]

    ax.semilogy(pct_x, read_dex,   'o-',  color=DEX_COLOR,   label='DEX Read')
    ax.semilogy(pct_x, read_chime, 'o--', color=CHIME_COLOR,  label='CHIME Read')
    ax.semilogy(pct_x, range_dex,  's-',  color=DEX_COLOR,   alpha=0.6, label='DEX Range')
    ax.semilogy(pct_x, range_chime,'s--', color=CHIME_COLOR,  alpha=0.6, label='CHIME Range')
    ax.set_xticks(pct_x); ax.set_xticklabels(percentiles)
    ax.set_ylabel('Latency (us, log)')
    ax.set_title(title)
    ax.legend(fontsize=8)
    ax.grid(axis='y', alpha=0.3, which='both')

fig.suptitle('QW1: Tail-Latency Profiles at Extreme Skew Points', fontsize=15, y=1.02)
fig.tight_layout()
fig.savefig(os.path.join(outdir, 'qw1_tail_latency.png'), bbox_inches='tight')
print(f"Saved qw1_tail_latency.png")

print("\nAll 4 plots saved to:", outdir)
