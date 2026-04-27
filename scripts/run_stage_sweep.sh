#!/usr/bin/env bash
# Run llist_heavy across a range of DSWP stage counts and write
# results/stage_sweep.csv (stages,n,seq_s,dswp_s,speedup). The cost
# table model and the cloned transform handle the partition automatically;
# the only thing the user controls is N via DSWP_NUM_STAGES.
#
# Output:
#   results/stage_sweep.csv
#   results/figures/stage_sweep.png  (via plot_stage_sweep.py)

set -euo pipefail
PROJ_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LLVM_PREFIX="${LLVM_PREFIX:-$(brew --prefix llvm@15)}"
BUILD_DIR="$PROJ_ROOT/build"
RESULTS="$PROJ_ROOT/results"
CSV="$RESULTS/stage_sweep.csv"

# Default: stages 1..5, three problem sizes. Override via env.
STAGES_LIST="${STAGES_LIST:-1 2 3 4 5}"
SIZES="${SIZES:-100000 1000000}"
BENCH="llist_heavy"

PLUGIN="$BUILD_DIR/pass/DSWPTransform.so"
RUNTIME_LIB="$BUILD_DIR/runtime/libdswp_runtime.a"
SRC="$PROJ_ROOT/benchmarks/$BENCH.c"
SEQ_BIN="$BUILD_DIR/benchmarks/${BENCH}_seq"

bash "$PROJ_ROOT/scripts/build.sh" >/dev/null

mkdir -p "$RESULTS"
echo "stages,n,seq_s,dswp_s,speedup" > "$CSV"

for N in $STAGES_LIST; do
  echo "=========================================================="
  echo "N=$N stages"
  echo "=========================================================="

  if [[ "$N" -eq 1 ]]; then
    # Sequential baseline. We still record it so the plot has a 1-stage point.
    EXE="$SEQ_BIN"
  else
    BC_RAW="$BUILD_DIR/${BENCH}_n${N}_pre.bc"
    BC_CANON="$BUILD_DIR/${BENCH}_n${N}_canon.bc"
    BC_DSWP="$BUILD_DIR/${BENCH}_n${N}_dswp.bc"
    EXE="$BUILD_DIR/${BENCH}_n${N}_dswp"

    "$LLVM_PREFIX/bin/clang" -O1 -Xclang -disable-llvm-passes \
      -emit-llvm -c -fno-unroll-loops "$SRC" -o "$BC_RAW"

    "$LLVM_PREFIX/bin/opt" \
      -passes='mem2reg,simplifycfg,loop-simplify,lcssa' \
      "$BC_RAW" -o "$BC_CANON"

    DSWP_NUM_STAGES="$N" "$LLVM_PREFIX/bin/opt" \
      -load-pass-plugin="$PLUGIN" \
      -passes='dswp-transform' \
      "$BC_CANON" -o "$BC_DSWP"

    "$LLVM_PREFIX/bin/clang++" -O2 \
      "$BC_DSWP" "$RUNTIME_LIB" -lpthread -lm -o "$EXE"
  fi

  for n in $SIZES; do
    seq_out=$("$SEQ_BIN" "$n")
    dswp_out=$("$EXE" "$n")
    seq_val=$(echo "$seq_out"  | sed 's/ *(.*//')
    dswp_val=$(echo "$dswp_out" | sed 's/ *(.*//')
    if [[ "$seq_val" != "$dswp_val" ]]; then
      echo "MISMATCH at n=$n stages=$N: seq='$seq_val' dswp='$dswp_val'" >&2
      exit 1
    fi
    st=$(echo "$seq_out"  | grep -oE '[0-9]+\.[0-9]+ s' | head -1 | grep -oE '[0-9]+\.[0-9]+')
    dt=$(echo "$dswp_out" | grep -oE '[0-9]+\.[0-9]+ s' | head -1 | grep -oE '[0-9]+\.[0-9]+')
    sp=$(/usr/bin/python3 -c "print(f'{${st}/${dt}:.3f}')")
    printf "  n=%-10s seq=%ss dswp=%ss speedup=%sx\n" "$n" "$st" "$dt" "$sp"
    echo "${N},${n},${st},${dt},${sp}" >> "$CSV"
  done
done

echo
echo "Wrote $CSV"

# Plot.
PY=""
if /usr/bin/python3 -c "import matplotlib" 2>/dev/null; then PY=/usr/bin/python3
elif python3 -c "import matplotlib" 2>/dev/null; then PY=python3
fi
if [[ -n "$PY" ]]; then
  "$PY" "$PROJ_ROOT/scripts/plot_stage_sweep.py"
else
  echo "(matplotlib not installed — skipping plot)"
fi
