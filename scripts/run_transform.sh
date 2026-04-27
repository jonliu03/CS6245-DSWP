#!/usr/bin/env bash
# Compile a benchmark, apply the DSWPTransform pass, link with the runtime,
# run, and compare against the sequential -O3 baseline.
#
# Usage:
#   bash scripts/run_transform.sh BENCHMARK_NAME
#
# Env overrides:
#   SIZES="10000 100000 1000000"   override the per-bench default n's
#   DSWP_NUM_STAGES=N              ask the pass for N stages (default 2)
#
# Output:
#   - Human-readable progress on stdout
#   - One machine-readable line per n:  "## RESULT n=N seq=ST dswp=DT status=OK"
#     (consumed by demo.sh's transform wrapper to render the colored table
#     and append to results/timing.csv — keeps the transform pipeline in one
#     place.)
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

bash "$PROJ_ROOT/scripts/build.sh"

if [[ ! -f "$SRC" ]]; then
  echo "no benchmark source: $SRC" >&2
  exit 1
fi

PLUGIN="$BUILD_DIR/pass/DSWPTransform.so"
if [[ ! -f "$PLUGIN" ]]; then
  echo "DSWPTransform plugin not built." >&2
  exit 1
fi

# Per-benchmark default sizes. SIZES env var (if set) wins.
DEFAULT_SIZES=""
case "$BENCH" in
  llist_sum|llist_compute|max_list|llist_heavy)
    DEFAULT_SIZES="10000 100000 1000000" ;;
  llist_heavy_12|markov_chain_12)
    DEFAULT_SIZES="10000 100000 1000000" ;;
  histogram|fib_iter)
    DEFAULT_SIZES="1000000 10000000 100000000" ;;
  *)
    echo "unknown benchmark '$BENCH' (no default sizes configured)" >&2
    exit 1
    ;;
esac
SIZES="${SIZES:-$DEFAULT_SIZES}"

RUNTIME_LIB="$BUILD_DIR/runtime/libdswp_runtime.a"
if [[ ! -f "$RUNTIME_LIB" ]]; then
  echo "Runtime library missing: $RUNTIME_LIB" >&2
  exit 1
fi

BC_RAW="$BUILD_DIR/${BENCH}_pre.bc"
BC_CANON="$BUILD_DIR/${BENCH}_canon.bc"
BC_DSWP="$BUILD_DIR/${BENCH}_dswp.bc"
LL_DSWP="$BUILD_DIR/${BENCH}_dswp.ll"
EXE="$BUILD_DIR/${BENCH}_dswp"
SEQ_BIN="$BUILD_DIR/benchmarks/${BENCH}_seq"

# 1. Compile to bitcode WITHOUT running the optimizer.
"$LLVM_PREFIX/bin/clang" \
  -O1 -Xclang -disable-llvm-passes \
  -emit-llvm -c -fno-unroll-loops \
  "$SRC" -o "$BC_RAW"

# 2. Canonicalize loops (mem2reg + simplifycfg + loop-simplify + lcssa).
"$LLVM_PREFIX/bin/opt" \
  -passes='mem2reg,simplifycfg,loop-simplify,lcssa' \
  "$BC_RAW" -o "$BC_CANON"

# 3. Apply the DSWPTransform. Passes through DSWP_NUM_STAGES env if set.
"$LLVM_PREFIX/bin/opt" \
  -load-pass-plugin="$PLUGIN" \
  -passes='dswp-transform' \
  "$BC_CANON" -o "$BC_DSWP"

# Human-readable IR for inspection.
"$LLVM_PREFIX/bin/llvm-dis" "$BC_DSWP" -o "$LL_DSWP"

# 4. Link with runtime + pthread + math (stages are in the bitcode itself).
#    -O2 so the per-stage compute (sin/cos/etc) gets the same optimization
#    budget as the -O3 sequential baseline.
"$LLVM_PREFIX/bin/clang++" -O2 \
  "$BC_DSWP" \
  "$RUNTIME_LIB" \
  -lpthread -lm \
  -o "$EXE"

echo
echo "=== Verifying $BENCH (DSWP-transformed) ==="
for n in $SIZES; do
  echo "------------------------------------------------------------"
  echo "n=$n"
  seq_out=$("$SEQ_BIN" "$n")
  dswp_out=$("$EXE" "$n")
  echo "  seq:  $seq_out"
  echo "  dswp: $dswp_out"

  seq_val=$(echo "$seq_out"  | sed 's/ *(.*//')
  dswp_val=$(echo "$dswp_out" | sed 's/ *(.*//')
  st=$(echo "$seq_out"  | grep -oE '[0-9]+\.[0-9]+ s' | head -1 | grep -oE '[0-9]+\.[0-9]+' || echo "")
  dt=$(echo "$dswp_out" | grep -oE '[0-9]+\.[0-9]+ s' | head -1 | grep -oE '[0-9]+\.[0-9]+' || echo "")
  status="OK"
  if [[ "$seq_val" != "$dswp_val" ]]; then
    echo "  MISMATCH (seq='$seq_val' dswp='$dswp_val')" >&2
    status="MISMATCH"
  else
    echo "  OK"
  fi
  # Machine-readable summary line — demo.sh's wrapper greps for "## RESULT".
  echo "## RESULT n=$n seq=${st:--} dswp=${dt:--} status=$status"
  if [[ "$status" == "MISMATCH" ]]; then exit 1; fi
done

echo
echo "Transformed IR written to: $LL_DSWP"
