//===- SimplifyCFG.cpp - CFG Simplification Pass --------------------------===//
//
// This file implements dead code elimination and basic block merging.
//
// Specifically, this:
//   * removes basic blocks with no predecessors
//   * merges a basic block into its predecessor if there is only one and the
//     predecessor only has one successor.
//   * Eliminates PHI nodes for basic blocks with a single predecessor
//   * Eliminates a basic block that only contains an unconditional branch
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Module.h"
#include "llvm/GlobalVariable.h"
#include "llvm/Support/CFG.h"
#include "llvm/Pass.h"
#include "Support/StatisticReporter.h"
#include <set>

static Statistic<> NumSimpl("cfgsimplify\t- Number of blocks simplified");

namespace {
  struct CFGSimplifyPass : public FunctionPass {
    virtual bool runOnFunction(Function &F);
  };
  RegisterPass<CFGSimplifyPass> X("simplifycfg", "Simplify the CFG");
}

Pass *createCFGSimplificationPass() {
  return new CFGSimplifyPass();
}

static bool MarkAliveBlocks(BasicBlock *BB, std::set<BasicBlock*> &Reachable) {
  if (Reachable.count(BB)) return false;
  Reachable.insert(BB);

  bool Changed = ConstantFoldTerminator(BB);
  for (succ_iterator SI = succ_begin(BB), SE = succ_end(BB); SI != SE; ++SI)
    MarkAliveBlocks(*SI, Reachable);

  return Changed;
}


// It is possible that we may require multiple passes over the code to fully
// simplify the CFG.
//
bool CFGSimplifyPass::runOnFunction(Function &F) {
  std::set<BasicBlock*> Reachable;
  bool Changed = MarkAliveBlocks(F.begin(), Reachable);

  // If there are unreachable blocks in the CFG...
  if (Reachable.size() != F.size()) {
    assert(Reachable.size() < F.size());
    NumSimpl += F.size()-Reachable.size();

    // Loop over all of the basic blocks that are not reachable, dropping all of
    // their internal references...
    for (Function::iterator BB = ++F.begin(), E = F.end(); BB != E; ++BB)
      if (!Reachable.count(BB)) {
        for (succ_iterator SI = succ_begin(BB), SE = succ_end(BB); SI!=SE; ++SI)
          if (Reachable.count(*SI))
            (*SI)->removePredecessor(BB);
        BB->dropAllReferences();
      }
    
    for (Function::iterator I = ++F.begin(); I != F.end();)
      if (!Reachable.count(I))
        I = F.getBasicBlockList().erase(I);
      else
        ++I;

    Changed = true;
  }

  bool LocalChange = true;
  while (LocalChange) {
    LocalChange = false;

    // Loop over all of the basic blocks (except the first one) and remove them
    // if they are unneeded...
    //
    for (Function::iterator BBIt = ++F.begin(); BBIt != F.end(); ) {
      if (SimplifyCFG(BBIt++)) {
        LocalChange = true;
        ++NumSimpl;
      }
    }
    Changed |= LocalChange;
  }

  return Changed;
}
