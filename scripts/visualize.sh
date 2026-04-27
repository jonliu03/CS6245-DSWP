#!/usr/bin/env bash
# Render every reports/<bench>/*.dot to results/figures/analyzer/<bench>/*.svg.
# Mirrors the per-benchmark subdirectory layout produced by run_analyzer.sh.
set -euo pipefail
PROJ_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT_ROOT="$PROJ_ROOT/results/figures/analyzer"
REPORTS="$PROJ_ROOT/reports"

# Walk reports/<bench>/*.dot. Top-level reports/*.dot (if any from older
# runs) get rendered into figures/analyzer/ directly for backward compat.
shopt -s nullglob

for d in "$REPORTS"/*.dot; do
  out="$OUT_ROOT/$(basename "${d%.dot}").svg"
  mkdir -p "$OUT_ROOT"
  dot -Tsvg "$d" -o "$out"
  echo "rendered: $(basename "$d") -> analyzer/$(basename "$out")"
done

for bench_dir in "$REPORTS"/*/; do
  bench="$(basename "$bench_dir")"
  for d in "$bench_dir"*.dot; do
    out_dir="$OUT_ROOT/$bench"
    mkdir -p "$out_dir"
    out="$out_dir/$(basename "${d%.dot}").svg"
    dot -Tsvg "$d" -o "$out"
    echo "rendered: $bench/$(basename "$d") -> analyzer/$bench/$(basename "$out")"
  done
done
