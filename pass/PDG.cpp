//===-- PDG.cpp - Program Dependence Graph implementation ---------------===//

#include "PDG.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"

#include <algorithm>
#include <functional>

using namespace llvm;
using namespace dswp;

const char *dswp::edgeKindName(EdgeKind K) {
  switch (K) {
  case EdgeKind::RegData: return "data";
  case EdgeKind::Mem:     return "mem";
  case EdgeKind::Ctrl:    return "ctrl";
  }
  return "?";
}

const char *dswp::edgeKindColor(EdgeKind K) {
  switch (K) {
  case EdgeKind::RegData: return "black";
  case EdgeKind::Mem:     return "red";
  case EdgeKind::Ctrl:    return "blue";
  }
  return "gray";
}

//===----------------------------------------------------------------------===//
// PDG
//===----------------------------------------------------------------------===//

void PDG::addNode(Instruction *I) {
  if (NodeSet.insert(I).second)
    Nodes.push_back(I);
}

void PDG::addEdge(Instruction *From, Instruction *To, EdgeKind Kind,
                  bool LoopCarried) {
  // Self-edges are sometimes interesting (e.g., a store that aliases itself
  // across iterations) but for register data we skip pure self-edges that
  // arise from PHIs aliased to themselves.
  if (From == To && Kind == EdgeKind::RegData)
    return;
  Out[From].push_back({From, To, Kind, LoopCarried});
}

size_t PDG::numEdges() const {
  size_t N = 0;
  for (auto &Kv : Out) N += Kv.second.size();
  return N;
}

//===----------------------------------------------------------------------===//
// PDGBuilder
//===----------------------------------------------------------------------===//

bool PDGBuilder::isInLoop(Instruction *I) const {
  return I && L->contains(I->getParent());
}

std::unique_ptr<PDG> PDGBuilder::build() {
  auto G = std::make_unique<PDG>();

  // Add every instruction in the loop as a node.
  for (BasicBlock *BB : L->blocks())
    for (Instruction &I : *BB)
      G->addNode(&I);

  addRegisterDataEdges(*G);
  addMemoryEdges(*G);
  addControlEdges(*G);

  return G;
}

// SSA def-use within the loop. PHI nodes whose incoming value comes from the
// loop latch are loop-carried (they flow value from iteration i to i+1).
void PDGBuilder::addRegisterDataEdges(PDG &G) {
  BasicBlock *Header = L->getHeader();
  BasicBlock *Latch = L->getLoopLatch();

  for (BasicBlock *BB : L->blocks()) {
    for (Instruction &I : *BB) {
      // For each user of I that is also in the loop, add a data edge.
      for (User *U : I.users()) {
        auto *UI = dyn_cast<Instruction>(U);
        if (!isInLoop(UI))
          continue;

        bool Carried = false;
        if (auto *PHI = dyn_cast<PHINode>(UI)) {
          // Header PHI taking value from the latch is loop-carried.
          if (PHI->getParent() == Header) {
            for (unsigned i = 0, e = PHI->getNumIncomingValues(); i != e; ++i) {
              if (PHI->getIncomingValue(i) == &I &&
                  PHI->getIncomingBlock(i) == Latch) {
                Carried = true;
                break;
              }
            }
          }
        }
        G.addEdge(&I, UI, EdgeKind::RegData, Carried);
      }
    }
  }
}

// Memory dependences inside the loop. We collect every memory-touching
// instruction in the loop, then for each ordered pair (I, J) where I appears
// before J in iteration order, query DependenceInfo. We add an edge whenever
// `depends` returns a non-null result (i.e. a possible dependence). The
// LoopCarried flag is set if the dependence has a non-zero direction at the
// loop's nesting level (or ANY level, conservatively).
void PDGBuilder::addMemoryEdges(PDG &G) {
  SmallVector<Instruction *, 16> Mems;
  for (BasicBlock *BB : L->blocks())
    for (Instruction &I : *BB)
      if (I.mayReadOrWriteMemory())
        Mems.push_back(&I);

  for (Instruction *Src : Mems) {
    for (Instruction *Dst : Mems) {
      // No memory ordering between two pure reads, even across iterations.
      if (!Src->mayWriteToMemory() && !Dst->mayWriteToMemory())
        continue;

      if (Src == Dst) {
        // Self memory dep across iterations only matters if Src writes
        // (store-store or load reordering with same store).
        auto Dep = DI.depends(Src, Dst, /*PossiblyLoopIndependent=*/false);
        if (Dep && !Dep->isLoopIndependent())
          G.addEdge(Src, Dst, EdgeKind::Mem, /*LoopCarried=*/true);
        continue;
      }

      auto Dep = DI.depends(Src, Dst, /*PossiblyLoopIndependent=*/true);
      if (!Dep)
        continue;

      bool Carried = !Dep->isLoopIndependent();
      G.addEdge(Src, Dst, EdgeKind::Mem, Carried);
    }
  }
}

