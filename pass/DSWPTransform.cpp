//===-- DSWPTransform.cpp - DSWP transform pass --------------------------===//
//
// Generic CFG-cloning DSWP transform. Drives stage generation off the
// analyzer's PDG/SCC/partition output: clones the loop region into N
// stage functions, surgically erases non-stage instructions, replaces
// cross-stage uses with queue dequeues / enqueues, and emits a driver
// that spawns the stages on pthreads.
//
// Stage count is read from DSWP_NUM_STAGES (default 2, capped at 8).
// A scaffolded 2-stage transform remains as a fallback for loops the
// cloned path can't handle.
//
//===---------------------------------------------------------------------===//

#include "llvm/IR/Attributes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/DependenceAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include "PDG.h"
#include "Partition.h"

#include <utility>
#include <vector>

using namespace llvm;

namespace {

// ─── Shared helpers ─────────────────────────────────────────────────

static void stripPurityAttrs(Function &F) {
  // LLVM 16 consolidated per-kind memory-effect attributes (ArgMemOnly,
  // InaccessibleMemOnly, InaccessibleMemOrArgMemOnly) into a single
  // string-keyed "memory" attribute. ReadNone / ReadOnly / WriteOnly
  // are still expressible as AttrKinds in 15 and 16; we strip both the
  // remaining legacy kinds and the unified "memory" string attribute.
  Attribute::AttrKind ToStrip[] = {
    Attribute::ReadNone,
    Attribute::ReadOnly,
    Attribute::WriteOnly,
    Attribute::WillReturn,
    Attribute::NoFree,
    Attribute::NoSync,
    Attribute::Speculatable,
  };
  for (auto K : ToStrip) F.removeFnAttr(K);
  F.removeFnAttr("memory");
}

static void eraseFunctionBody(Function &F) {
  for (BasicBlock &BB : F)
    for (Instruction &I : BB)
      if (!I.use_empty())
        I.replaceAllUsesWith(UndefValue::get(I.getType()));
  for (BasicBlock &BB : F) BB.dropAllReferences();
  while (!F.empty()) F.back().eraseFromParent();
}

// Runtime declarations shared by all targets. Does NOT include
// per-target types (ArgsTy, NodeTy) — those vary.
struct RT {
  LLVMContext &Ctx;
  Module      *M;
  Type        *VoidTy, *I1Ty, *I32Ty, *I64Ty, *DoubleTy;
  PointerType *PtrTy;

