//===-- PDG.h - Program Dependence Graph for DSWP ------------------------===//
//
// Per-loop PDG with three edge kinds (register data, memory, control), each
// flagged loop-carried or not. Built by PDGBuilder; consumed by SCC + DOT.
//
//===---------------------------------------------------------------------===//

#ifndef DSWP_PDG_H
#define DSWP_PDG_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/DependenceAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Instruction.h"

#include <memory>
#include <vector>

namespace dswp {

enum class EdgeKind : unsigned char { RegData = 0, Mem = 1, Ctrl = 2 };

const char *edgeKindName(EdgeKind K);
const char *edgeKindColor(EdgeKind K);

struct PDGEdge {
  llvm::Instruction *From;
  llvm::Instruction *To;
  EdgeKind Kind;
  bool LoopCarried;
};

class PDG {
public:
  void addNode(llvm::Instruction *I);
  void addEdge(llvm::Instruction *From, llvm::Instruction *To, EdgeKind Kind,
               bool LoopCarried);

  llvm::ArrayRef<llvm::Instruction *> nodes() const { return Nodes; }

  llvm::ArrayRef<PDGEdge> outEdges(llvm::Instruction *I) const {
    auto It = Out.find(I);
    if (It == Out.end())
      return {};
    return It->second;
  }

  size_t numNodes() const { return Nodes.size(); }
  size_t numEdges() const;

private:
  std::vector<llvm::Instruction *> Nodes;
  llvm::DenseSet<llvm::Instruction *> NodeSet;
  llvm::DenseMap<llvm::Instruction *, llvm::SmallVector<PDGEdge, 4>> Out;
};

class PDGBuilder {
public:
  PDGBuilder(llvm::Loop *L, llvm::LoopInfo &LI, llvm::AAResults &AA,
             llvm::MemorySSA &MSSA, llvm::DependenceInfo &DI,
             llvm::PostDominatorTree &PDT)
      : L(L), LI(LI), AA(AA), MSSA(MSSA), DI(DI), PDT(PDT) {}

  std::unique_ptr<PDG> build();

private:
  llvm::Loop *L;
  llvm::LoopInfo &LI;
  llvm::AAResults &AA;
  llvm::MemorySSA &MSSA;
  llvm::DependenceInfo &DI;
  llvm::PostDominatorTree &PDT;

  bool isInLoop(llvm::Instruction *I) const;

  void addRegisterDataEdges(PDG &G);
  void addMemoryEdges(PDG &G);
  void addControlEdges(PDG &G);
};

// Tarjan's SCC. Returns SCCs in reverse topological order (consumers before
// producers); we flip later for partitioning.
struct SCCResult {
  // SCCs[i] = list of instructions in SCC i.
  std::vector<std::vector<llvm::Instruction *>> SCCs;
  // Reverse map: instruction -> SCC index.
  llvm::DenseMap<llvm::Instruction *, unsigned> InstToSCC;
  // Did this SCC contain a cycle (size > 1, OR self-loop)?
  std::vector<bool> IsCyclic;
};

// Compute SCCs over the PDG. If `IncludeControl` is false, control-dep edges
// are ignored — this is the right granularity for DSWP partitioning, since
// branches are duplicatable across stages.
SCCResult computeSCCs(const PDG &G, bool IncludeControl = true);

} // namespace dswp

#endif // DSWP_PDG_H