// Control dependence (Cytron et al.):
//   B is control-dep on A iff
//     (1) A has ≥2 successors,
//     (2) ∃ successor S of A such that B post-dominates S (or S == B),
//     (3) B does not strictly post-dominate A.
// Edge: from terminator(A) to every instruction in B.
void PDGBuilder::addControlEdges(PDG &G) {
  // Collect all (A, B) pairs where A is in the loop and has a conditional
  // terminator, and B is some loop block control-dep on A.
  for (BasicBlock *A : L->blocks()) {
    Instruction *Term = A->getTerminator();
    if (!Term) continue;
    if (Term->getNumSuccessors() < 2) continue;

    for (BasicBlock *B : L->blocks()) {
      if (B == A) continue;
      if (PDT.properlyDominates(B, A)) continue; // B postdoms A → not ctrl-dep

      bool Found = false;
      for (BasicBlock *S : successors(A)) {
        if (S == B || PDT.dominates(B, S)) {
          Found = true;
          break;
        }
      }
      if (!Found) continue;

      // Add ctrl edge from A's terminator to every instruction in B.
      // Loop-carried iff A is the latch (or B reaches the latch only via A
      // crossing the back-edge). For our innermost-simplified loops, the
      // latch's branch is unconditional so this rarely fires; we keep the
      // flag false here. A more precise classifier can come later.
      for (Instruction &I : *B) {
        if (isa<PHINode>(I)) continue; // PHIs are not ctrl-dep on br
        G.addEdge(Term, &I, EdgeKind::Ctrl, /*LoopCarried=*/false);
      }
    }
  }
}

//===----------------------------------------------------------------------===//
// Tarjan's SCC.
//===----------------------------------------------------------------------===//

SCCResult dswp::computeSCCs(const PDG &G, bool IncludeControl) {
  SCCResult R;

  DenseMap<Instruction *, int> Index;
  DenseMap<Instruction *, int> LowLink;
  DenseSet<Instruction *> OnStack;
  std::vector<Instruction *> Stack;
  int Counter = 0;

  std::function<void(Instruction *)> StrongConnect = [&](Instruction *V) {
    Index[V]   = Counter;
    LowLink[V] = Counter;
    Counter++;
    Stack.push_back(V);
    OnStack.insert(V);

    for (const PDGEdge &E : G.outEdges(V)) {
      if (!IncludeControl && E.Kind == EdgeKind::Ctrl)
        continue;
      Instruction *W = E.To;
      auto It = Index.find(W);
      if (It == Index.end()) {
        StrongConnect(W);
        LowLink[V] = std::min(LowLink[V], LowLink[W]);
      } else if (OnStack.count(W)) {
        LowLink[V] = std::min(LowLink[V], It->second);
      }
    }

    if (LowLink[V] == Index[V]) {
      std::vector<Instruction *> Component;
      while (true) {
        Instruction *W = Stack.back();
        Stack.pop_back();
        OnStack.erase(W);
        Component.push_back(W);
        if (W == V) break;
      }
      unsigned Id = R.SCCs.size();
      bool Cyclic = Component.size() > 1;
      if (!Cyclic) {
        // Singleton with self-edge?
        for (const PDGEdge &E : G.outEdges(Component[0])) {
          if (!IncludeControl && E.Kind == EdgeKind::Ctrl) continue;
          if (E.To == Component[0]) { Cyclic = true; break; }
        }
      }
      for (Instruction *I : Component)
        R.InstToSCC[I] = Id;
      R.SCCs.push_back(std::move(Component));
      R.IsCyclic.push_back(Cyclic);
    }
  };

  for (Instruction *I : G.nodes())
    if (!Index.count(I))
      StrongConnect(I);

  return R;
}
