# CS6245 DSWP Project

LLVM pass implementing **Decoupled Software Pipelining** (Ottoni et al., MICRO 2005).

The project has two passes:

- **`DSWPAnalyzer`** — builds a per-loop PDG (register / memory / control edges, loop-carried flagged), runs Tarjan's SCC, coalesces into the SCC-DAG, and produces a 2-stage greedy partition. Emits JSON + Graphviz DOT for inspection.
- **`DSWPTransform`** — actually rewrites IR. For most benchmarks this is `cloned::transformGenericCloned`: clone the loop CFG into each stage function, surgically erase non-stage instructions, replace cross-stage uses with queue dequeues, insert enqueues after producers. The "loop control" SCCs (anything the loop's terminators transitively depend on) are replicated to every stage so each thread iterates independently — the queue carries data, not control. A separate hardcoded `transformProcessPipeline` handles the 5-stage `process_pipeline` benchmark; everything else goes through the generic path.

---

## Setup

```bash
brew install llvm@15 graphviz
bash scripts/build.sh        # produces build/pass/{DSWPAnalyzer,DSWPTransform}.so + runtime + benchmark binaries
```

If `llvm@15` lives somewhere unusual, set `LLVM_PREFIX` (the scripts default to `$(brew --prefix llvm@15)`).

---

## Testing

Two layers: the **demo script** (pretty output, recommended) and the **raw scripts** (direct invocation, useful when debugging).

### Demo script — `scripts/demo.sh`

One unified entry point with flags. Builds, runs, compares DSWP output vs the `-O3` sequential baseline, and reports speedup.

```bash
bash scripts/demo.sh                # build + analyze + transform-all
bash scripts/demo.sh --analyze      # analyzer only (PDG + SCC + partition + DOT/SVG)
bash scripts/demo.sh --transform    # all 6 transform benchmarks
```

Per-benchmark transform flags:

| Flag | Benchmark | Path | What it tests |
|---|---|---|---|
| `--sum`       | `llist_sum`          | cloned 2-stage | linked-list sum (i64 reduction) |
| `--compute`   | `llist_compute`      | cloned 2-stage | list walk + per-node `sqrt + log` (double reduction) |
| `--max`       | `max_list`           | cloned 2-stage | list walk + max-via-select (different reduction op) |
| **`--heavy`** | `llist_heavy`        | cloned N-stage | **5 chained heavy compute calls per node — partitions into 1..5 stages, see sweep section** |
| `--histogram` | `histogram`          | cloned 2-stage | counted loop, void return, **memory recurrence** (`bins[idx]++`) |
| `--fib`       | `fib_iter`           | not transformed | counted loop, single-stage partition (no parallelism — by design) |

For multi-stage testing of `llist_heavy`, use the sweep script (next section) rather than `demo.sh --heavy` which only runs at the default `DSWP_NUM_STAGES=2`.

Each transform run prints a table of `n / seq (s) / DSWP (s) / speedup`, and a single `[OK] Output matches sequential baseline across all n.` confirms correctness without spamming the result-value column for every n.

`bash scripts/demo.sh --transform` (all benchmarks) additionally:
- writes per-row timings to **`results/timing.csv`** (`benchmark,n,seq_s,dswp_s,speedup`)
- generates plots (requires matplotlib — system python3 on macOS works):
  - **`results/figures/runtime.png`** — seq vs DSWP runtime at largest n, log scale
  - **`results/figures/speedup.png`** — speedup bar with 1.0× parity line
  - **`results/figures/runtime_scaling.png`** — runtime vs n curves per benchmark, log-log

To regenerate plots from an existing `timing.csv`:
```bash
/usr/bin/python3 scripts/plot_timing.py
```

### Direct script — `scripts/run_transform.sh`

```bash
bash scripts/run_transform.sh <bench_name>
# e.g.
bash scripts/run_transform.sh histogram
bash scripts/run_transform.sh max_list
```

Compiles `benchmarks/<bench_name>.c` → bitcode → `opt -passes=dswp-transform` → links with runtime → runs across n = 100 / 10000 / 100000 against the sequential baseline, asserting result equality. Faster for tight iteration; less polished output.

### Analyzer — `scripts/run_analyzer.sh`

```bash
bash scripts/run_analyzer.sh <bench_name>
# Outputs (under reports/<bench>/):
#   <func>_<loop>.json              — partition + cost summary
#   <func>_<loop>_pdg.dot           — full PDG (data/mem/ctrl color-coded by SCC)
#   <func>_<loop>_partition.dot     — SCC-DAG colored by stage assignment
#   <func>_<loop>_cfg.dot           — loop CFG (header/latch/body/exit color-coded,
#                                     back-edge highlighted, T/F branch labels)
```

Render DOT to SVG:
```bash
bash scripts/visualize.sh        # reports/<bench>/*.dot → results/figures/analyzer/<bench>/*.svg
```

---

## Adding a new benchmark

1. Drop `benchmarks/<name>.c` with a `noinline` hot loop and a `main` that times it (`clock_gettime(CLOCK_MONOTONIC, ...)` and `printf("result = ... (%.4f s, n=%ld)\n", ...)`).
2. Add `<name>` to `benchmarks/CMakeLists.txt`'s `foreach(b ...)` list (the seq-build foreach).
3. Add a case to `scripts/run_transform.sh`'s benchmark→function-name switch.
4. (Optional) Add a `--<name>` flag in `scripts/demo.sh` if you want pretty output.

The pass itself needs no edits — the cloned transform discovers the loop and partition automatically. If the loop's shape is too unusual, the transform returns `false` and the binary just runs the original sequential code.

---

## Layout

```
pass/                  — LLVM passes (PDG, Partition, Analyzer, Transform)
runtime/               — lock-free SPSC queue (libdswp_runtime.a)
benchmarks/            — .c files, both as analyzer targets and seq baselines
scripts/               — build, run, visualize, demo, plot_timing
reports/               — analyzer output (JSON + DOT)
results/
├── timing.csv         — per-(benchmark, n) seq + DSWP runtimes + speedup (from --transform, default 2 stages)
├── stage_sweep.csv    — per-(stage_count, n) timings for llist_heavy (from run_stage_sweep.sh)
└── figures/
    ├── runtime.png            — seq vs DSWP at largest n
    ├── speedup.png            — speedup bar at largest n
    ├── runtime_scaling.png    — runtime vs n, log-log
    ├── stage_sweep.png        — llist_heavy speedup + runtime vs stage count (1..5)
    └── analyzer/<bench>/      — per-loop PDG / partition / CFG SVGs (from --analyze)
```
