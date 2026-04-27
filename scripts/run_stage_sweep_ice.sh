#!/usr/bin/env bash
# DSWP stage sweep designed for PACE ICE compute nodes (or any
# many-core Linux box). Sweeps llist_heavy_12 (12 chained heavy calls)
# across N = 1..12 stages, takes the median of several samples per
# stage count, writes results/stage_sweep_ice.csv, and renders the
# plot to results/figures/stage_sweep_ice.png.
#
# Usage on PACE ICE (typical):
#   salloc -A <account> -p <partition> -N1 -c24 -t 0:30:00     # interactive node
#   module load llvm/15            # or set LLVM_PREFIX manually
#   bash scripts/run_stage_sweep_ice.sh
#
# Env overrides:
#   LLVM_PREFIX="..."              install root containing bin/clang, bin/opt
#   STAGES_LIST="1 2 3 4 ..."      stage counts (default: 1..12)
#   N_SIZE=1000000                 problem size (single value)
#   SAMPLES=5                      number of runs per stage to take median over
#
# This script is self-contained: it builds the project from source so
# you can run it on a fresh cluster checkout.

set -euo pipefail

PROJ_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$PROJ_ROOT/build"
RESULTS="$PROJ_ROOT/results"

# Pick which benchmark to sweep. The 12-heavy version (default) gives the
# headline curve; the 5-heavy llist_heavy is useful as a comparison —
# its peak should land around N=5 since it has 5 partitionable units.
BENCH="${BENCH:-llist_heavy_12}"
CSV="$RESULTS/stage_sweep_ice_${BENCH}.csv"

STAGES_LIST="${STAGES_LIST:-1 2 3 4 5 6 7 8 9 10 11 12}"
N_SIZE="${N_SIZE:-1000000}"
SAMPLES="${SAMPLES:-5}"

# ─── Locate LLVM 15 ──────────────────────────────────────────────────
if [[ -z "${LLVM_PREFIX:-}" ]]; then
  if command -v llvm-config >/dev/null 2>&1; then
    LLVM_PREFIX="$(llvm-config --prefix)"
    LLVM_VER="$(llvm-config --version)"
    case "$LLVM_VER" in
      15.*|16.*) ;;  # tested
      *)
        echo "WARNING: llvm-config reports $LLVM_VER. Project is tested on" >&2
        echo "         LLVM 15 and 16. Continuing anyway — set LLVM_PREFIX" >&2
        echo "         explicitly to override." >&2
        ;;
    esac
  elif command -v brew >/dev/null 2>&1 && brew --prefix llvm@15 >/dev/null 2>&1; then
    LLVM_PREFIX="$(brew --prefix llvm@15)"
  else
    cat >&2 <<EOF
ERROR: Cannot find LLVM. On PACE ICE, try:
  module avail llvm
  module load llvm/16.0.2
or set LLVM_PREFIX explicitly:
  export LLVM_PREFIX=/path/to/llvm
EOF
    exit 1
  fi
fi
export LLVM_PREFIX
echo "Using LLVM at: $LLVM_PREFIX"

# ─── Build everything ────────────────────────────────────────────────
echo "Building project..."
bash "$PROJ_ROOT/scripts/build.sh" >/dev/null
echo "Build OK."

PLUGIN_SO="$BUILD_DIR/pass/DSWPTransform.so"
PLUGIN_DYLIB="$BUILD_DIR/pass/DSWPTransform.dylib"
if   [[ -f "$PLUGIN_SO"    ]]; then PLUGIN="$PLUGIN_SO"
elif [[ -f "$PLUGIN_DYLIB" ]]; then PLUGIN="$PLUGIN_DYLIB"
else
  echo "Plugin not built at $PLUGIN_SO or $PLUGIN_DYLIB" >&2
  exit 1
fi

RUNTIME_LIB="$BUILD_DIR/runtime/libdswp_runtime.a"
SRC="$PROJ_ROOT/benchmarks/$BENCH.c"
SEQ_BIN="$BUILD_DIR/benchmarks/${BENCH}_seq"

