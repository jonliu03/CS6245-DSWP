//===-- Partition.h - SCC-DAG and 2-stage DSWP partition ---------------===//
//
// Coalesces an SCCResult into a DAG, estimates per-SCC cost, and produces
// a 2-stage partition by topo-walking and accumulating to ~50% of total.
// The DAG is acyclic by construction (every cycle lives inside one SCC),
// so any prefix/suffix split is legal — no edges go from stage 1 back to
// stage 0.
//
//===---------------------------------------------------------------------===//

#ifndef DSWP_PARTITION_H
#define DSWP_PARTITION_H

#include "PDG.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include <cstdint>
#include <vector>

namespace dswp {

struct SCCDAGEdge {
  unsigned ToSCC;
  bool LoopCarried;       // any underlying PDG edge was loop-carried
  bool HasControl;        // any underlying edge was a control dep
  bool HasMemory;         // any underlying edge was a memory dep
  bool HasData;           // any underlying edge was a register data dep
  unsigned Count;         // multiplicity of underlying PDG edges
};

struct SCCDAG {
  // Indexed by SCC id (matches SCCResult).
  std::vector<llvm::SmallVector<SCCDAGEdge, 2>> Out;
  std::vector<uint64_t> Cost;        // per-SCC cost
  std::vector<bool> Cyclic;
  std::vector<unsigned> InstCount;
  size_t numSCCs() const { return Out.size(); }
};

// Build the SCC-DAG from the PDG and an SCCResult. Edges within an SCC
// are dropped; edges between SCCs are aggregated into one SCCDAGEdge per
// (src, dst) pair. Cost is summed per-SCC using a fixed instruction
// latency table. Control edges in the PDG are included in the DAG so
// downstream consumers can see them, but they were excluded when
// computing the SCC partition (see PDG.h).
SCCDAG buildSCCDAG(const PDG &G, const SCCResult &SCC);

struct Partition {
  // Stage assignment per SCC id (0..NumStages-1). Empty if partitioning failed.
  std::vector<unsigned> Stage;
  // Cost per stage (size = NumStages). Convenience getters StageCost(0/1)
  // alias to legacy Stage0Cost/Stage1Cost users below.
  std::vector<uint64_t> StageCosts;
  uint64_t TotalCost  = 0;
  unsigned CrossStageEdges = 0;
  unsigned NumStages = 0;
  // All-in-one-stage fallback (no parallelism). Set when the DAG has only
  // one node, every SCC is a single self-cycle, every non-empty stage is
  // the same one, etc.
  bool SingleStage = false;

  // Legacy accessors — kept so existing analyzer reporting code still
  // compiles. Return 0 if the partition has fewer stages than the index.
  uint64_t Stage0Cost() const { return StageCosts.size() > 0 ? StageCosts[0] : 0; }
  uint64_t Stage1Cost() const { return StageCosts.size() > 1 ? StageCosts[1] : 0; }

  double estSpeedupUpperBound() const {
    if (StageCosts.empty()) return 1.0;
    uint64_t MaxStage = 0;
    for (auto c : StageCosts) if (c > MaxStage) MaxStage = c;
    if (MaxStage == 0) return 1.0;
    return double(TotalCost) / double(MaxStage);
  }
};

// N-stage greedy partition: topo-sort the SCC-DAG, accumulate SCCs into
// stage k until cumulative cost ≥ k * Total/N, then advance to stage k+1.
// `NumStages` >= 2. If the cost distribution can't fill all N stages
// (some end up empty), SingleStage is set.
Partition partitionNStage(const SCCDAG &DAG, unsigned NumStages);

// Convenience: 2-stage version (preserved for analyzer backward-compat).
inline Partition partition2Stage(const SCCDAG &DAG) {
  return partitionNStage(DAG, 2);
}

// Topological order of the SCC-DAG. Returns SCC indices in dependency
// order (sources before sinks).
std::vector<unsigned> topoSort(const SCCDAG &DAG);

} // namespace dswp

#endif // DSWP_PARTITION_H