  FunctionCallee QueueCreate, QueueDestroy, Enqueue, SendEof, Dequeue;
  FunctionCallee PthreadCreate, PthreadJoin;
};

static RT makeRT(Module *M) {
  LLVMContext &Ctx = M->getContext();
  RT R{Ctx, M};
  R.VoidTy   = Type::getVoidTy(Ctx);
  R.I1Ty     = Type::getInt1Ty(Ctx);
  R.I32Ty    = Type::getInt32Ty(Ctx);
  R.I64Ty    = Type::getInt64Ty(Ctx);
  R.DoubleTy = Type::getDoubleTy(Ctx);
  R.PtrTy    = PointerType::get(Ctx, 0);

  auto decl = [&](StringRef N, Type *Ret, ArrayRef<Type *> A) {
    return M->getOrInsertFunction(N, FunctionType::get(Ret, A, false));
  };
  R.QueueCreate  = decl("dswp_queue_create",  R.PtrTy, {R.I64Ty});
  R.QueueDestroy = decl("dswp_queue_destroy", R.VoidTy, {R.PtrTy});
  R.Enqueue      = decl("dswp_enqueue",       R.VoidTy, {R.PtrTy, R.I64Ty});
  R.SendEof      = decl("dswp_send_eof",      R.VoidTy, {R.PtrTy});
  R.Dequeue      = decl("dswp_dequeue",       R.I1Ty,   {R.PtrTy, R.PtrTy});
  if (auto *F = dyn_cast<Function>(R.Dequeue.getCallee()))
    F->addRetAttr(Attribute::ZExt);
  R.PthreadCreate = decl("pthread_create", R.I32Ty,
                          {R.PtrTy, R.PtrTy, R.PtrTy, R.PtrTy});
  R.PthreadJoin   = decl("pthread_join",   R.I32Ty, {R.PtrTy, R.PtrTy});
  return R;
}

// Emit a multi-stage driver: allocate args, create N queues, spawn N+1
// threads (walker + N-1 compute stages + reducer), join all, destroy
// queues, return live-out.
//
// `ArgsTy` layout must be:
//   [0] = ptr  head
//   [1] = sum type (i64 or double)
//   [2..2+NQueues-1] = ptr queue_i
//
// `StageFns` must have NQueues+1 entries (one per stage).
static void emitDriver(Function &F, const RT &R,
                        StructType *ArgsTy, Type *SumTy,
                        unsigned NQueues,
                        ArrayRef<Function *> StageFns) {
  eraseFunctionBody(F);
  stripPurityAttrs(F);

  BasicBlock *Entry = BasicBlock::Create(R.Ctx, "dswp.entry", &F);
  IRBuilder<> B(Entry);

  AllocaInst *Args = B.CreateAlloca(ArgsTy, nullptr, "args");

  // args.head = F.arg(0)
  B.CreateStore(F.getArg(0), B.CreateStructGEP(ArgsTy, Args, 0));

  // args.sum_out = zero
  Value *SumGEP = B.CreateStructGEP(ArgsTy, Args, 1, "sum.addr");
  B.CreateStore(Constant::getNullValue(SumTy), SumGEP);

  // Create queues, store into args.
  SmallVector<Value *, 4> Queues;
  for (unsigned i = 0; i < NQueues; ++i) {
    Value *Q = B.CreateCall(R.QueueCreate,
                            {ConstantInt::get(R.I64Ty, 1024)});
    B.CreateStore(Q, B.CreateStructGEP(ArgsTy, Args, 2 + i));
    Queues.push_back(Q);
  }

  Value *Null = ConstantPointerNull::get(R.PtrTy);

  // Spawn threads.
  SmallVector<AllocaInst *, 8> TSlots;
  for (unsigned i = 0; i < StageFns.size(); ++i) {
    AllocaInst *T = B.CreateAlloca(R.PtrTy);
    B.CreateCall(R.PthreadCreate, {T, Null, StageFns[i], Args});
    TSlots.push_back(T);
  }

  // Join.
  for (auto *T : TSlots)
    B.CreateCall(R.PthreadJoin, {B.CreateLoad(R.PtrTy, T), Null});

  // Destroy queues.
  for (auto *Q : Queues) B.CreateCall(R.QueueDestroy, {Q});

  // Return live-out.
  Value *Result = B.CreateLoad(SumTy, SumGEP, "result");
  B.CreateRet(Result);
}



// ──────────────────────────────────────────────────────────────────────
// CFG-cloning generic 2-stage DSWP transform
// ──────────────────────────────────────────────────────────────────────
//
// Removes the hand-built loop scaffolding from transformGeneric2Stage.
// Instead, clones the original loop CFG into each stage function and
// surgically:
//   - erases instructions not assigned to this stage and not in the
//     replicated control / iv chain
//   - replaces cross-stage incoming uses with queue dequeues
//   - inserts queue enqueues after cross-stage outgoing producers
//   - redirects exit edges to a synthesized stage-exit block that
//     stores any live-out into the args struct, sends EOF on producer
//     queues, and returns
//
// "Replicated" SCCs = the SCC(s) containing each loop exit branch and
// every SCC they transitively depend on (BFS backward in the SCC-DAG).
// These get cloned into every stage so each thread can iterate
// independently — the queue carries data, not loop control.
//
// Restrictions still in place:
//   - 2 stages only
//   - single-exit loop in simplified form
//   - cross-edge producer must be a non-PHI 8-byte (i32/i64/double/ptr)
//   - cross-edge from a memory-SCC value-write isn't yet packaged
//
// Wider than the scaffolding path: handles counted loops, multi-block
// loop bodies, void return types, multi-arg functions, multiple
// cross-edges.

namespace cloned {

static llvm::DenseSet<unsigned>
findReplicatedSCCs(Loop *L, const dswp::SCCResult &SCC,
                   const dswp::SCCDAG &DAG) {
  llvm::DenseSet<unsigned> R;
  // Every branch / terminator in the loop is part of "loop control" and
  // must exist in every stage's clone — otherwise the cloned CFG would
  // have blocks without terminators after surgery.
  for (BasicBlock *BB : L->blocks()) {
    Instruction *T = BB->getTerminator();
    if (!T) continue;
    auto It = SCC.InstToSCC.find(T);
    if (It != SCC.InstToSCC.end()) R.insert(It->second);
  }
  // Reverse adjacency over the SCC-DAG.
  std::vector<SmallVector<unsigned, 2>> In(DAG.numSCCs());
  for (unsigned i = 0; i < DAG.numSCCs(); ++i)
    for (const auto &E : DAG.Out[i])
      In[E.ToSCC].push_back(i);
  SmallVector<unsigned, 8> Worklist(R.begin(), R.end());
  while (!Worklist.empty()) {
    unsigned x = Worklist.pop_back_val();
    for (unsigned p : In[x])
      if (R.insert(p).second) Worklist.push_back(p);
  }
  return R;
}

// -2 = not in any SCC (outside loop), -1 = replicated, >= 0 = stage id
static int sclStage(Instruction *I, const dswp::SCCResult &SCC,
                     const dswp::Partition &P,
                     const llvm::DenseSet<unsigned> &Repl) {
  auto It = SCC.InstToSCC.find(I);
  if (It == SCC.InstToSCC.end()) return -2;
  if (Repl.count(It->second)) return -1;
  return (int)P.Stage[It->second];
}

struct CrossEdge {
  Instruction *Producer;
  unsigned     FromStage;
  unsigned     ToStage;
};

static SmallVector<CrossEdge, 4>
findCrossEdges(Loop *L, const dswp::SCCResult &SCC,
               const dswp::Partition &P,
               const llvm::DenseSet<unsigned> &Repl) {
  SmallVector<CrossEdge, 4> Out;
  llvm::DenseSet<std::pair<Instruction *, unsigned>> Seen;
  for (BasicBlock *BB : L->blocks()) {
    for (Instruction &I : *BB) {
      int From = sclStage(&I, SCC, P, Repl);
      if (From < 0) continue;
      for (User *U : I.users()) {
        auto *UI = dyn_cast<Instruction>(U);
        if (!UI || !L->contains(UI)) continue;
        int To = sclStage(UI, SCC, P, Repl);
        if (To < 0 || To == From) continue;
        auto Key = std::make_pair(&I, (unsigned)To);
        if (Seen.insert(Key).second)
          Out.push_back({&I, (unsigned)From, (unsigned)To});
      }
    }
  }
  return Out;
}

// Build a stage function by cloning the loop region.
static Function *
generateStage(Function &OrigF, const RT &R, StructType *ArgsTy,
              unsigned NumOrigArgs, bool HasReturn,
              unsigned RetIdx, unsigned QStart,
              Loop *L, BasicBlock *OrigPreheader, BasicBlock *OrigExit,
              const dswp::SCCResult &SCC, const dswp::Partition &P,
              const llvm::DenseSet<unsigned> &Repl,
              ArrayRef<CrossEdge> Edges,
              unsigned StageId, StringRef Name) {
  LLVMContext &Ctx = R.Ctx;
  auto FT = FunctionType::get(R.PtrTy, {R.PtrTy}, false);
  Function *F = Function::Create(FT, GlobalValue::InternalLinkage, Name, R.M);
  Argument *ArgsP = F->getArg(0); ArgsP->setName("args");

  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", F);
  IRBuilder<> B(Entry);

  ValueToValueMapTy VMap;

  // Materialize original-arg loads in entry, map to original args.
  for (unsigned i = 0; i < NumOrigArgs; ++i) {
    Argument *OA = OrigF.getArg(i);
    Value *Slot = B.CreateStructGEP(ArgsTy, ArgsP, i);
    Value *V = B.CreateLoad(OA->getType(), Slot,
                            OA->hasName() ? OA->getName() : Twine("arg"));
    VMap[OA] = V;
  }

  // Load queue handles + per-edge i64 slots.
  SmallVector<Value *, 4> Queues;
  SmallVector<AllocaInst *, 4> Slots;
  for (size_t i = 0; i < Edges.size(); ++i) {
    Value *QSlot = B.CreateStructGEP(ArgsTy, ArgsP, QStart + i);
    Queues.push_back(B.CreateLoad(R.PtrTy, QSlot, ("q" + Twine(i)).str()));
    Slots.push_back(B.CreateAlloca(R.I64Ty, nullptr, ("slot" + Twine(i)).str()));
  }

  BasicBlock *StageExit = BasicBlock::Create(Ctx, "stage.exit", F);

  // Clone all loop blocks.
  SmallVector<BasicBlock *, 8> ClonedBlocks;
  for (BasicBlock *BB : L->blocks()) {
    BasicBlock *NB = CloneBasicBlock(BB, VMap, ".s" + Twine(StageId), F);
    VMap[BB] = NB;
    ClonedBlocks.push_back(NB);
  }
  if (OrigExit) VMap[OrigExit] = StageExit;

  // Branch entry → cloned header.
  BasicBlock *ClonedHeader = cast<BasicBlock>(VMap[L->getHeader()]);
  B.CreateBr(ClonedHeader);

  // Remap operands in cloned blocks (uses point at cloned defs / args-loads).
  for (BasicBlock *NB : ClonedBlocks)
    for (Instruction &I : *NB)
      RemapInstruction(&I, VMap,
                        RF_NoModuleLevelChanges | RF_IgnoreMissingLocals);

  // Retarget exit successors and rewrite preheader incoming edges in PHIs.
  for (BasicBlock *NB : ClonedBlocks) {
    Instruction *Term = NB->getTerminator();
    for (unsigned s = 0; s < Term->getNumSuccessors(); ++s)
      if (Term->getSuccessor(s) == OrigExit)
        Term->setSuccessor(s, StageExit);
  }
  for (BasicBlock *NB : ClonedBlocks) {
    for (Instruction &I : *NB) {
      auto *Phi = dyn_cast<PHINode>(&I);
      if (!Phi) break;
      for (unsigned i = 0, e = Phi->getNumIncomingValues(); i < e; ++i)
        if (Phi->getIncomingBlock(i) == OrigPreheader)
          Phi->setIncomingBlock(i, Entry);
    }
  }

  // Cross-edge index maps.
  llvm::DenseMap<Instruction *, unsigned> Incoming;
  llvm::DenseMap<Instruction *, SmallVector<unsigned, 2>> Outgoing;
  for (unsigned ei = 0; ei < Edges.size(); ++ei) {
    if (Edges[ei].ToStage   == StageId) Incoming[Edges[ei].Producer] = ei;
    if (Edges[ei].FromStage == StageId) Outgoing[Edges[ei].Producer].push_back(ei);
  }

  // Walk original loop instructions, surgery on their clones.
  SmallVector<Instruction *, 16> ToErase;
  for (BasicBlock *OrigBB : L->blocks()) {
    for (Instruction &OrigI : *OrigBB) {
      auto It = VMap.find(&OrigI);
      if (It == VMap.end()) continue;
      auto *NewI = dyn_cast_or_null<Instruction>(It->second);
      if (!NewI) continue;

      int Stg = sclStage(&OrigI, SCC, P, Repl);
      bool InThisStage = (Stg == (int)StageId) || (Stg == -1);

      if (InThisStage) {
        auto OIt = Outgoing.find(&OrigI);
        if (OIt == Outgoing.end()) continue;
        IRBuilder<> EB(NewI->getParent());
        if (isa<PHINode>(NewI))
          EB.SetInsertPoint(NewI->getParent()->getFirstNonPHI());
        else
          EB.SetInsertPoint(NewI->getNextNode());
        for (unsigned ei : OIt->second) {
          EB.CreateStore(NewI, Slots[ei]);
          Value *Packed = EB.CreateLoad(R.I64Ty, Slots[ei], "pck");
          EB.CreateCall(R.Enqueue, {Queues[ei], Packed});
        }
      } else {
        auto IIt = Incoming.find(&OrigI);
        if (IIt != Incoming.end()) {
          if (isa<PHINode>(NewI)) {
            errs() << "DSWPTransform(cloned): unsupported PHI cross-edge\n";
            F->eraseFromParent();
            return nullptr;
          }
          unsigned ei = IIt->second;
          IRBuilder<> EB(NewI);
          EB.CreateCall(R.Dequeue, {Queues[ei], Slots[ei]});
          Value *V = EB.CreateLoad(OrigI.getType(), Slots[ei], "deq");
          NewI->replaceAllUsesWith(V);
          ToErase.push_back(NewI);
        } else {
          ToErase.push_back(NewI);
        }
      }
    }
  }

  // Erase: replace any dangling uses with undef (other in-stage deletes
  // may not have happened yet), then erase. Reverse order = users before
  // producers, which avoids most undef substitution.
  for (auto It = ToErase.rbegin(); It != ToErase.rend(); ++It) {
    Instruction *I = *It;
    if (!I->use_empty())
      I->replaceAllUsesWith(UndefValue::get(I->getType()));
    I->eraseFromParent();
  }

  // Build stage.exit: send EOF on outgoing queues, store live-out, return.
  B.SetInsertPoint(StageExit);
  for (unsigned ei = 0; ei < Edges.size(); ++ei)
    if (Edges[ei].FromStage == StageId)
      B.CreateCall(R.SendEof, {Queues[ei]});

  if (HasReturn) {
    ReturnInst *OrigRet = nullptr;
    for (BasicBlock &BB : OrigF)
      if (auto *Ret = dyn_cast<ReturnInst>(BB.getTerminator())) {
        OrigRet = Ret; break;
      }
    if (OrigRet && OrigRet->getReturnValue()) {
      Value *RV = OrigRet->getReturnValue();
      while (auto *Phi = dyn_cast<PHINode>(RV)) {
        if (Phi->getNumIncomingValues() != 1) break;
        RV = Phi->getIncomingValue(0);
      }
      bool ShouldStore = false;
      Value *StoreV = nullptr;
      if (auto *DefI = dyn_cast<Instruction>(RV)) {
        int DStg = sclStage(DefI, SCC, P, Repl);
        if (DStg == (int)StageId || DStg == -1) {
          auto It = VMap.find(DefI);
          if (It != VMap.end() && It->second) {
            ShouldStore = true;
            StoreV = cast<Value>(It->second);
          }
        }
      } else if (isa<Constant>(RV) && StageId == 0) {
        ShouldStore = true;
        StoreV = RV;
      }
      if (ShouldStore && StoreV) {
        Value *RetSlot = B.CreateStructGEP(ArgsTy, ArgsP, RetIdx);
        B.CreateStore(StoreV, RetSlot);
      }
    }
  }
  B.CreateRet(ConstantPointerNull::get(R.PtrTy));

  // Suppress verifier output — failure here just means we'll bail and
  // let the scaffolding fallback (or no-op) handle the function.
  if (verifyFunction(*F, /*OS=*/nullptr)) {
    if (getenv("DSWP_VERBOSE")) {
      errs() << "DSWPTransform(cloned): verifyFunction failed for " << Name << "\n";
      verifyFunction(*F, &errs());
    }
    F->eraseFromParent();
    return nullptr;
  }
  return F;
}

static void
emitDriver(Function &F, const RT &R, StructType *ArgsTy,
           unsigned NumArgs, bool HasReturn, unsigned RetIdx,
           unsigned QStart, unsigned NumQueues,
           ArrayRef<Function *> Stages) {
  eraseFunctionBody(F);
  stripPurityAttrs(F);
  BasicBlock *Entry = BasicBlock::Create(R.Ctx, "dswp.entry", &F);
  IRBuilder<> B(Entry);

  AllocaInst *Args = B.CreateAlloca(ArgsTy, nullptr, "args");

  for (unsigned i = 0; i < NumArgs; ++i)
    B.CreateStore(F.getArg(i), B.CreateStructGEP(ArgsTy, Args, i));
  if (HasReturn)
    B.CreateStore(Constant::getNullValue(F.getReturnType()),
                   B.CreateStructGEP(ArgsTy, Args, RetIdx));

  SmallVector<Value *, 4> Queues;
  for (unsigned i = 0; i < NumQueues; ++i) {
    Value *Q = B.CreateCall(R.QueueCreate,
                             {ConstantInt::get(R.I64Ty, 1024)});
    B.CreateStore(Q, B.CreateStructGEP(ArgsTy, Args, QStart + i));
    Queues.push_back(Q);
  }

  Value *Null = ConstantPointerNull::get(R.PtrTy);
  SmallVector<AllocaInst *, 4> TSlots;
  for (Function *S : Stages) {
    AllocaInst *T = B.CreateAlloca(R.PtrTy);
    B.CreateCall(R.PthreadCreate, {T, Null, S, Args});
    TSlots.push_back(T);
  }
  for (auto *T : TSlots)
    B.CreateCall(R.PthreadJoin, {B.CreateLoad(R.PtrTy, T), Null});
  for (auto *Q : Queues)
    B.CreateCall(R.QueueDestroy, {Q});

  if (HasReturn) {
    Value *RetSlot = B.CreateStructGEP(ArgsTy, Args, RetIdx);
    Value *V = B.CreateLoad(F.getReturnType(), RetSlot);
    B.CreateRet(V);
  } else {
    B.CreateRetVoid();
  }
}

static bool transformGenericCloned(Function &F, FunctionAnalysisManager &FAM,
                                     const RT &R) {
  bool VERB = getenv("DSWP_VERBOSE");
  auto bail = [&](const char *why) {
    if (VERB) errs() << "DSWP(cloned) " << F.getName() << ": bail — " << why << "\n";
    return false;
  };
  if (F.isDeclaration()) return false;
  auto &LI = FAM.getResult<LoopAnalysis>(F);
  if (LI.empty()) return bail("no loops");
  auto &AA   = FAM.getResult<AAManager>(F);
  auto &MSSA = FAM.getResult<MemorySSAAnalysis>(F).getMSSA();
  auto &DI   = FAM.getResult<DependenceAnalysis>(F);
  auto &PDT  = FAM.getResult<PostDominatorTreeAnalysis>(F);

  Loop *L = nullptr;
  SmallVector<Loop *, 8> Worklist(LI.begin(), LI.end());
  while (!Worklist.empty()) {
    Loop *X = Worklist.pop_back_val();
    for (Loop *Sub : X->getSubLoops()) Worklist.push_back(Sub);
    if (!X->getSubLoops().empty()) continue;
    if (!X->isLoopSimplifyForm()) continue;
    if (!X->getExitBlock()) continue;
    L = X; break;
  }
  if (!L) return bail("no candidate loop");
  BasicBlock *Preheader = L->getLoopPreheader();
  BasicBlock *ExitBlock = L->getExitBlock();
  if (!Preheader || !ExitBlock) return bail("no preheader/exit");

  // Stage count from env (default 2). Capped at 8 for sanity.
  unsigned NumStages = 2;
  if (const char *S = getenv("DSWP_NUM_STAGES")) {
    int v = atoi(S);
    if (v >= 2 && v <= 8) NumStages = (unsigned)v;
  }

  dswp::PDGBuilder Builder(L, LI, AA, MSSA, DI, PDT);
  auto G   = Builder.build();
  auto SCC = dswp::computeSCCs(*G, /*IncludeControl=*/false);
  auto DAG = dswp::buildSCCDAG(*G, SCC);
  auto P   = dswp::partitionNStage(DAG, NumStages);
  if (P.SingleStage) return bail("partition single-stage");

  auto Repl = findReplicatedSCCs(L, SCC, DAG);

  // After replication, every requested stage must have at least one
  // non-replicated SCC; otherwise we can't produce N stages.
  std::vector<bool> StageOccupied(NumStages, false);
  for (unsigned s = 0; s < SCC.SCCs.size(); ++s) {
    if (Repl.count(s)) continue;
    StageOccupied[P.Stage[s]] = true;
  }
  unsigned OccupiedCount = 0;
  for (bool b : StageOccupied) if (b) OccupiedCount++;
  if (OccupiedCount < 2) return bail("after replication, <2 stages occupied");
  if (OccupiedCount < NumStages) return bail("after replication, some stages empty");

  auto Edges = findCrossEdges(L, SCC, P, Repl);
  if (Edges.empty()) return bail("no cross-stage edges");

  for (auto &E : Edges) {
    Type *T = E.Producer->getType();
    if (!T->isIntegerTy(64) && !T->isIntegerTy(32) &&
        !T->isDoubleTy()    && !T->isPointerTy()) {
      if (VERB) errs() << "DSWP(cloned) " << F.getName()
                       << ": bail — unsupported cross-edge type\n";
      return false;
    }
    if (isa<PHINode>(E.Producer)) return bail("PHI cross-edge");
  }

  bool HasReturn = !F.getReturnType()->isVoidTy();
  unsigned NumArgs = F.arg_size();
  SmallVector<Type *, 8> Fields;
  for (Argument &A : F.args()) Fields.push_back(A.getType());
  unsigned RetIdx = 0;
  if (HasReturn) {
    RetIdx = Fields.size();
    Fields.push_back(F.getReturnType());
  }
  unsigned QStart = Fields.size();
  for (size_t i = 0; i < Edges.size(); ++i) Fields.push_back(R.PtrTy);
  StructType *ArgsTy = StructType::create(R.Ctx, Fields, "struct.dswp_cl_args");

  // Generate one stage function per stage, clean up on any failure.
  SmallVector<Function *, 8> Stages;
  for (unsigned s = 0; s < NumStages; ++s) {
    Function *Fn = generateStage(F, R, ArgsTy, NumArgs, HasReturn,
                                  RetIdx, QStart, L, Preheader, ExitBlock,
                                  SCC, P, Repl, Edges, s,
                                  ("dswp.cl.s" + Twine(s) + "." + F.getName()).str());
    if (!Fn) {
      for (Function *Prev : Stages) Prev->eraseFromParent();
      return false;
    }
    Stages.push_back(Fn);
  }

  emitDriver(F, R, ArgsTy, NumArgs, HasReturn, RetIdx, QStart,
             Edges.size(), Stages);
  errs() << "DSWPTransform: rewrote '" << F.getName()
         << "' (cloned " << NumStages << "-stage)\n";
  return true;
}

} // namespace cloned

// ─── Pass entry point ───────────────────────────────────────────────

struct DSWPTransform : PassInfoMixin<DSWPTransform> {
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &FAM) {
    if (F.isDeclaration()) return PreservedAnalyses::all();
    // Skip functions we generated (otherwise the pass tries to transform
    // its own outputs when iterating the module).
    if (F.getName().startswith("dswp."))
      return PreservedAnalyses::all();

    RT R = makeRT(F.getParent());

    // CFG-cloning generic transform; stage count from DSWP_NUM_STAGES
    // (default 2). Returns false if the loop shape doesn't fit (e.g.
    // cross-edge type unsupported, partition collapsed) — in that case
    // the function is left unchanged and runs sequentially.
    bool Changed = cloned::transformGenericCloned(F, FAM, R);
    return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
  }
  static bool isRequired() { return true; }
};

} // namespace

extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
  return {
    LLVM_PLUGIN_API_VERSION, "DSWPTransform", LLVM_VERSION_STRING,
    [](PassBuilder &PB) {
      PB.registerPipelineParsingCallback(
        [](StringRef Name, FunctionPassManager &FPM,
           ArrayRef<PassBuilder::PipelineElement>) {
          if (Name == "dswp-transform") {
            FPM.addPass(DSWPTransform());
            return true;
          }
          return false;
        });
    }};
}
