#!/usr/bin/env python3
"""Plot stage sweep from the ICE/cluster run (single n, more stages).

Reads:  results/stage_sweep_ice.csv (stages,n,seq_s,dswp_s,speedup,samples)
Writes: results/figures/stage_sweep_ice.png
"""

import csv
import sys
from pathlib import Path

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError:
    print("matplotlib not installed", file=sys.stderr); sys.exit(1)

ROOT = Path(__file__).resolve().parent.parent
CSV  = ROOT / "results" / "stage_sweep_ice.csv"
OUT  = ROOT / "results" / "figures" / "stage_sweep_ice.png"
OUT.parent.mkdir(parents=True, exist_ok=True)

if not CSV.exists():
    print(f"no sweep data at {CSV}", file=sys.stderr); sys.exit(1)

stages, seq_s, dswp_s, speedup = [], [], [], []
n_size = None
with CSV.open() as f:
    for row in csv.DictReader(f):
        stages.append(int(row["stages"]))
        seq_s.append(float(row["seq_s"]))
        dswp_s.append(float(row["dswp_s"]))
        speedup.append(float(row["speedup"]))
        n_size = int(row["n"])

if not stages:
    print("CSV has no usable rows", file=sys.stderr); sys.exit(1)

stages = np.array(stages); speedup = np.array(speedup)
seq_s = np.array(seq_s); dswp_s = np.array(dswp_s)

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(13, 5.5))

# ── Speedup vs N ──
ax1.plot(stages, speedup, marker="o", linewidth=2.5, markersize=8,
         color="#377eb8", label="measured speedup")
ax1.plot(stages, stages, color="grey", linestyle=":", linewidth=1.5,
         alpha=0.7, label="ideal linear (N×)")
ax1.axhline(1.0, color="black", linestyle="--", linewidth=1, alpha=0.4,
            label="1.0× parity")
for x, y in zip(stages, speedup):
    ax1.annotate(f"{y:.2f}×", (x, y), textcoords="offset points",
                 xytext=(0, 10), ha="center", fontsize=9)
ax1.set_xlabel("DSWP stages (DSWP_NUM_STAGES)")
ax1.set_ylabel("speedup (seq / DSWP)")
ax1.set_title(f"llist_heavy_12 — speedup vs stage count (n={n_size:,})")
ax1.set_xticks(stages)
ax1.legend(loc="upper left")
ax1.grid(alpha=0.3)

# ── Runtime vs N ──
ax2.plot(stages, dswp_s, marker="s", linewidth=2.5, markersize=8,
         color="#377eb8", label="DSWP runtime")
ax2.axhline(seq_s[0], color="#888", linestyle="--", linewidth=1.5,
            label=f"sequential baseline ({seq_s[0]:.3f}s)")
ax2.set_xlabel("DSWP stages")
ax2.set_ylabel("runtime (s)")
ax2.set_title(f"llist_heavy_12 — runtime vs stage count (n={n_size:,})")
ax2.set_xticks(stages)
ax2.legend()
ax2.grid(alpha=0.3)

fig.tight_layout()
fig.savefig(OUT, dpi=150)
print(f"wrote: {OUT}")
