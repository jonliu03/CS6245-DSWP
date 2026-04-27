#!/usr/bin/env python3
"""Plot speedup vs stage count from results/stage_sweep.csv.

Reads:  results/stage_sweep.csv (stages,n,seq_s,dswp_s,speedup)
Writes: results/figures/stage_sweep.png
"""

import csv
import sys
from collections import defaultdict
from pathlib import Path

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    import numpy as np
except ImportError:
    print("matplotlib not installed", file=sys.stderr); sys.exit(1)

ROOT = Path(__file__).resolve().parent.parent
CSV  = ROOT / "results" / "stage_sweep.csv"
OUT  = ROOT / "results" / "figures" / "stage_sweep.png"
OUT.parent.mkdir(parents=True, exist_ok=True)

if not CSV.exists():
    print(f"no sweep data at {CSV}; run scripts/run_stage_sweep.sh first",
          file=sys.stderr)
    sys.exit(1)

# By-n: stages → speedup.
by_n = defaultdict(dict)
seq_times_by_n = {}
dswp_times_by_n = defaultdict(dict)
with CSV.open() as f:
    for row in csv.DictReader(f):
        n   = int(row["n"])
        s   = int(row["stages"])
        sp  = float(row["speedup"])
        by_n[n][s] = sp
        seq_times_by_n[n] = float(row["seq_s"])
        dswp_times_by_n[n][s] = float(row["dswp_s"])

ns = sorted(by_n.keys())
all_stages = sorted({s for d in by_n.values() for s in d.keys()})

# ── Figure: 2 panels — speedup vs stages, runtime vs stages ───────
fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(12, 5))

for n in ns:
    xs = sorted(by_n[n].keys())
    ys = [by_n[n][s] for s in xs]
    ax1.plot(xs, ys, marker="o", linewidth=2, label=f"n = {n:,}")
ax1.axhline(1.0, color="black", linestyle="--", linewidth=1, alpha=0.4,
            label="1.0× parity")
# Ideal linear-speedup reference up to physical core count.
ax1.plot(all_stages, all_stages, color="grey", linestyle=":", alpha=0.5,
         label="ideal linear")
ax1.set_xlabel("DSWP stages (DSWP_NUM_STAGES)")
ax1.set_ylabel("speedup (seq / DSWP)")
ax1.set_title("llist_heavy — speedup vs stage count")
ax1.set_xticks(all_stages)
ax1.legend()
ax1.grid(alpha=0.3)

for n in ns:
    xs = sorted(dswp_times_by_n[n].keys())
    ys = [dswp_times_by_n[n][s] for s in xs]
    ax2.plot(xs, ys, marker="s", linewidth=2, label=f"DSWP n = {n:,}")
    ax2.axhline(seq_times_by_n[n], color=ax2.lines[-1].get_color(),
                linestyle="--", alpha=0.5,
                label=f"seq n = {n:,}")
ax2.set_xlabel("DSWP stages")
ax2.set_ylabel("runtime (s)")
ax2.set_yscale("log")
ax2.set_title("llist_heavy — runtime vs stage count")
ax2.set_xticks(all_stages)
ax2.legend(fontsize=8)
ax2.grid(alpha=0.3, which="both")

fig.tight_layout()
fig.savefig(OUT, dpi=140)
print(f"wrote: {OUT}")