if [[ ! -f "$SRC" ]]; then echo "missing source: $SRC" >&2; exit 1; fi
if [[ ! -f "$SEQ_BIN" ]]; then echo "missing seq baseline: $SEQ_BIN" >&2; exit 1; fi

# ─── Per-stage compile ───────────────────────────────────────────────
mkdir -p "$RESULTS"
echo "stages,n,seq_s,dswp_s,speedup,samples" > "$CSV"

# Helper: median of $@.
median() {
  local sorted=( $(printf "%s\n" "$@" | sort -n) )
  local n=${#sorted[@]}
  local mid=$((n / 2))
  if (( n % 2 == 1 )); then
    echo "${sorted[$mid]}"
  else
    /usr/bin/python3 -c "print(f'{(${sorted[$((mid-1))]} + ${sorted[$mid]}) / 2:.6f}')"
  fi
}

extract_time() {
  echo "$1" | grep -oE '[0-9]+\.[0-9]+ s' | head -1 | grep -oE '[0-9]+\.[0-9]+'
}

# Sample the seq baseline first (it doesn't change with N).
echo "=========================================================="
echo "sampling sequential baseline ($SAMPLES runs)..."
echo "=========================================================="
SEQ_TIMES=()
for ((s = 0; s < SAMPLES; s++)); do
  out=$("$SEQ_BIN" "$N_SIZE")
  t=$(extract_time "$out")
  SEQ_TIMES+=( "$t" )
  echo "  run $((s+1)): seq=${t}s"
done
SEQ_MED=$(median "${SEQ_TIMES[@]}")
echo "seq median over $SAMPLES runs: ${SEQ_MED}s"

for N in $STAGES_LIST; do
  echo
  echo "=========================================================="
  echo "N=$N stages"
  echo "=========================================================="

  if [[ "$N" -eq 1 ]]; then
    EXE="$SEQ_BIN"
    echo "  (using sequential baseline as 1-stage point)"
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
      "$BC_CANON" -o "$BC_DSWP" 2>&1 | { grep -v '^$' || true; }

    "$LLVM_PREFIX/bin/clang++" -O2 \
      "$BC_DSWP" "$RUNTIME_LIB" -lpthread -lm -o "$EXE"
  fi

  # Multiple runs, take median runtime.
  DSWP_TIMES=()
  for ((s = 0; s < SAMPLES; s++)); do
    out=$("$EXE" "$N_SIZE")

    # First sample also checks correctness.
    if (( s == 0 )); then
      seq_out=$("$SEQ_BIN" "$N_SIZE")
      seq_val=$(echo "$seq_out" | sed 's/ *(.*//')
      dswp_val=$(echo "$out" | sed 's/ *(.*//')
      if [[ "$seq_val" != "$dswp_val" ]]; then
        echo "MISMATCH at N=$N: seq='$seq_val' dswp='$dswp_val'" >&2
        exit 1
      fi
    fi

    t=$(extract_time "$out")
    DSWP_TIMES+=( "$t" )
    echo "  run $((s+1)): dswp=${t}s"
  done
  DSWP_MED=$(median "${DSWP_TIMES[@]}")
  SPEEDUP=$(/usr/bin/python3 -c "print(f'{${SEQ_MED}/${DSWP_MED}:.3f}')")
  echo "  -> N=$N median: seq=${SEQ_MED}s dswp=${DSWP_MED}s speedup=${SPEEDUP}x"
  echo "${N},${N_SIZE},${SEQ_MED},${DSWP_MED},${SPEEDUP},${SAMPLES}" >> "$CSV"
done

echo
echo "=========================================================="
echo "wrote $CSV"
echo "=========================================================="

# ─── Plot ────────────────────────────────────────────────────────────
PY=""
for cand in /usr/bin/python3 python3; do
  if "$cand" -c "import matplotlib" 2>/dev/null; then PY="$cand"; break; fi
done
if [[ -n "$PY" ]]; then
  "$PY" "$PROJ_ROOT/scripts/plot_stage_sweep_ice.py" || \
    echo "(plot script failed — CSV is intact at $CSV)"
else
  echo "(matplotlib not available — install with 'pip install matplotlib' to plot)"
fi
