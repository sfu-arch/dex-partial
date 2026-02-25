#!/usr/bin/env python3
"""
Plot DEX B+ Tree Performance: 20M vs 100M Keys
Tree Height Crossover Experiment
"""

import matplotlib
matplotlib.use('Agg')  # Non-interactive backend
import matplotlib.pyplot as plt
import numpy as np

# Data from experiments
workloads = ['20M Keys\n(Height 5)', '100M Keys\n(Height 6)']
keys_millions = [20, 100]

# Latency data (nanoseconds)
avg_latency = [4647.53, 5441.61]
p50_latency = [4500, 5500]
p90_latency = [6500, 7000]
p95_latency = [7000, 7500]
p99_latency = [8500, 9000]
p999_latency = [11500, 11500]

# Throughput data (Mops/s)
throughput = [5.895, 5.134]

# Tree metrics
tree_height = [5, 6]
leaf_nodes = [689655, 3448275]
leaf_size_mb = [673.49, 3367.46]

# Create figure with multiple subplots
fig, axes = plt.subplots(2, 2, figsize=(12, 10))
fig.suptitle('DEX B+ Tree Performance: Tree Height Crossover Analysis\n(Cache Size: 64MB, Page Size: 1024B, Leaf Cardinality: 58)', 
             fontsize=14, fontweight='bold')

# Plot 1: Latency Percentiles Line Plot
ax1 = axes[0, 0]
percentiles = ['Avg', 'P50', 'P90', 'P95', 'P99', 'P99.9']
latency_20m = [4647.53, 4500, 6500, 7000, 8500, 11500]
latency_100m = [5441.61, 5500, 7000, 7500, 9000, 11500]

x = np.arange(len(percentiles))
ax1.plot(x, latency_20m, 'b-o', label='20M Keys (Height 5)', linewidth=2, markersize=8)
ax1.plot(x, latency_100m, 'r-s', label='100M Keys (Height 6)', linewidth=2, markersize=8)
ax1.set_xlabel('Percentile', fontsize=11)
ax1.set_ylabel('Latency (ns)', fontsize=11)
ax1.set_title('Read Latency Distribution', fontsize=12)
ax1.set_xticks(x)
ax1.set_xticklabels(percentiles)
ax1.legend(loc='upper left')
ax1.grid(True, alpha=0.3)
ax1.set_ylim(0, 13000)

# Add value annotations
for i, (v1, v2) in enumerate(zip(latency_20m, latency_100m)):
    ax1.annotate(f'{v1:.0f}', (i, v1), textcoords="offset points", 
                 xytext=(0, 10), ha='center', fontsize=8, color='blue')
    ax1.annotate(f'{v2:.0f}', (i, v2), textcoords="offset points", 
                 xytext=(0, -15), ha='center', fontsize=8, color='red')

# Plot 2: Throughput Comparison
ax2 = axes[0, 1]
colors = ['steelblue', 'coral']
bars = ax2.bar(workloads, throughput, color=colors, edgecolor='black', linewidth=1.5)
ax2.set_ylabel('Throughput (Mops/s)', fontsize=11)
ax2.set_title('Cluster Throughput', fontsize=12)
ax2.set_ylim(0, 7)
ax2.grid(True, alpha=0.3, axis='y')

# Add value labels on bars
for bar, val in zip(bars, throughput):
    ax2.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 0.1, 
             f'{val:.3f}', ha='center', va='bottom', fontsize=11, fontweight='bold')

# Add percentage difference
diff_pct = ((throughput[1] - throughput[0]) / throughput[0]) * 100
ax2.annotate(f'{diff_pct:.1f}%', 
             xy=(0.5, (throughput[0] + throughput[1])/2), 
             fontsize=12, color='darkred', fontweight='bold',
             ha='center')

# Plot 3: Average Latency with Error Bars (showing P50-P99 range)
ax3 = axes[1, 0]
x_pos = [0, 1]
yerr_lower = [abs(avg_latency[0] - p50_latency[0]), abs(avg_latency[1] - p50_latency[1])]
yerr_upper = [abs(p99_latency[0] - avg_latency[0]), abs(p99_latency[1] - avg_latency[1])]

bars = ax3.bar(x_pos, avg_latency, yerr=[yerr_lower, yerr_upper], 
               capsize=10, color=colors, edgecolor='black', linewidth=1.5,
               error_kw={'linewidth': 2})
ax3.set_xticks(x_pos)
ax3.set_xticklabels(workloads)
ax3.set_ylabel('Latency (ns)', fontsize=11)
ax3.set_title('Average Latency (with P50-P99 range)', fontsize=12)
ax3.set_ylim(0, 11000)
ax3.grid(True, alpha=0.3, axis='y')

