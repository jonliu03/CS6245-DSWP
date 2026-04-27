//===-- DSWPAnalyzer.cpp - DSWP analysis pass --------------------------===//
//
// For each candidate loop in each function, build the PDG, find SCCs, and
// emit a DOT graph. Partitioning lands in a follow-up commit.
//
//===---------------------------------------------------------------------===//

#include "PDG.h"
#include "Partition.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/DependenceAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <system_error>

using namespace llvm;
using namespace dswp;

static cl::opt<std::string> ReportDir(
    "dswp-report-dir",
    cl::desc("Directory to write per-loop PDG DOT files"),
    cl::init("reports"));

namespace {

// A loop is a candidate for DSWP if:
//   - it is innermost (no sub-loops),
//   - in loop-simplify form,
//   - has a single latch and a single exiting block,
//   - contains no calls/invokes that we cannot reason about (we tolerate
//     readnone/readonly intrinsics and library calls — for the analyzer we
//     just flag them; the transform pass will be stricter).
static bool isCandidateLoop(Loop *L, std::string *Why) {
  if (!L->getSubLoops().empty())     { *Why = "not innermost";     return false; }
  if (!L->isLoopSimplifyForm())      { *Why = "not loop-simplify"; return false; }
  if (!L->getLoopLatch())            { *Why = "no single latch";   return false; }
  if (!L->getExitingBlock())         { *Why = "no single exiting"; return false; }
  return true;
}

// Slugify a string for filename use.
static std::string slug(StringRef S) {
  std::string Out;
  Out.reserve(S.size());
  for (char C : S) {
    if (isalnum((unsigned char)C) || C == '_' || C == '.')
      Out.push_back(C);
    else
      Out.push_back('_');
  }
  if (Out.empty()) Out = "anon";
  return Out;
}

// Print a one-line summary of a PDG node (instruction).
static std::string instLabel(Instruction *I, unsigned Id) {
  std::string S;
  raw_string_ostream OS(S);
  OS << "[" << Id << "] ";
  // Prefer the printed instruction (truncated).
  std::string Buf;
  raw_string_ostream BS(Buf);
  I->print(BS);
  BS.flush();
  // Strip leading whitespace from instruction print.
  StringRef T = StringRef(Buf).ltrim();
  // Truncate excessively long lines for graph readability.
  if (T.size() > 80) T = T.substr(0, 77);
  OS << T;
  if (Buf.size() > 80) OS << "...";
  OS.flush();
  return S;
}

// Escape a label for DOT.
static std::string dotEscape(StringRef S) {
  std::string Out;
  Out.reserve(S.size());
  for (char C : S) {
    switch (C) {
    case '"':  Out += "\\\""; break;
    case '\\': Out += "\\\\"; break;
    case '\n': Out += "\\l";  break;
    default:   Out.push_back(C);
    }
  }
  return Out;
}

// Pick a fill color for an SCC index from a small palette. Used for
// every SCC (cyclic or singleton) so the PDG diagram color-codes by
// SCC identity matching the partition diagram.
static const char *sccColor(unsigned Idx, bool Cyclic) {
  (void)Cyclic;
  static const char *Palette[] = {
    "#ffd6a5", "#a0c4ff", "#caffbf", "#ffadad", "#bdb2ff",
    "#fdffb6", "#9bf6ff", "#ffc6ff", "#e2f0cb", "#d3d3d3"
  };
  return Palette[Idx % (sizeof(Palette)/sizeof(Palette[0]))];
}

// Write the loop's CFG as a DOT file. Shows preheader + all loop blocks
// + exit blocks, color-coded by role (header / latch / body / exit /
// preheader). Edges labelled T/F for conditional branches; back-edge
// from latch to header drawn dashed.
static void writeCFGDOT(Loop *L, StringRef Path, StringRef GraphName) {
  std::error_code EC;
  raw_fd_ostream OS(Path, EC, sys::fs::OF_Text);
  if (EC) {
    errs() << "DSWPAnalyzer: cannot open " << Path << ": "
           << EC.message() << "\n";
    return;
  }

  // Collect the loop region: preheader + loop blocks + exit blocks.
  SmallVector<BasicBlock *, 8> Blocks;
  DenseSet<BasicBlock *> Seen;
  auto add = [&](BasicBlock *BB) {
    if (BB && Seen.insert(BB).second) Blocks.push_back(BB);
  };
  add(L->getLoopPreheader());
  for (BasicBlock *BB : L->blocks()) add(BB);
  SmallVector<BasicBlock *, 4> ExitBBs;
  L->getExitBlocks(ExitBBs);
  for (BasicBlock *BB : ExitBBs) add(BB);

  // Stable id per block.
  DenseMap<BasicBlock *, unsigned> BBId;
  for (BasicBlock *BB : Blocks) BBId[BB] = BBId.size();

  OS << "digraph \"" << dotEscape(GraphName) << "\" {\n";
  OS << "  rankdir=TB;\n";
  OS << "  node [shape=box, style=\"filled,rounded\", fontname=\"monospace\","
        " fontsize=10];\n";
  OS << "  edge [fontname=\"monospace\", fontsize=9];\n";

  BasicBlock *Preheader = L->getLoopPreheader();
  BasicBlock *Header    = L->getHeader();
  BasicBlock *Latch     = L->getLoopLatch();

  for (BasicBlock *BB : Blocks) {
    std::string Name = BB->hasName() ? BB->getName().str()
                                     : ("bb" + std::to_string(BBId[BB]));

    SmallVector<StringRef, 4> Tags;
    const char *Fill;
    if (BB == Preheader) {
      Tags.push_back("preheader"); Fill = "#caffbf";
    } else if (!L->contains(BB)) {
      Tags.push_back("exit");      Fill = "#ffadad";
    } else if (BB == Header) {
      Tags.push_back("header");    Fill = "#a0c4ff";
      if (BB == Latch)        Tags.push_back("latch");
      if (L->isLoopExiting(BB)) Tags.push_back("exiting");
    } else if (BB == Latch) {
      Tags.push_back("latch");     Fill = "#ffd6a5";
      if (L->isLoopExiting(BB)) Tags.push_back("exiting");
    } else {
      Tags.push_back("body");      Fill = "#fdffb6";
      if (L->isLoopExiting(BB)) Tags.push_back("exiting");
    }

    std::string Lbl = Name + " [";
    for (unsigned i = 0; i < Tags.size(); ++i) {
      if (i > 0) Lbl += ", ";
      Lbl += Tags[i].str();
    }
    Lbl += "]\\n" + std::to_string(BB->size()) + " insts";

    Instruction *T = BB->getTerminator();
    if (auto *Br = dyn_cast<BranchInst>(T))
      Lbl += Br->isConditional() ? "\\n(conditional br)" : "\\n(br)";
    else if (isa<ReturnInst>(T))
      Lbl += "\\n(ret)";

    OS << "  bb" << BBId[BB] << " [fillcolor=\"" << Fill
       << "\", label=\"" << dotEscape(Lbl) << "\"];\n";
  }

  // Edges.
  for (BasicBlock *BB : Blocks) {
    Instruction *T = BB->getTerminator();
    if (!T) continue;
    for (unsigned i = 0; i < T->getNumSuccessors(); ++i) {
      BasicBlock *Succ = T->getSuccessor(i);
      auto It = BBId.find(Succ);
      if (It == BBId.end()) continue;

      const char *Style = "";
      const char *Color = "black";
      // Back-edge from latch to header — distinguish visually.
      if (BB == Latch && Succ == Header) {
        Color = "darkgreen";
        Style = ", style=dashed";
      }

      std::string ELabel;
      if (auto *Br = dyn_cast<BranchInst>(T))
        if (Br->isConditional())
          ELabel = (i == 0) ? "T" : "F";

      OS << "  bb" << BBId[BB] << " -> bb" << It->second
         << " [color=\"" << Color << "\"" << Style;
      if (!ELabel.empty()) OS << ", label=\"" << ELabel << "\"";
      OS << "];\n";
    }
  }
  OS << "}\n";
}

// Write the SCC-DAG with stage coloring.
static void writePartitionDOT(const SCCDAG &DAG, const SCCResult &SCC,
                              const Partition &P, StringRef Path,
                              StringRef GraphName) {
  std::error_code EC;
  raw_fd_ostream OS(Path, EC, sys::fs::OF_Text);
  if (EC) {
    errs() << "DSWPAnalyzer: cannot open " << Path << ": "
           << EC.message() << "\n";
    return;
  }

  static const char *StageColors[] = { "#a0c4ff", "#ffadad" };

  OS << "digraph \"partition_" << GraphName.str() << "\" {\n";
  OS << "  rankdir=TB;\n";
  OS << "  node [shape=box, style=\"filled,rounded\","
        " fontname=\"monospace\", fontsize=10];\n";
  OS << "  edge [fontname=\"monospace\", fontsize=8];\n";

  for (unsigned i = 0; i < DAG.numSCCs(); ++i) {
    unsigned Stage = P.SingleStage ? 0 : P.Stage[i];
    const char *Fill = StageColors[Stage % 2];
    OS << "  scc" << i
       << " [fillcolor=\"" << Fill << "\","
       << " label=\"SCC " << i
       << (DAG.Cyclic[i] ? " (cyclic)" : "")
       << "\\n" << DAG.InstCount[i] << " insts, cost=" << DAG.Cost[i]
       << "\\nstage " << Stage << "\"];\n";
  }

  for (unsigned src = 0; src < DAG.numSCCs(); ++src) {
    for (const SCCDAGEdge &E : DAG.Out[src]) {
      // Build a compact edge label.
      std::string Tag;
      if (E.HasData)    Tag += "d";
      if (E.HasMemory)  Tag += "m";
      if (E.HasControl) Tag += "c";
      if (E.LoopCarried) Tag += "*";

      bool Cross = !P.SingleStage && (P.Stage[src] != P.Stage[E.ToSCC]);
      const char *Color = Cross ? "darkgreen" : "gray40";
      OS << "  scc" << src << " -> scc" << E.ToSCC
         << " [label=\"" << Tag;
      if (E.Count > 1) OS << "x" << E.Count;
      OS << "\", color=\"" << Color << "\""
         << (Cross ? ", penwidth=2" : "")
         << (E.LoopCarried ? ", style=dashed" : "")
         << "];\n";
    }
  }

  OS << "  // total=" << P.TotalCost
     << " s0=" << P.Stage0Cost() << " s1=" << P.Stage1Cost()
     << " cross=" << P.CrossStageEdges
     << " upper=" << format("%.2fx", P.estSpeedupUpperBound())
     << "\n";
  OS << "}\n";
}

static void writeReportJSON(StringRef Path, StringRef FuncName, unsigned LoopIdx,
                            const PDG &G, const SCCResult &SCC,
                            const SCCDAG &DAG, const Partition &P) {
  std::error_code EC;
  raw_fd_ostream OS(Path, EC, sys::fs::OF_Text);
  if (EC) {
    errs() << "DSWPAnalyzer: cannot open " << Path << ": "
           << EC.message() << "\n";
    return;
  }

  auto countCyclic = [](const SCCResult &S) {
    unsigned N = 0;
    for (bool C : S.IsCyclic) if (C) N++;
    return N;
  };

  OS << "{\n";
  OS << "  \"function\": \"" << FuncName << "\",\n";
  OS << "  \"loop_idx\": " << LoopIdx << ",\n";
  OS << "  \"pdg\": {\n";
  OS << "    \"nodes\": " << G.numNodes() << ",\n";
  OS << "    \"edges\": " << G.numEdges() << "\n";
  OS << "  },\n";
  OS << "  \"sccs\": {\n";
  OS << "    \"count\": " << SCC.SCCs.size() << ",\n";
  OS << "    \"cyclic\": " << countCyclic(SCC) << "\n";
  OS << "  },\n";
  OS << "  \"partition\": {\n";
  OS << "    \"single_stage\": " << (P.SingleStage ? "true" : "false") << ",\n";
  OS << "    \"total_cost\": " << P.TotalCost << ",\n";
  OS << "    \"stage_0_cost\": " << P.Stage0Cost() << ",\n";
  OS << "    \"stage_1_cost\": " << P.Stage1Cost() << ",\n";
  OS << "    \"cross_stage_edges\": " << P.CrossStageEdges << ",\n";
  OS << "    \"est_speedup_upper_bound\": "
     << format("%.3f", P.estSpeedupUpperBound()) << ",\n";

  // SCC membership per stage.
  for (unsigned stage = 0; stage < 2; ++stage) {
    OS << "    \"stage_" << stage << "\": [";
    bool First = true;
    for (unsigned i = 0; i < DAG.numSCCs(); ++i) {
      unsigned S = P.SingleStage ? 0 : P.Stage[i];
      if (S != stage) continue;
      if (!First) OS << ", ";
      First = false;
      OS << "{\"scc\": " << i
         << ", \"cost\": " << DAG.Cost[i]
         << ", \"insts\": " << DAG.InstCount[i]
         << ", \"cyclic\": " << (DAG.Cyclic[i] ? "true" : "false")
         << "}";
    }
    OS << "]" << (stage == 0 ? "," : "") << "\n";
  }
  OS << "  }\n";
  OS << "}\n";
}

static void writeDOT(const PDG &G, const SCCResult &SCC, StringRef Path,
                     StringRef GraphName) {
  std::error_code EC;
  raw_fd_ostream OS(Path, EC, sys::fs::OF_Text);
  if (EC) {
    errs() << "DSWPAnalyzer: cannot open " << Path << ": "
           << EC.message() << "\n";
    return;
  }

  // Assign a small id per node for readable labels.
  DenseMap<Instruction *, unsigned> NodeId;
  for (Instruction *I : G.nodes())
    NodeId[I] = NodeId.size();

  OS << "digraph \"" << dotEscape(GraphName) << "\" {\n";
  OS << "  rankdir=TB;\n";
  OS << "  node [shape=box, style=\"filled,rounded\", fontname=\"monospace\","
        " fontsize=10];\n";
  OS << "  edge [fontname=\"monospace\", fontsize=8];\n";

  // Group nodes by SCC. Cyclic multi-instruction SCCs get a dashed
  // cluster box (visually obvious recurrence). Singletons are emitted
  // as bare nodes — but every node label is prefixed with its SCC id
  // so the PDG diagram maps 1:1 against the partition diagram (which
  // labels every SCC numerically, including singletons).
  auto emitNode = [&](Instruction *I, unsigned SccIdx, const char *Fill,
                       const char *Indent) {
    std::string Body = instLabel(I, NodeId[I]);
    std::string Label = "scc " + std::to_string(SccIdx) + " | " + Body;
    OS << Indent << "n" << NodeId[I]
       << " [fillcolor=\"" << Fill << "\","
       << " label=\"" << dotEscape(Label) << "\"];\n";
  };
  for (unsigned i = 0; i < SCC.SCCs.size(); ++i) {
    const auto &Comp = SCC.SCCs[i];
    bool Cyclic = SCC.IsCyclic[i];
    const char *Fill = sccColor(i, Cyclic);
    if (Cyclic && Comp.size() > 1) {
      OS << "  subgraph cluster_scc" << i << " {\n";
      OS << "    label=\"SCC " << i << " (cyclic, " << Comp.size()
         << " insts)\";\n";
      OS << "    style=\"rounded,dashed\"; color=gray50;\n";
      for (Instruction *I : Comp)
        emitNode(I, i, Fill, "    ");
      OS << "  }\n";
    } else {
      for (Instruction *I : Comp)
        emitNode(I, i, Fill, "  ");
    }
  }

  // Edges.
  for (Instruction *I : G.nodes()) {
    for (const PDGEdge &E : G.outEdges(I)) {
      OS << "  n" << NodeId[E.From] << " -> n" << NodeId[E.To]
         << " [color=\"" << edgeKindColor(E.Kind) << "\""
         << ", label=\"" << edgeKindName(E.Kind);
      if (E.LoopCarried) OS << "*";
      OS << "\"";
      if (E.LoopCarried) OS << ", style=dashed";
      OS << "];\n";
    }
  }

  OS << "}\n";
}

struct DSWPAnalyzer : public PassInfoMixin<DSWPAnalyzer> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
    if (F.isDeclaration())
      return PreservedAnalyses::all();

