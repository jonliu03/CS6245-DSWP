#!/usr/bin/env python3
"""Generate two presentation diagrams into supplemental/.

  architecture.png — pipeline + LLVM integration
  schedule.png     — pipelined-execution schedule of a 4-stage DSWP

Run with: /usr/bin/python3 supplemental/make_diagrams.py
"""

from pathlib import Path
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch, FancyArrowPatch, Patch
from matplotlib.lines import Line2D

OUT = Path(__file__).resolve().parent
OUT.mkdir(parents=True, exist_ok=True)

LLVM_COLOR  = "#cfe2f3"   # blue   — stock LLVM tools
OURS_COLOR  = "#ffd966"   # yellow — our pass / runtime / scripts
ART_COLOR   = "#e6e6e6"   # grey   — generated artefacts
TEXT_DARK   = "#222222"
ARROW_COL   = "#444444"

# ─────────────────────────────────────────────────────────────────────
# Diagram 1: architecture
# ─────────────────────────────────────────────────────────────────────
def draw_box(ax, x, y, w, h, text, color, fontsize=9, weight="normal"):
    box = FancyBboxPatch((x, y), w, h,
                         boxstyle="round,pad=0.04,rounding_size=0.10",
                         linewidth=1.1, edgecolor=TEXT_DARK,
                         facecolor=color, zorder=2)
    ax.add_patch(box)
    ax.text(x + w/2, y + h/2, text, ha="center", va="center",
            fontsize=fontsize, color=TEXT_DARK, zorder=3, weight=weight,
            wrap=True)

def arrow(ax, x1, y1, x2, y2, label=None, **kwargs):
    a = FancyArrowPatch((x1, y1), (x2, y2),
                        arrowstyle="-|>", mutation_scale=14,
                        linewidth=1.4, color=ARROW_COL, zorder=1, **kwargs)
    ax.add_patch(a)
    if label:
        ax.text((x1 + x2) / 2, (y1 + y2) / 2 + 0.15, label,
                ha="center", va="bottom", fontsize=8, color=ARROW_COL,
                style="italic")

