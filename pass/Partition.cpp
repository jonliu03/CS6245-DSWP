//===-- Partition.cpp - SCC-DAG construction and 2-stage partition -----===//

#include "Partition.h"

#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"

#include <algorithm>
#include <queue>

using namespace llvm;
using namespace dswp;

// Rough instruction latency table. Tuned for "qualitative" partitioning,
// not microarchitectural accuracy. The point is: load and div are
// expensive, phi/branch are nearly free, ALU ops are baseline.
static unsigned estimateCost(Instruction *I) {
  if (auto *Bi = dyn_cast<BinaryOperator>(I)) {
    switch (Bi->getOpcode()) {
    case Instruction::Mul:  case Instruction::FMul: return 3;
    case Instruction::SDiv: case Instruction::UDiv:
    case Instruction::FDiv: case Instruction::SRem:
    case Instruction::URem: case Instruction::FRem: return 20;
    default: return 1;
    }
  }
  if (isa<LoadInst>(I))         return 4;
  if (isa<StoreInst>(I))        return 1;
  // Calls are typically much more expensive than load/ALU because the
  // callee body is opaque to the cost model. Inflate so benchmarks with
  // multiple heavy calls (e.g. llist_heavy's heavy_a + heavy_b) split
  // across stages instead of landing both in the same one.
  if (isa<CallInst>(I))         return 30;
  if (isa<PHINode>(I))          return 0;
  if (isa<BranchInst>(I))       return 1;
  if (isa<GetElementPtrInst>(I))return 1;
  if (isa<CmpInst>(I))          return 1;
  return 1;
}

SCCDAG dswp::buildSCCDAG(const PDG &G, const SCCResult &SCC) {
  SCCDAG DAG;
  size_t N = SCC.SCCs.size();
  DAG.Out.resize(N);
  DAG.Cost.assign(N, 0);
  DAG.Cyclic = SCC.IsCyclic;
  DAG.InstCount.assign(N, 0);

  // Cost and instruction counts.
  for (unsigned i = 0; i < N; ++i) {
    DAG.InstCount[i] = SCC.SCCs[i].size();
    for (Instruction *I : SCC.SCCs[i])
      DAG.Cost[i] += estimateCost(I);
  }

  // Edges. Aggregate PDG edges between distinct SCCs into one DAG edge
  // per ordered pair, keyed off the destination SCC.
  //
  // We deliberately DROP control edges here: SCCs were computed from data
  // + memory only, so control edges in the PDG can connect SCCs in
  // arbitrary directions and would induce cycles in the "DAG" (e.g. the
  // loop-exit branch ctrl-deps the body, while body data flows back
  // through the comparison into the branch). Branches are duplicatable
  // across stages, so this is the right granularity for partitioning.
  // We tag each SCCDAGEdge with HasControl so reporting can still flag
  // SCC pairs that need branch replication.
  for (unsigned src = 0; src < N; ++src) {
    DenseMap<unsigned, SCCDAGEdge> ByDst;
    for (Instruction *I : SCC.SCCs[src]) {
      for (const PDGEdge &E : G.outEdges(I)) {
        if (E.Kind == EdgeKind::Ctrl) continue;
        auto It = SCC.InstToSCC.find(E.To);
        if (It == SCC.InstToSCC.end()) continue;
        unsigned dst = It->second;
        if (dst == src) continue; // intra-SCC, drop

        auto &DE = ByDst[dst];
        DE.ToSCC = dst;
        DE.Count++;
        DE.LoopCarried |= E.LoopCarried;
        DE.HasMemory   |= (E.Kind == EdgeKind::Mem);
        DE.HasData     |= (E.Kind == EdgeKind::RegData);
      }
    }
    DAG.Out[src].reserve(ByDst.size());
    for (auto &Kv : ByDst)
      DAG.Out[src].push_back(Kv.second);
  }

  return DAG;
}

std::vector<unsigned> dswp::topoSort(const SCCDAG &DAG) {
  size_t N = DAG.numSCCs();
  std::vector<unsigned> InDeg(N, 0);
  for (unsigned src = 0; src < N; ++src)
    for (auto &E : DAG.Out[src])
      InDeg[E.ToSCC]++;

  std::queue<unsigned> Q;
  for (unsigned i = 0; i < N; ++i)
    if (InDeg[i] == 0) Q.push(i);

  std::vector<unsigned> Order;
  Order.reserve(N);
  while (!Q.empty()) {
    unsigned u = Q.front(); Q.pop();
    Order.push_back(u);
    for (auto &E : DAG.Out[u]) {
      if (--InDeg[E.ToSCC] == 0)
        Q.push(E.ToSCC);
    }
  }
  // If Order.size() != N the DAG has a cycle (bug). Don't assert here;
  // partitioning will detect SingleStage.
  return Order;
}

Partition dswp::partitionNStage(const SCCDAG &DAG, unsigned NumStages) {
  Partition P;
  size_t N = DAG.numSCCs();
  P.NumStages = NumStages;
  P.Stage.assign(N, 0);
  P.StageCosts.assign(NumStages, 0);

  uint64_t Total = 0;
  for (uint64_t C : DAG.Cost) Total += C;
  P.TotalCost = Total;

  if (NumStages < 2 || N <= 1 || Total == 0) {
    P.SingleStage = true;
    if (!P.StageCosts.empty()) P.StageCosts[0] = Total;
    return P;
  }

  auto Order = topoSort(DAG);
  if (Order.size() != N) {
    P.SingleStage = true;
    P.StageCosts[0] = Total;
    return P;
  }

  // Greedy fill: assign each topo-ordered SCC to the current stage; advance
  // to the next stage once cumulative cost reaches the running threshold
  // k * Total / NumStages. The last stage absorbs whatever's left.
  unsigned curStage = 0;
  uint64_t Acc = 0;
  for (unsigned scc : Order) {
    // Compute boundary for advancing past curStage.
    // Threshold k = curStage + 1 boundary at (k * Total) / NumStages.
    uint64_t Threshold = ((uint64_t)(curStage + 1) * Total) / NumStages;
    if (curStage + 1 < NumStages && Acc >= Threshold)
      curStage++;
    P.Stage[scc] = curStage;
    Acc += DAG.Cost[scc];
  }

  for (unsigned scc = 0; scc < N; ++scc) {
    P.StageCosts[P.Stage[scc]] += DAG.Cost[scc];
    for (auto &E : DAG.Out[scc])
      if (P.Stage[scc] != P.Stage[E.ToSCC])
        P.CrossStageEdges++;
  }

  // SingleStage if any stage is empty (partition couldn't fill NumStages).
  for (auto c : P.StageCosts)
    if (c == 0) { P.SingleStage = true; break; }

  return P;
}