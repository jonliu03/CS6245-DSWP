#!/usr/bin/env bash
# ══════════════════════════════════════════════════════════════════════
# DSWP Project Demo Script
# ══════════════════════════════════════════════════════════════════════
#
# Usage:
#   bash scripts/demo.sh                   # run everything
#   bash scripts/demo.sh --analyze         # analyzer only (all benchmarks)
#   bash scripts/demo.sh --transform       # LLVM transform (all benchmarks)
#   bash scripts/demo.sh --heavy           # llist_heavy (chained heavy compute, default 2 stages)
#   bash scripts/demo.sh --sum             # sum_list only
#   bash scripts/demo.sh --compute         # sum_with_compute only
#   bash scripts/demo.sh --max             # max_list only
#   bash scripts/demo.sh --histogram       # histogram only (counted loop, mem recurrence)
#   bash scripts/demo.sh --fib             # fib_iter only (no parallelism — partition is single-stage)
#
set -euo pipefail

PROJ_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LLVM_PREFIX="${LLVM_PREFIX:-$(brew --prefix llvm@15)}"
BUILD_DIR="$PROJ_ROOT/build"

# ─── Formatting helpers ──────────────────────────────────────────────

BOLD='\033[1m'
GREEN='\033[0;32m'
RED='\033[0;31m'
CYAN='\033[0;36m'
YELLOW='\033[0;33m'
DIM='\033[2m'
RESET='\033[0m'

banner() {
  echo
  echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════════════════${RESET}"
  echo -e "${BOLD}${CYAN}  $1${RESET}"
  echo -e "${BOLD}${CYAN}══════════════════════════════════════════════════════════════${RESET}"
}

section() {
  echo
  echo -e "${BOLD}${YELLOW}── $1 ──${RESET}"
}

ok()   { echo -e "  ${GREEN}[OK]${RESET} $1"; }
fail() { echo -e "  ${RED}[FAIL]${RESET} $1"; }
info() { echo -e "  ${DIM}$1${RESET}"; }

# ─── Build ────────────────────────────────────────────────────────────

do_build() {
  banner "Step 1: Building project"
  bash "$PROJ_ROOT/scripts/build.sh" 2>&1 | tail -3
  echo
  ok "Pass plugins:  DSWPAnalyzer.so, DSWPTransform.so"
  ok "Runtime:       libdswp_runtime.a"
  ok "Benchmarks:    llist_sum_seq, llist_heavy_seq, histogram_seq, ..."
}

# ─── Analyzer ─────────────────────────────────────────────────────────

do_analyze() {
  banner "Step 2: DSWP Analyzer (PDG + SCC + Partition)"
  info "Running analyzer on all benchmarks..."
  echo

  for bench in llist_sum llist_compute histogram fib_iter; do
    section "$bench"
    bash "$PROJ_ROOT/scripts/run_analyzer.sh" "$bench" 2>&1 | grep -E '(loop\[|partition:|->)' | while read -r line; do
      echo "  $line"
    done
  done

  echo
  section "Rendering DOT files to SVG"
  bash "$PROJ_ROOT/scripts/visualize.sh" 2>&1 | while read -r line; do
    echo "  $line"
  done

  echo
  section "Summary table"
  echo
  python3 "$PROJ_ROOT/scripts/summary.py"
  echo
  ok "Reports:  reports/*.json, reports/*.dot"
  ok "Figures:  results/figures/*.svg"
}

# ─── LLVM Transform ──────────────────────────────────────────────────