    auto &LI    = FAM.getResult<LoopAnalysis>(F);
    if (LI.empty())
      return PreservedAnalyses::all();

    auto &AA    = FAM.getResult<AAManager>(F);
    auto &MSSA  = FAM.getResult<MemorySSAAnalysis>(F).getMSSA();
    auto &DI    = FAM.getResult<DependenceAnalysis>(F);
    auto &PDT   = FAM.getResult<PostDominatorTreeAnalysis>(F);

    // Ensure report directory exists.
    if (auto EC = sys::fs::create_directories(ReportDir.getValue())) {
      errs() << "DSWPAnalyzer: could not create " << ReportDir << ": "
             << EC.message() << "\n";
    }

    errs() << "=== DSWPAnalyzer: function '" << F.getName() << "' ===\n";

    unsigned LoopIdx = 0;
    SmallVector<Loop *, 8> Worklist(LI.begin(), LI.end());
    while (!Worklist.empty()) {
      Loop *L = Worklist.pop_back_val();
      for (Loop *Sub : L->getSubLoops())
        Worklist.push_back(Sub);

      std::string Why;
      if (!isCandidateLoop(L, &Why)) {
        errs() << "  loop[" << LoopIdx << "] SKIP (" << Why << ")\n";
        LoopIdx++;
        continue;
      }

      PDGBuilder Builder(L, LI, AA, MSSA, DI, PDT);
      auto G = Builder.build();
      // Two SCC views:
      //   Full       - includes control edges (program-correctness view)
      //   Data-only  - data + memory only (DSWP partitioning view; branches
      //                are duplicatable across stages so control deps don't
      //                force co-location).
      auto SCCFull = computeSCCs(*G, /*IncludeControl=*/true);
      auto SCCData = computeSCCs(*G, /*IncludeControl=*/false);

      auto countCyclic = [](const SCCResult &S) {
        unsigned N = 0;
        for (bool C : S.IsCyclic) if (C) N++;
        return N;
      };
      auto largestSize = [](const SCCResult &S) {
        unsigned N = 0;
        for (auto &C : S.SCCs) N = std::max(N, (unsigned)C.size());
        return N;
      };

      errs() << "  loop[" << LoopIdx << "] OK"
             << " nodes=" << G->numNodes()
             << " edges=" << G->numEdges()
             << " | full-sccs=" << SCCFull.SCCs.size()
             << " cyclic=" << countCyclic(SCCFull)
             << " largest=" << largestSize(SCCFull)
             << " | data-sccs=" << SCCData.SCCs.size()
             << " cyclic=" << countCyclic(SCCData)
             << " largest=" << largestSize(SCCData)
             << "\n";

      // Build SCC-DAG and partition (data-only SCCs are the partitioning
      // granularity; control deps are handled by branch replication later).
      SCCDAG DAG = buildSCCDAG(*G, SCCData);
      Partition Part = partition2Stage(DAG);

      errs() << "    partition: total=" << Part.TotalCost
             << " s0=" << Part.Stage0Cost()
             << " s1=" << Part.Stage1Cost()
             << " cross=" << Part.CrossStageEdges
             << " single=" << (Part.SingleStage ? "yes" : "no")
             << " upper=" << format("%.2fx", Part.estSpeedupUpperBound())
             << "\n";

      // Write artifacts.
      std::string Base =
          slug(F.getName().str()) + "_loop" + std::to_string(LoopIdx);
      std::string GraphName = F.getName().str() + ":" + std::to_string(LoopIdx);

      SmallString<128> PdgPath(ReportDir.getValue());
      sys::path::append(PdgPath, Base + "_pdg.dot");
      writeDOT(*G, SCCData, PdgPath, GraphName);

      SmallString<128> PartPath(ReportDir.getValue());
      sys::path::append(PartPath, Base + "_partition.dot");
      writePartitionDOT(DAG, SCCData, Part, PartPath, GraphName);

      SmallString<128> CfgPath(ReportDir.getValue());
      sys::path::append(CfgPath, Base + "_cfg.dot");
      writeCFGDOT(L, CfgPath, GraphName);

      SmallString<128> JsonPath(ReportDir.getValue());
      sys::path::append(JsonPath, Base + ".json");
      writeReportJSON(JsonPath, F.getName(), LoopIdx, *G, SCCData, DAG, Part);

      errs() << "    -> " << PdgPath << "\n";
      errs() << "    -> " << PartPath << "\n";
      errs() << "    -> " << CfgPath << "\n";
      errs() << "    -> " << JsonPath << "\n";

      LoopIdx++;
    }

    errs() << "\n";
    return PreservedAnalyses::all();
  }

  static bool isRequired() { return true; }
};

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {
    LLVM_PLUGIN_API_VERSION, "DSWPAnalyzer", LLVM_VERSION_STRING,
    [](PassBuilder &PB) {
      PB.registerPipelineParsingCallback(
        [](StringRef Name, FunctionPassManager &FPM,
           ArrayRef<PassBuilder::PipelineElement>) {
          if (Name == "dswp-analyze") {
            FPM.addPass(DSWPAnalyzer());
            return true;
          }
          return false;
        });
    }};
}
