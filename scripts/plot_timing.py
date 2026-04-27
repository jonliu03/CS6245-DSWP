#!/usr/bin/env python3
# Run via the system python (matplotlib is installed for /usr/bin/python3 on this box):
#   /usr/bin/python3 scripts/plot_timing.py
"""Generate runtime + speedup plots from results/timing.csv.

Reads:  results/timing.csv  (columns: benchmark,n,seq_s,dswp_s,speedup)
Writes: results/figures/runtime.png
        results/figures/speedup.png
"""

import csv
import os
import sys
from collections import defaultdict
from pathlib import Path

try:
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
except ImportError:
    print("matplotlib not installed; pip install matplotlib", file=sys.stderr)
    sys.exit(1)

ROOT = Path(__file__).resolve().parent.parent
CSV  = ROOT / "results" / "timing.csv"
OUT  = ROOT / "results" / "figures"
OUT.mkdir(parents=True, exist_ok=True)

if not CSV.exists():
    print(f"no timing.csv at {CSV}; run `bash scripts/demo.sh --transform` first",
          file=sys.stderr)
    sys.exit(1)

# Group rows by benchmark.
rows = defaultdict(list)  # bench -> [(n, seq_s, dswp_s, speedup), ...]
with CSV.open() as f:
    for row in csv.DictReader(f):
        try:
            n        = int(row["n"])
            seq_s    = float(row["seq_s"])
            dswp_s   = float(row["dswp_s"])
            speedup  = float(row["speedup"])
        except (KeyError, ValueError):
            continue
        rows[row["benchmark"]].append((n, seq_s, dswp_s, speedup))

if not rows:
    print("timing.csv has no usable rows", file=sys.stderr)
    sys.exit(1)

benches = sorted(rows.keys())

# Use the largest n per benchmark for the headline plots.
def largest_n_row(bench):
    return max(rows[bench], key=lambda r: r[0])

# ── Runtime plot: side-by-side bars, seq vs DSWP, log scale ──────────
fig, ax = plt.subplots(figsize=(10, 5))
import numpy as np
x = np.arange(len(benches))
w = 0.38
seq_t  = [largest_n_row(b)[1] for b in benches]
dswp_t = [largest_n_row(b)[2] for b in benches]
ax.bar(x - w/2, seq_t,  w, label="sequential (-O3)",      color="#888888")
ax.bar(x + w/2, dswp_t, w, label="DSWP (cloned + runtime)", color="#377eb8")
ax.set_xticks(x)
labels = [f"{b}\n(n={largest_n_row(b)[0]:,})" for b in benches]
ax.set_xticklabels(labels, rotation=15, ha="right")
ax.set_ylabel("runtime (s)")
ax.set_yscale("log")
ax.set_title("DSWP vs sequential — runtime at largest n")
ax.legend()
ax.grid(axis="y", alpha=0.3, which="both")
fig.tight_layout()
fig.savefig(OUT / "runtime.png", dpi=140)
plt.close(fig)

# ── Speedup plot: bar at largest n, with 1.0x reference line ─────────
fig, ax = plt.subplots(figsize=(10, 4.5))
sp = [largest_n_row(b)[3] for b in benches]
colors = ["#4daf4a" if s >= 1.0 else "#e41a1c" for s in sp]
xpos = np.arange(len(benches))
bars = ax.bar(xpos, sp, color=colors)
ax.axhline(1.0, color="black", linewidth=1, linestyle="--", alpha=0.6,
           label="1.0× (parity)")
for b, s in zip(bars, sp):
    ax.text(b.get_x() + b.get_width() / 2, b.get_height(), f"{s:.2f}×",
            ha="center", va="bottom", fontsize=9)
ax.set_ylabel("speedup (seq / DSWP)")
ax.set_title("DSWP speedup at largest n")
ax.set_xticks(xpos)
ax.set_xticklabels(benches, rotation=15, ha="right")
ax.legend()
ax.grid(axis="y", alpha=0.3)
fig.tight_layout()
fig.savefig(OUT / "speedup.png", dpi=140)
plt.close(fig)

# ── Scaling plot: runtime vs n per benchmark ─────────────────────────
fig, ax = plt.subplots(figsize=(10, 5))
for b in benches:
    pts = sorted(rows[b])
    ns = [r[0] for r in pts]
    seq = [r[1] for r in pts]
    dswp = [r[2] for r in pts]
    ax.plot(ns, seq,  "--", marker="o", label=f"{b} seq",  alpha=0.7)
    ax.plot(ns, dswp, "-",  marker="s", label=f"{b} DSWP", alpha=0.9)
ax.set_xscale("log")
ax.set_yscale("log")
ax.set_xlabel("n")
ax.set_ylabel("runtime (s)")
ax.set_title("Runtime scaling — sequential vs DSWP")
ax.legend(fontsize=8, ncol=2)
ax.grid(alpha=0.3, which="both")
fig.tight_layout()
fig.savefig(OUT / "runtime_scaling.png", dpi=140)
plt.close(fig)

print(f"wrote: {OUT/'runtime.png'}")
print(f"wrote: {OUT/'speedup.png'}")
print(f"wrote: {OUT/'runtime_scaling.png'}")
