#!/usr/bin/env python3
"""Print a markdown summary table aggregating all reports/*.json files."""
import glob
import json
import os
import sys

PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPORTS = os.path.join(PROJ, "reports")


def main():
    rows = []
    for path in sorted(glob.glob(os.path.join(REPORTS, "*.json"))):
        with open(path) as f:
            r = json.load(f)
        rows.append({
            "function": r["function"],
            "loop": r["loop_idx"],
            "nodes": r["pdg"]["nodes"],
            "edges": r["pdg"]["edges"],
            "sccs": r["sccs"]["count"],
            "cyclic": r["sccs"]["cyclic"],
            "single": r["partition"]["single_stage"],
            "total": r["partition"]["total_cost"],
            "s0":    r["partition"]["stage_0_cost"],
            "s1":    r["partition"]["stage_1_cost"],
            "cross": r["partition"]["cross_stage_edges"],
            "upper": r["partition"]["est_speedup_upper_bound"],
        })

    if not rows:
        print("No JSON reports found in reports/.", file=sys.stderr)
        return 1

    cols = [
        ("function",  "{:<22}"),
        ("loop",      "{:>4}"),
        ("nodes",     "{:>5}"),
        ("edges",     "{:>5}"),
        ("sccs",      "{:>4}"),
        ("cyclic",    "{:>6}"),
        ("total",     "{:>5}"),
        ("s0",        "{:>4}"),
        ("s1",        "{:>4}"),
        ("cross",     "{:>5}"),
        ("upper",     "{:>5.2f}x"),
    ]

    headers = [c[0] for c in cols]
    fmts = [c[1] for c in cols]

    # Header row.
    print("| " + " | ".join(h.ljust(max(len(h), 4)) for h in headers) + " |")
    print("|" + "|".join("-" * (max(len(h), 4) + 2) for h in headers) + "|")
    for r in rows:
        cells = []
        for h, fmt in zip(headers, fmts):
            v = r[h]
            try:
                cells.append(fmt.format(v))
            except Exception:
                cells.append(str(v))
        print("| " + " | ".join(cells) + " |")

    return 0


if __name__ == "__main__":
    sys.exit(main())
