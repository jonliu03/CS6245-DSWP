#!/usr/bin/env python3
"""Plot stage sweep results from PACE ICE.

Reads:
  results/stage_sweep_ice_<bench>.csv  — one CSV per benchmark
                                         (cols: stages,n,seq_s,dswp_s,
                                                speedup,samples)

Backward-compat: if results/stage_sweep_ice.csv exists alone (older
naming), it's treated as the llist_heavy_12 series.

Writes:
  results/figures/stage_sweep_ice.png  — overlay of all available series
"""

import csv
import sys
from pathlib import Path
from collections import defaultdict

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError:
    print("matplotlib not installed", file=sys.stderr); sys.exit(1)

ROOT = Path(__file__).resolve().parent.parent
RESULTS = ROOT / "results"
OUT = RESULTS / "figures" / "stage_sweep_ice.png"
OUT.parent.mkdir(parents=True, exist_ok=True)

# Each benchmark gets a (heavy_count, color, divisor_set) entry. The
# divisor set determines which N values get a "balanced partition"
# highlight ring on the plot.
BENCH_META = {
    "llist_heavy":     dict(heavies=5,  color="#e67e22", divisors={2, 3, 4, 5}),
    "llist_heavy_12":  dict(heavies=12, color="#377eb8", divisors={2, 3, 4, 6, 12}),
    "markov_chain_12": dict(heavies=12, color="#2ca02c", divisors={2, 3, 4, 6, 12}),
}

# ─── Discover available CSVs ────────────────────────────────────────
series = []  # (bench_name, stages, dswp_s, speedup, seq_baseline, n_size)
csv_files = sorted(RESULTS.glob("stage_sweep_ice_*.csv"))
# Backward-compat: include legacy name too if no per-bench CSVs found.
legacy = RESULTS / "stage_sweep_ice.csv"
if not csv_files and legacy.exists():
    csv_files = [legacy]

if not csv_files:
    print("no stage_sweep_ice*.csv found in results/", file=sys.stderr)
    sys.exit(1)

for f in csv_files:
    if f.name == "stage_sweep_ice.csv":
        bench = "llist_heavy_12"        # legacy name → map to 12-heavy
    else:
        bench = f.stem.replace("stage_sweep_ice_", "")
    stages, dswp, speedup, seq_s = [], [], [], None
    n_size = None
    with f.open() as fh:
        for row in csv.DictReader(fh):
            stages.append(int(row["stages"]))
            dswp.append(float(row["dswp_s"]))
            speedup.append(float(row["speedup"]))
            seq_s = float(row["seq_s"])
            n_size = int(row["n"])
    if stages:
        series.append((bench, np.array(stages), np.array(dswp),
                        np.array(speedup), seq_s, n_size))

# ─── Plot ────────────────────────────────────────────────────────────
fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(15, 6))

all_x = sorted({int(s) for _, ss, _, _, _, _ in series for s in ss})

for bench, stages, dswp, speedup, seq_s, n_size in series:
    meta = BENCH_META.get(bench, dict(heavies=None, color="#888", divisors=set()))
    color = meta["color"]
    divisors = meta["divisors"]
    heavies = meta["heavies"]

    label = f"{bench}"
    if heavies is not None:
        label += f"  ({heavies} heavy calls)"

    # ── Speedup panel ──
    ax1.plot(stages, speedup, marker="o", linewidth=2.3, markersize=7,
             color=color, label=label, zorder=3)
    # Highlight ring on balanced-partition stages.
    div_x = [s for s in stages if s in divisors]
    div_y = [speedup[i] for i, s in enumerate(stages) if s in divisors]
    ax1.scatter(div_x, div_y, s=170, facecolors="none",
                edgecolors=color, linewidth=2, alpha=0.9, zorder=4)
    # Number annotations.
    for x, y in zip(stages, speedup):
        ax1.annotate(f"{y:.1f}", (x, y), textcoords="offset points",
                     xytext=(0, 9), ha="center", fontsize=8, color=color)
    # Peak callout.
    pk = int(np.argmax(speedup))
    ax1.annotate(
        f"peak @ N={stages[pk]}: {speedup[pk]:.2f}×",
        xy=(stages[pk], speedup[pk]),
        xytext=(stages[pk] - 2.5, speedup[pk] + 1.5),
        fontsize=9, color=color, weight="bold",
        arrowprops=dict(arrowstyle="->", color=color, lw=1.3))

    # ── Runtime panel ──
    ax2.plot(stages, dswp, marker="s", linewidth=2.3, markersize=7,
             color=color, label=f"{bench} DSWP", zorder=3)
    ax2.axhline(seq_s, color=color, linestyle="--", linewidth=1.0,
                alpha=0.5, label=f"{bench} seq ({seq_s:.2f}s)")

# Reference lines on speedup panel.
ax1.plot(all_x, all_x, color="grey", linestyle=":", linewidth=1.3,
         alpha=0.6, label="ideal linear (N×)")
ax1.axhline(1.0, color="black", linestyle="--", linewidth=1, alpha=0.4,
            label="1.0× parity")

# Peak symbol legend entry.
ax1.scatter([], [], s=170, facecolors="none", edgecolors="#444",
            linewidth=2, label="N divides heavy count evenly")

ax1.set_xlabel("DSWP stages (DSWP_NUM_STAGES)")
ax1.set_ylabel("speedup (seq / DSWP)")
n_text = ", ".join(f"n={s[5]:,}" for s in series[:1])  # just first
ax1.set_title(f"DSWP speedup vs stage count — PACE ICE (Gold 6226), {n_text}, median of 5 runs")
ax1.set_xticks(all_x)
peak_max = max(float(np.max(s[3])) for s in series)
ax1.set_ylim(0, max(max(all_x), peak_max + 2.5))
ax1.legend(loc="upper left", fontsize=9)
ax1.grid(alpha=0.3)

ax2.set_xlabel("DSWP stages")
ax2.set_ylabel("runtime (s)")
ax2.set_title(f"DSWP runtime vs stage count — {n_text}")
ax2.set_xticks(all_x)
ax2.legend(fontsize=9)
ax2.grid(alpha=0.3)

fig.tight_layout()
fig.savefig(OUT, dpi=150)
print(f"wrote: {OUT}")
print(f"series plotted: {[s[0] for s in series]}")