# Pretty-print wrapper around scripts/run_transform.sh. Delegates the
# actual compile + opt + link + run pipeline to that script (single
# source of truth) and parses its `## RESULT n=N seq=ST dswp=DT` lines
# to render the colored table + append to results/timing.csv.
run_transform_bench() {
  local bench="$1"
  local label="$2"
  local sizes="${3:-}"  # optional override; empty → use run_transform's defaults

  section "$label ($bench)"
  info "delegates to scripts/run_transform.sh"
  echo

  local CSV="$PROJ_ROOT/results/timing.csv"
  mkdir -p "$PROJ_ROOT/results"
  [[ -f "$CSV" ]] || echo "benchmark,n,seq_s,dswp_s,speedup" > "$CSV"

  printf "  %-10s  %-12s  %-12s  %s\n" "n"          "seq (s)"      "DSWP (s)"     "speedup"
  printf "  %-10s  %-12s  %-12s  %s\n" "----------" "------------" "------------" "-------"

  # Capture run_transform.sh output. SIZES env override only applied if the
  # caller explicitly passed a sizes string.
  local rt_out rt_status=0
  if [[ -n "$sizes" ]]; then
    rt_out=$(SIZES="$sizes" bash "$PROJ_ROOT/scripts/run_transform.sh" "$bench" 2>&1) || rt_status=$?
  else
    rt_out=$(bash "$PROJ_ROOT/scripts/run_transform.sh" "$bench" 2>&1) || rt_status=$?
  fi

  local all_ok=true mismatch_detail=""
  while IFS= read -r line; do
    [[ "$line" == "## RESULT "* ]] || continue
    # parse "## RESULT n=NNN seq=ST dswp=DT status=STATUS"
    local n st dt status
    n=$(echo "$line"      | sed -E 's/.*n=([^ ]+).*/\1/')
    st=$(echo "$line"     | sed -E 's/.*seq=([^ ]+).*/\1/')
    dt=$(echo "$line"     | sed -E 's/.*dswp=([^ ]+).*/\1/')
    status=$(echo "$line" | sed -E 's/.*status=([^ ]+).*/\1/')

    local speedup="--" color="$GREEN" display
    if [[ "$st" != "-" && "$dt" != "-" ]]; then
      speedup=$(python3 -c "
st=${st}; dt=${dt}
print(f'{st/dt:.2f}x' if dt > 0 else '--')" 2>/dev/null || echo "--")
    fi
    display="$speedup"
    if [[ "$speedup" != "--" ]]; then
      local sval="${speedup%x}"
      local is_slow=$(python3 -c "print(1 if ${sval} < 1.0 else 0)" 2>/dev/null || echo 0)
      if [[ "$is_slow" == "1" ]]; then
        color="$RED"
        local slow=$(python3 -c "print(f'{1.0/${sval}:.1f}')" 2>/dev/null || echo "?")
        display="${speedup}  (${slow}x SLOWER)"
      fi
    fi
    printf "  %-10s  %-12s  %-12s  ${color}%s${RESET}\n" "$n" "$st" "$dt" "$display"
    if [[ "$st" != "-" && "$dt" != "-" ]]; then
      echo "${bench},${n},${st},${dt},${speedup%x}" >> "$CSV"
    fi
    if [[ "$status" != "OK" ]]; then
      all_ok=false
      mismatch_detail="n=$n status=$status"
    fi
  done <<< "$rt_out"

  echo
  if $all_ok && [[ $rt_status -eq 0 ]]; then
    ok "Output matches sequential baseline across all n."
  else
    fail "run_transform.sh failed${mismatch_detail:+: $mismatch_detail} (exit=$rt_status)"
  fi
}

do_transform_sum() {
  run_transform_bench "llist_sum" "sum_list (generic 2-stage)"
}

do_transform_compute() {
  run_transform_bench "llist_compute" "sum_with_compute (generic 2-stage)"
}

do_transform_max() {
  run_transform_bench "max_list" "max_list (generic 2-stage, NEW)"
}

do_transform_histogram() {
  run_transform_bench "histogram" "histogram (cloned 2-stage, memory recurrence)"
}

do_transform_fib() {
  run_transform_bench "fib_iter" "fib_iter (single-stage — no parallelism)"
}

do_transform_heavy() {
  run_transform_bench "llist_heavy" "sum_heavy (cloned 2-stage, balanced heavy stages)"
}

do_transform() {
  banner "Step 3: LLVM Transform Pass (IR-generated stages)"
  mkdir -p "$PROJ_ROOT/results"
  echo "benchmark,n,seq_s,dswp_s,speedup" > "$PROJ_ROOT/results/timing.csv"
  do_transform_sum
  do_transform_compute
  do_transform_max
  do_transform_heavy
  do_transform_histogram
  do_transform_fib
  echo
  section "Generating runtime plots"
  # Prefer system python (matplotlib install lives there on macOS x86_64).
  local PY="${PYTHON_BIN:-}"
  if [[ -z "$PY" ]]; then
    if /usr/bin/python3 -c "import matplotlib" 2>/dev/null; then
      PY="/usr/bin/python3"
    elif python3 -c "import matplotlib" 2>/dev/null; then
      PY="python3"
    fi
  fi
  if [[ -n "$PY" ]]; then
    "$PY" "$PROJ_ROOT/scripts/plot_timing.py" || info "(plot script failed)"
  else
    info "(matplotlib not installed — skipping plots)"
  fi
}

# ─── Parse flags ──────────────────────────────────────────────────────

MODE="${1:-all}"

echo -e "${BOLD}"
cat <<'BANNER'
   ____  ______        ______   ____
  |  _ \/ ___\ \      / /  _ \ |  _ \  ___ _ __ ___   ___
  | | | \___ \\ \ /\ / /| |_) || | | |/ _ \ '_ ` _ \ / _ \
  | |_| |___) |\ V  V / |  __/ | |_| |  __/ | | | | | (_) |
  |____/|____/  \_/\_/  |_|    |____/ \___|_| |_| |_|\___/
BANNER
echo -e "${RESET}"
echo -e "${DIM}  Decoupled Software Pipelining — CS6245 Project${RESET}"
echo

do_build

case "$MODE" in
  --analyze)
    do_analyze
    ;;
  --transform)
    do_transform
    ;;
  --sum)
    do_transform_sum
    ;;
  --compute)
    do_transform_compute
    ;;
  --max)
    do_transform_max
    ;;
  --histogram)
    do_transform_histogram
    ;;
  --fib)
    do_transform_fib
    ;;
  --heavy)
    do_transform_heavy
    ;;
  all|--all)
    do_analyze
    do_transform
    ;;
  *)
    echo "Unknown flag: $MODE"
    echo "Usage: $0 [--analyze|--transform|--sum|--compute|--max|--heavy|--histogram|--fib|--all]"
    exit 1
    ;;
esac

banner "Done"
echo -e "  ${GREEN}All steps completed successfully.${RESET}"
echo
