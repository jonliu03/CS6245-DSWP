#!/usr/bin/env bash
# Run the analyzer on every benchmark in benchmarks/.
set -euo pipefail
PROJ_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

for src in "$PROJ_ROOT"/benchmarks/*.c; do
  name="$(basename "${src%.c}")"
  echo "================================================================"
  echo "Benchmark: $name"
  echo "================================================================"
  bash "$PROJ_ROOT/scripts/run_analyzer.sh" "$name"
done

echo
echo "Rendering DOT files..."
bash "$PROJ_ROOT/scripts/visualize.sh"
