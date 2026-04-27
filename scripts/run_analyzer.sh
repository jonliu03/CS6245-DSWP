#!/usr/bin/env bash
# Compile a benchmark to LLVM bitcode and run the DSWPAnalyzer pass on it.
set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "usage: $0 BENCHMARK_NAME (e.g. llist_sum)"
  exit 1
fi

BENCH="$1"
PROJ_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LLVM_PREFIX="${LLVM_PREFIX:-$(brew --prefix llvm@15)}"
SRC="$PROJ_ROOT/benchmarks/$BENCH.c"
BUILD_DIR="$PROJ_ROOT/build"
BC="$BUILD_DIR/$BENCH.bc"

if [[ ! -f "$SRC" ]]; then
  echo "no benchmark source: $SRC"
  exit 1
fi

# Plugin (Darwin uses .dylib, Linux uses .so).
PLUGIN=""
for ext in dylib so; do
  if [[ -f "$BUILD_DIR/pass/DSWPAnalyzer.$ext" ]]; then
    PLUGIN="$BUILD_DIR/pass/DSWPAnalyzer.$ext"
    break
  fi
done
if [[ -z "$PLUGIN" ]]; then
  echo "Plugin not built. Run scripts/build.sh first."
  exit 1
fi

mkdir -p "$BUILD_DIR"

# Compile to bitcode. -O1 + mem2reg gives recognizable loops without
# aggressive transformations that hide the structure we care about.
"$LLVM_PREFIX/bin/clang" \
  -O1 -Xclang -disable-llvm-passes \
  -emit-llvm -c -fno-unroll-loops \
  "$SRC" -o "$BC.raw"

# Apply mem2reg + simplifycfg + loop-simplify to canonicalize loops.
"$LLVM_PREFIX/bin/opt" \
  -passes='mem2reg,simplifycfg,loop-simplify,lcssa' \
  "$BC.raw" -o "$BC"

# Per-benchmark output subdirectory keeps reports/ tidy when many
# benchmarks have functions with overlapping names (e.g. build_list).
REPORT_DIR="$PROJ_ROOT/reports/$BENCH"
mkdir -p "$REPORT_DIR"

# Run the analyzer pass.
echo
echo "=== running dswp-analyze on $BENCH ==="
"$LLVM_PREFIX/bin/opt" \
  -load-pass-plugin="$PLUGIN" \
  -passes='dswp-analyze' \
  -dswp-report-dir="$REPORT_DIR" \
  -disable-output \
  "$BC"