def make_architecture():
    """Single straight horizontal pipeline at top, runtime.a as a side
    input feeding the link step from below, and a callout below the
    DSWPTransform box showing what's inside it. Reads strictly L→R.
    """
    fig, ax = plt.subplots(figsize=(17, 7.5))
    ax.set_xlim(0, 19); ax.set_ylim(0, 8.5); ax.set_aspect("equal"); ax.axis("off")

    # ── Main pipeline (top): every box is a *process*; arrows label the
    # ── file format flowing between them. Reads naturally L → R.
    y_main = 6.20
    h_main = 0.95
    boxes = [
        # (x, label, color, width)
        (0.40,  ".c\nbenchmark",                                  ART_COLOR,  1.50),
        (3.20,  "clang -O1\n-emit-llvm",                           LLVM_COLOR, 1.70),
        (6.20,  "opt\nmem2reg, simplifycfg,\nloop-simplify, lcssa",LLVM_COLOR, 2.50),
        (10.00, "opt\n-passes=dswp-transform\n(our pass plugin)",  OURS_COLOR, 2.60),
        (13.90, "clang++ -O2\n-lpthread -lm",                      LLVM_COLOR, 1.95),
        (17.15, "binary",                                          ART_COLOR,  1.50),
    ]
    edges = []
    for (x, t, c, w) in boxes:
        draw_box(ax, x, y_main, w, h_main, t, c, fontsize=9,
                 weight=("bold" if c == OURS_COLOR else "normal"))
        edges.append((x, x + w))

    # Connect with labeled arrows. Label = artefact flowing out of the
    # source box. (.c is the input; rest of the arrows carry .bc.)
    arrow_labels = ["", ".bc", ".bc (canonical)", ".bc (DSWP'd)", ""]
    for i in range(len(boxes) - 1):
        _, xR = edges[i]
        xL2, _ = edges[i+1]
        arrow(ax, xR + 0.10, y_main + h_main/2,
                  xL2 - 0.10, y_main + h_main/2,
              label=arrow_labels[i])

    # ── libdswp_runtime.a as a side input — sits BELOW the clang++ box
    # ── and points up at it. No crossings.
    rt_x = 13.90; rt_w = 1.95
    rt_y = 4.40;  rt_h = 0.95
    draw_box(ax, rt_x, rt_y, rt_w, rt_h,
             "libdswp_runtime.a\nSPSC queue\n+ pthread driver",
             OURS_COLOR, fontsize=8.4, weight="bold")
    arrow(ax, rt_x + rt_w/2, rt_y + rt_h,
              rt_x + rt_w/2, y_main - 0.02, label="link")

    # ── Callout below the DSWPTransform box: internals of our pass.
    # ── Connected by a dashed line so it reads as detail, not a stage.
    iy = 0.80; ih = 1.85; ix = 0.50; iw = 18.00
    inset = FancyBboxPatch((ix, iy), iw, ih,
                           boxstyle="round,pad=0.04,rounding_size=0.10",
                           linewidth=1.4, edgecolor="#aa7800",
                           facecolor="#fff4cc", zorder=1.5)
    ax.add_patch(inset)
    ax.text(ix + 0.25, iy + ih - 0.25,
            "Inside the dswp-transform pass",
            fontsize=10.5, weight="bold", color="#7a5500")

    inner = [
        "Build PDG\n(data + mem +\ncontrol edges)",
        "Tarjan SCC\n+ SCC-DAG",
        "partitionNStage\n(N stages, cost-\nbalanced)",
        "Clone loop CFG\nper stage; erase\nnon-stage instrs",
        "Insert dequeue/\nenqueue + emit\npthread driver",
    ]
    bx = ix + 0.35
    bw = (iw - 0.70 - (len(inner) - 1) * 0.30) / len(inner)
    bh = 1.00
    by = iy + 0.20
    inner_edges = []
    for t in inner:
        draw_box(ax, bx, by, bw, bh, t, OURS_COLOR, fontsize=8.0, weight="bold")
        inner_edges.append((bx, bx + bw))
        bx += bw + 0.30
    for i in range(len(inner_edges) - 1):
        _, xR = inner_edges[i]
        xL2, _ = inner_edges[i+1]
        arrow(ax, xR + 0.04, by + bh/2, xL2 - 0.04, by + bh/2)

    # Dashed connector from the dswp-transform box down to the callout
    # (no text label — the visual relationship is self-evident).
    dswp_x_center = 10.00 + 2.60/2
    ax.plot([dswp_x_center, dswp_x_center],
            [y_main - 0.05, iy + ih + 0.05],
            linestyle=(0, (4, 3)), linewidth=1.3, color="#aa7800", zorder=1)

    # ── Legend (top-left, with breathing room above the pipeline row).
    leg = [
        Patch(facecolor=LLVM_COLOR, edgecolor=TEXT_DARK, label="LLVM tool (stock)"),
        Patch(facecolor=OURS_COLOR, edgecolor=TEXT_DARK, label="Our pass / runtime"),
        Patch(facecolor=ART_COLOR,  edgecolor=TEXT_DARK, label="Artifact (file)"),
    ]
    ax.legend(handles=leg, loc="upper left", bbox_to_anchor=(0.005, 0.97),
              frameon=False, fontsize=9.5,
              borderaxespad=0.6)

    ax.set_title("DSWP pipeline architecture & LLVM integration",
                 fontsize=14, weight="bold", pad=18)
    fig.tight_layout()
    out = OUT / "architecture.png"
    fig.savefig(out, dpi=160, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote: {out}")


# ─────────────────────────────────────────────────────────────────────
# Diagram 2: pipelined-execution schedule
# ─────────────────────────────────────────────────────────────────────
def make_schedule():
    fig, (ax_seq, ax_dswp) = plt.subplots(2, 1, figsize=(13, 6.5),
                                           gridspec_kw={"height_ratios":[1, 2]})

    # Source-code annotation (top-left text box, separate)
    src = ("void process_list(Node *head) {\n"
           "  for (Node *p = head; p != NULL; p = p->next) {\n"
           "    func1(p->value);   // heavy\n"
           "    func2(p->value);   // heavy\n"
           "    func3(p->value);   // heavy\n"
           "  }\n}")

    # ── Sequential timeline (top) ──
    # 3 iterations, each running walker + 3 funcs back-to-back.
    iters = 3
    walker_w = 0.4
    func_w   = 1.0
    stage_colors = {
        "walk": "#888888",
        "f1":   "#4daf4a",
        "f2":   "#377eb8",
        "f3":   "#984ea3",
    }
    seq_blocks = []
    t = 0.0
    for i in range(iters):
        seq_blocks.append((t,             walker_w, "walk", i))
        t += walker_w
        seq_blocks.append((t,             func_w,   "f1",   i))
        t += func_w
        seq_blocks.append((t,             func_w,   "f2",   i))
        t += func_w
        seq_blocks.append((t,             func_w,   "f3",   i))
        t += func_w
    seq_total = t

    for (x, w, kind, i) in seq_blocks:
        ax_seq.barh(0, w, left=x, height=0.6,
                     color=stage_colors[kind], edgecolor="black", linewidth=0.6)
        ax_seq.text(x + w/2, 0, ({"walk":"w","f1":"f1","f2":"f2","f3":"f3"}[kind] +
                                  f"\ni{i+1}"),
                     ha="center", va="center", fontsize=8.5,
                     color=("white" if kind != "walk" else "white"))
    ax_seq.set_yticks([0]); ax_seq.set_yticklabels(["sequential\n(1 thread)"], fontsize=10)
    ax_seq.set_xlim(-0.2, max(seq_total, 14) + 0.2)
    ax_seq.set_ylim(-0.8, 0.8)
    ax_seq.spines["top"].set_visible(False); ax_seq.spines["right"].set_visible(False)
    ax_seq.spines["left"].set_visible(False)
    ax_seq.tick_params(left=False)
    ax_seq.set_title("Sequential vs DSWP — pipelined execution schedule",
                      fontsize=13, weight="bold", pad=12)

    # ── DSWP timeline (bottom) — 4 stages, 6 iterations ──
    n_iters = 6
    stage_widths = [walker_w, func_w, func_w, func_w]
    stage_kinds  = ["walk", "f1", "f2", "f3"]
    stage_labels = ["S0\nwalker", "S1\nfunc1", "S2\nfunc2", "S3\nfunc3"]

    # Once steady-state, the bottleneck is max(stage_widths) = func_w (=1.0).
    # Each stage's iteration k starts at: k * func_w + (offset to flow through prior stages).
    # For a clean teaching picture: stage i of iter k starts at (k + i) * func_w
    # if we just align everyone on the func_w grid. Walker is shorter so we can
    # visualize idle/wait gaps but to keep it simple we'll align the func stages
    # at func_w cadence.
    period = func_w
    # We'll show the walker filling its own grid (since it's cheap, mostly idle).
    blocks = []
    for k in range(n_iters):
        # Walker on stage 0: starts at k*period (idles after walker_w)
        blocks.append((0, k * period,                walker_w, "walk", k))
        # Stage i ≥ 1: starts at (k + i) * period (after walker has produced + previous stage)
        for i in range(1, 4):
            start = (k + i) * period
            blocks.append((i, start, func_w, stage_kinds[i], k))

    # Draw stages top→bottom (S0 at top).
    n_stages = 4
    bar_h = 0.6
    for (stage, start, w, kind, k) in blocks:
        y = (n_stages - 1 - stage) + 0.5  # invert so S0 is at top
        ax_dswp.barh(y, w, left=start, height=bar_h,
                      color=stage_colors[kind], edgecolor="black", linewidth=0.6)
        label = ({"walk":"w","f1":"f1","f2":"f2","f3":"f3"}[kind] + f"\ni{k+1}")
        ax_dswp.text(start + w/2, y, label, ha="center", va="center",
                      fontsize=8.5, color="white")

    # Annotate startup transient region
    ax_dswp.axvspan(0, 3 * period, alpha=0.06, color="orange")
    ax_dswp.text(1.5 * period, n_stages + 0.10, "fill phase",
                  ha="center", fontsize=9, color="#aa6500", style="italic")
    # Annotate steady-state
    ax_dswp.text(5.0 * period, n_stages + 0.10, "steady state — all 4 threads busy",
                  ha="center", fontsize=9, color="#226600", style="italic")
    ax_dswp.axvline(3 * period, color="#aa6500", linestyle=":", linewidth=1.2)

    ax_dswp.set_yticks([(n_stages - 1 - i) + 0.5 for i in range(n_stages)])
    ax_dswp.set_yticklabels(stage_labels, fontsize=10)
    ax_dswp.set_xlim(-0.2, (n_iters + 3) * period + 0.2)
    ax_dswp.set_ylim(-0.2, n_stages + 0.6)
    ax_dswp.set_xlabel("time →", fontsize=10)
    ax_dswp.spines["top"].set_visible(False); ax_dswp.spines["right"].set_visible(False)

    # Throughput annotation: max(stage cost) = func_w in steady state.
    # Total iters processed in window of length L: ~ L / func_w (after fill).
    ax_dswp.annotate("",
        xy=(7 * period, n_stages + 0.30), xytext=(8 * period, n_stages + 0.30),
        arrowprops=dict(arrowstyle="<->", color="#226600", lw=1.4))
    ax_dswp.text(7.5 * period, n_stages + 0.40, "1 iter / 1 stage-cost",
                  ha="center", fontsize=8.5, color="#226600")

    # Source-code box
    fig.text(0.81, 0.78, src, fontsize=8.5, family="monospace",
             bbox=dict(boxstyle="round,pad=0.5", facecolor="#f5f5f5",
                       edgecolor="#888888"), va="top")

    # Legend at bottom
    leg_handles = [
        Patch(facecolor=stage_colors["walk"], label="walker (load p->value, p->next)"),
        Patch(facecolor=stage_colors["f1"],   label="stage running func1"),
        Patch(facecolor=stage_colors["f2"],   label="stage running func2"),
        Patch(facecolor=stage_colors["f3"],   label="stage running func3"),
    ]
    ax_dswp.legend(handles=leg_handles, loc="upper right",
                    bbox_to_anchor=(1.0, -0.18), ncol=4, frameon=False,
                    fontsize=9)

    fig.tight_layout(rect=[0, 0, 0.79, 0.97])
    out = OUT / "schedule.png"
    fig.savefig(out, dpi=160, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote: {out}")


# ─────────────────────────────────────────────────────────────────────
# Diagram 3: per-stage IR before/after for sum_list
# ─────────────────────────────────────────────────────────────────────
def make_ir_diff():
    fig, ax = plt.subplots(figsize=(15.5, 7.0))
    ax.set_xlim(0, 17); ax.set_ylim(0, 9); ax.set_aspect("equal"); ax.axis("off")

    # 3 columns of code. Rows are aligned by *concept* — each row is the
    # same logical step across all three columns, so the audience can read
    # left-to-right and see exactly what surgery did per stage.
    col_x = [0.30, 5.85, 11.40]
    col_w = 5.45
    row_label_x = -0.0   # we'll inline row labels into column 0 instead

    KEPT       = ("#ffffff", "#222222")
    REPLICATED = ("#fff4cc", "#7a5500")
    INSERTED   = ("#cfe8d4", "#1a5e2a")
    ERASED     = ("#ececec", "#999999")
    EMPTY      = (None, None)

    # Each row: (concept_label, cell_orig, cell_s0, cell_s1)
    # cell = (text, style)
    rows = [
        ("value addr",
         ("%va  = gep %p, 0",         KEPT),
         ("%va  = gep %p, 0",         KEPT),
         ("",                          ERASED)),
        ("value load",
         ("%v   = load i64, %va",     KEPT),
         ("%v   = load i64, %va",     KEPT),
         ("",                          ERASED)),
        ("queue send",
         ("",                          EMPTY),
         ("dswp_enqueue(q, %v)",      INSERTED),
         ("",                          EMPTY)),
        ("queue recv",
         ("",                          EMPTY),
         ("",                          EMPTY),
         ("dswp_dequeue(q, &slot)",   INSERTED)),
        ("unpack",
         ("",                          EMPTY),
         ("",                          EMPTY),
         ("%v   = load i64, slot",    INSERTED)),
        ("reduction",
         ("%new = add i64 %sum, %v",  KEPT),
         ("",                          ERASED),
         ("%new = add i64 %sum, %v",  KEPT)),
        ("next addr",
         ("%na  = gep %p, 1",         KEPT),
         ("%na  = gep %p, 1",         REPLICATED),
         ("%na  = gep %p, 1",         REPLICATED)),
        ("next load",
         ("%next= load ptr, %na",     KEPT),
         ("%next= load ptr, %na",     REPLICATED),
         ("%next= load ptr, %na",     REPLICATED)),
        ("back-edge",
         ("br label %header",         KEPT),
         ("br label %header",         REPLICATED),
         ("br label %header",         REPLICATED)),
    ]

    # Column titles.
    title_y = 8.10
    titles = ["ORIGINAL  (sum_list body)",
              "STAGE 0  (walker)",
              "STAGE 1  (accumulator)"]
    for i, t in enumerate(titles):
        ax.text(col_x[i] + col_w/2, title_y, t, ha="center",
                fontsize=11, weight="bold", color="#222")
        # underline
        ax.plot([col_x[i] + 0.10, col_x[i] + col_w - 0.10],
                [title_y - 0.30, title_y - 0.30],
                color="#888", linewidth=1.0)

    # Concept labels (left side of column 0).
    code_top_y = 7.40
    line_h = 0.65
    for j, row in enumerate(rows):
        y = code_top_y - j * line_h
        ax.text(col_x[0] - 0.20, y, row[0], ha="right", va="center",
                fontsize=8.5, color="#666666", style="italic")

    # Render each cell.
    for j, row in enumerate(rows):
        y = code_top_y - j * line_h
        for i in range(3):
            text, (bg, fg) = row[i + 1]
            x = col_x[i]
            # Background
            if bg is not None:
                rect = FancyBboxPatch((x, y - line_h/2 + 0.05),
                                       col_w, line_h - 0.10,
                                       boxstyle="round,pad=0.01,rounding_size=0.06",
                                       linewidth=0.6, edgecolor="#bbbbbb",
                                       facecolor=bg, zorder=2)
                ax.add_patch(rect)

            display = text if text else ""
            # Visual hint for erased cells: italicised gray "(erased)" if no text
            if (bg, fg) == ERASED and not display:
                display = "— erased —"
                style_kwargs = {"style": "italic"}
            elif (bg, fg) == ERASED:
                style_kwargs = {"style": "italic"}
            else:
                style_kwargs = {}
            if display:
                ax.text(x + 0.18, y, display,
                        ha="left", va="center",
                        fontsize=9, family="monospace",
                        color=fg, zorder=3, **style_kwargs)

    # Cross-edge arrow: from STAGE 0 row "queue send" → STAGE 1 row "queue recv".
    send_idx = next(j for j, r in enumerate(rows) if r[0] == "queue send")
    recv_idx = next(j for j, r in enumerate(rows) if r[0] == "queue recv")
    s0_y = code_top_y - send_idx * line_h
    s1_y = code_top_y - recv_idx * line_h
    s0_right = col_x[1] + col_w
    s1_left  = col_x[2]

    a = FancyArrowPatch((s0_right - 0.05, s0_y),
                        (s1_left + 0.05, s1_y),
                        arrowstyle="-|>", mutation_scale=20,
                        linewidth=2.5, color="#e67e22",
                        connectionstyle="arc3,rad=-0.25", zorder=10)
    ax.add_patch(a)
    midx = (s0_right + s1_left) / 2
    midy = (s0_y + s1_y) / 2 + 0.45
    ax.text(midx, midy, "queue (one double per iteration)",
            ha="center", fontsize=9, color="#e67e22",
            weight="bold", style="italic")

    # Legend at bottom — 2 rows × 2 columns so labels never collide.
    leg_grid = [
        [(KEPT[0],       "kept  (in this stage's partition)"),
         (INSERTED[0],   "inserted  (queue op generated by transform)")],
        [(REPLICATED[0], "replicated  (loop control / iv chain — every stage)"),
         (ERASED[0],     "erased  (other stage's work — not needed here)")],
    ]
    leg_y = 0.95
    for row in leg_grid:
        leg_x = 0.30
        for color, label in row:
            ax.add_patch(FancyBboxPatch((leg_x, leg_y), 0.45, 0.35,
                                         boxstyle="round,pad=0.02,rounding_size=0.05",
                                         facecolor=color, edgecolor="#888",
                                         linewidth=0.7))
            ax.text(leg_x + 0.55, leg_y + 0.18, label,
                    va="center", fontsize=9.0)
            leg_x += 7.80
        leg_y -= 0.50

    ax.set_title("DSWP transform — surgical erase + replace per stage  (sum_list, N=2)",
                 fontsize=13, weight="bold", pad=15)
    fig.tight_layout()
    out = OUT / "ir_diff.png"
    fig.savefig(out, dpi=160, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote: {out}")


# ─────────────────────────────────────────────────────────────────────
# Diagram 4: driver IR snippet
# ─────────────────────────────────────────────────────────────────────
def make_driver():
    # Drop aspect=equal — for a code-heavy diagram the boxes should
    # match content extent, not be square. Figure size and xlim/ylim
    # set to match content ratio so nothing gets shrunk on render.
    fig, ax = plt.subplots(figsize=(14, 8.5))
    ax.set_xlim(0, 14); ax.set_ylim(0, 8.5); ax.axis("off")

    # Code lines, each tagged with a section. Sections get a tinted band.
    sections = [
        ("alloc",  "#e1efff",  "1. allocate args struct"),
        ("queue",  "#e6f7e1",  "2. create per-edge queues"),
        ("spawn",  "#fff4cc",  "3. spawn N stage threads"),
        ("join",   "#fce0e0",  "4. wait for completion"),
        ("ret",    "#ececec",  "5. cleanup + return live-out"),
    ]

    # Each line: (kind, code_text)
    lines = [
        ("alloc",  "define i64 @sum_list(ptr %head) {"),
        ("alloc",  "  %args = alloca %struct.dswp_cl_args"),
        ("alloc",  "  store ptr %head, ptr %args.field0"),
        ("queue",  "  %q = call ptr @dswp_queue_create(i64 1024)"),
        ("queue",  "  store ptr %q, ptr %args.field2"),
        ("spawn",  "  %t0 = alloca ptr"),
        ("spawn",  "  call i32 @pthread_create(%t0, null, @dswp.cl.s0.sum_list, %args)"),
        ("spawn",  "  %t1 = alloca ptr"),
        ("spawn",  "  call i32 @pthread_create(%t1, null, @dswp.cl.s1.sum_list, %args)"),
        ("join",   "  call i32 @pthread_join(load(%t0), null)"),
        ("join",   "  call i32 @pthread_join(load(%t1), null)"),
        ("ret",    "  call void @dswp_queue_destroy(ptr %q)"),
        ("ret",    "  %r = load i64, ptr %args.field1"),
        ("ret",    "  ret i64 %r"),
        ("ret",    "}"),
    ]

    section_color = {k: c for k, c, _ in sections}
    section_label = {k: l for k, _, l in sections}

    # Layout
    code_left = 0.50
    code_right_pad = 4.50          # reserve right side for section labels
    code_w = 14.0 - code_left - code_right_pad
    line_h = 0.46
    top_y = 7.00

    # Group lines by section to draw a single tinted band per group
    bands = []  # (kind, y_top, y_bottom)
    cur = None
    for j, (kind, _) in enumerate(lines):
        y = top_y - j * line_h
        if cur is None or cur[0] != kind:
            if cur is not None:
                bands.append((cur[0], cur[1], cur[2]))
            cur = [kind, y + line_h/2, y - line_h/2]
        else:
            cur[2] = y - line_h/2
    if cur is not None:
        bands.append((cur[0], cur[1], cur[2]))

    # Draw bands
    for kind, y_top, y_bot in bands:
        h = y_top - y_bot
        rect = FancyBboxPatch((code_left - 0.10, y_bot + 0.02),
                               code_w + 0.20, h - 0.04,
                               boxstyle="round,pad=0.02,rounding_size=0.06",
                               facecolor=section_color[kind],
                               edgecolor="#bbbbbb", linewidth=0.7,
                               zorder=1)
        ax.add_patch(rect)
        # section label on the right
        ax.text(code_left + code_w + 0.40, (y_top + y_bot) / 2,
                section_label[kind],
                ha="left", va="center", fontsize=11.5,
                color="#444", weight="bold")

    # Draw code lines on top of bands
    for j, (kind, code) in enumerate(lines):
        y = top_y - j * line_h
        ax.text(code_left, y, code, ha="left", va="center",
                fontsize=11.0, family="monospace", color="#202020",
                zorder=3)

    # Header note above
    ax.text(code_left, top_y + 1.05,
            "After step 8: original sum_list is replaced by a thin driver function.",
            fontsize=11.5, color="#444", style="italic")
    ax.text(code_left, top_y + 0.55,
            "All actual work happens inside dswp.cl.s0.sum_list and dswp.cl.s1.sum_list.",
            fontsize=11.5, color="#444", style="italic")

    ax.set_title("DSWP-transformed driver function (sum_list, N=2)",
                 fontsize=13, weight="bold", pad=15)
    fig.tight_layout()
    out = OUT / "driver.png"
    fig.savefig(out, dpi=160, bbox_inches="tight")
    plt.close(fig)
    print(f"wrote: {out}")


if __name__ == "__main__":
    make_architecture()
    make_schedule()
    make_ir_diff()
    make_driver()