# Add value labels
for bar, val in zip(bars, avg_latency):
    ax3.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 600, 
             f'{val:.1f} ns', ha='center', va='bottom', fontsize=10, fontweight='bold')

# Add percentage difference
lat_diff_pct = ((avg_latency[1] - avg_latency[0]) / avg_latency[0]) * 100
ax3.annotate(f'+{lat_diff_pct:.1f}%', 
             xy=(0.5, (avg_latency[0] + avg_latency[1])/2 + 500), 
             fontsize=12, color='darkred', fontweight='bold',
             ha='center')

# Plot 4: Tree Structure Comparison
ax4 = axes[1, 1]
metrics = ['Tree Height', 'Leaf Nodes\n(x100K)', 'Data Size\n(GB)']
values_20m = [5, 6.89655, 0.69754]
values_100m = [6, 34.48275, 3.48772]

x = np.arange(len(metrics))
width = 0.35

bars1 = ax4.bar(x - width/2, values_20m, width, label='20M Keys', color='steelblue', edgecolor='black')
bars2 = ax4.bar(x + width/2, values_100m, width, label='100M Keys', color='coral', edgecolor='black')

ax4.set_ylabel('Value', fontsize=11)
ax4.set_title('Tree Structure Metrics', fontsize=12)
ax4.set_xticks(x)
ax4.set_xticklabels(metrics)
ax4.legend()
ax4.grid(True, alpha=0.3, axis='y')

# Add value labels
for bar in bars1:
    height = bar.get_height()
    ax4.text(bar.get_x() + bar.get_width()/2, height + 0.2, 
             f'{height:.2f}', ha='center', va='bottom', fontsize=9)
for bar in bars2:
    height = bar.get_height()
    ax4.text(bar.get_x() + bar.get_width()/2, height + 0.2, 
             f'{height:.2f}', ha='center', va='bottom', fontsize=9)

plt.tight_layout(rect=[0, 0.03, 1, 0.95])
plt.savefig('dex_20m_vs_100m_comparison.png', dpi=150, bbox_inches='tight')
plt.savefig('dex_20m_vs_100m_comparison.pdf', bbox_inches='tight')
print("Plots saved: dex_20m_vs_100m_comparison.png, dex_20m_vs_100m_comparison.pdf")

# Create additional line plot for latency vs key count scaling
fig2, ax = plt.subplots(figsize=(10, 6))

# Latency scaling with key count
key_counts = [20, 100]
avg_latencies = [4.648, 5.442]  # microseconds
p99_latencies = [8.5, 9.0]

ax.plot(key_counts, avg_latencies, 'b-o', label='Average Latency', linewidth=2.5, markersize=12)
ax.plot(key_counts, p99_latencies, 'r--s', label='P99 Latency', linewidth=2.5, markersize=12)

ax.set_xlabel('Number of Keys (Millions)', fontsize=12)
ax.set_ylabel('Latency (microseconds)', fontsize=12)
ax.set_title('DEX Read Latency Scaling with Key Count\n(64MB Cache, 1024B Pages, 58 Entries/Leaf)', 
             fontsize=14, fontweight='bold')
ax.legend(fontsize=11)
ax.grid(True, alpha=0.3)
ax.set_xlim(0, 120)
ax.set_ylim(0, 12)

# Add annotations for tree height
ax.annotate('Height = 5', (20, avg_latencies[0]), textcoords="offset points", 
            xytext=(10, 20), ha='left', fontsize=11,
            arrowprops=dict(arrowstyle='->', color='gray'))
ax.annotate('Height = 6', (100, avg_latencies[1]), textcoords="offset points", 
            xytext=(10, 20), ha='left', fontsize=11,
            arrowprops=dict(arrowstyle='->', color='gray'))

# Add latency difference annotation
ax.annotate(f'+17.1% latency\n(+1 tree level)', 
            xy=(60, (avg_latencies[0] + avg_latencies[1])/2), 
            fontsize=11, color='darkgreen', fontweight='bold',
            ha='center', va='center',
            bbox=dict(boxstyle='round', facecolor='lightyellow', edgecolor='gray'))

plt.tight_layout()
plt.savefig('dex_latency_scaling.png', dpi=150, bbox_inches='tight')
plt.savefig('dex_latency_scaling.pdf', bbox_inches='tight')
print("Plots saved: dex_latency_scaling.png, dex_latency_scaling.pdf")

# plt.show()  # Commented out for non-interactive execution
print("All plots generated successfully!")
