//===- DeadStoreElimination.cpp - Dead Store Elimination ------------------===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file implements a trivial dead store elimination that only considers
// basic-block local redundant stores.
//
// FIXME: This should eventually be extended to be a post-dominator tree
// traversal.  Doing so would be pretty trivial.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AliasSetTracker.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Transforms/Utils/Local.h"
#include "Support/SetVector.h"
#include "Support/Statistic.h"
using namespace llvm;

namespace {
  Statistic<> NumStores("dse", "Number of stores deleted");
  Statistic<> NumOther ("dse", "Number of other instrs removed");

  struct DSE : public FunctionPass {

    virtual bool runOnFunction(Function &F) {
      bool Changed = false;
      for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I)
        Changed |= runOnBasicBlock(*I);
      return Changed;
    }
    
    bool runOnBasicBlock(BasicBlock &BB);
    
    void DeleteDeadInstructionChains(Instruction *I,
                                     SetVector<Instruction*> &DeadInsts);

    // getAnalysisUsage - We require post dominance frontiers (aka Control
    // Dependence Graph)
    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.setPreservesCFG();
      AU.addRequired<TargetData>();
      AU.addRequired<AliasAnalysis>();
      AU.addPreserved<AliasAnalysis>();
    }
  };
  RegisterOpt<DSE> X("dse", "Dead Store Elimination");
}

Pass *llvm::createDeadStoreEliminationPass() { return new DSE(); }

bool DSE::runOnBasicBlock(BasicBlock &BB) {
  TargetData &TD = getAnalysis<TargetData>();
  AliasAnalysis &AA = getAnalysis<AliasAnalysis>();
  AliasSetTracker KillLocs(AA);

  // If this block ends in a return, unwind, and eventually tailcall/barrier,
  // then all allocas are dead at its end.
  if (BB.getTerminator()->getNumSuccessors() == 0) {

  }

  // PotentiallyDeadInsts - Deleting dead stores from the program can make other
  // instructions die if they were only used as operands to stores.  Keep track
  // of the operands to stores so that we can try deleting them at the end of
  // the traversal.
  SetVector<Instruction*> PotentiallyDeadInsts;

  bool MadeChange = false;
  for (BasicBlock::iterator BBI = BB.end(); BBI != BB.begin(); ) {
    Instruction *I = --BBI;   // Keep moving iterator backwards
    
    // If this is a free instruction, it makes the free'd location dead!
    if (FreeInst *FI = dyn_cast<FreeInst>(I)) {
      // Free instructions make any stores to the free'd location dead.
      KillLocs.add(FI);
      continue;
    }

    if (!isa<StoreInst>(I) || cast<StoreInst>(I)->isVolatile()) {
      // If this is a non-store instruction, it makes everything referenced no
      // longer killed.  Remove anything aliased from the alias set tracker.
      KillLocs.remove(I);
      continue;
    }

    // If this is a non-volatile store instruction, and if it is already in
    // the stored location is already in the tracker, then this is a dead
    // store.  We can just delete it here, but while we're at it, we also
    // delete any trivially dead expression chains.
    unsigned ValSize = TD.getTypeSize(I->getOperand(0)->getType());
    Value *Ptr = I->getOperand(1);

    if (AliasSet *AS = KillLocs.getAliasSetForPointerIfExists(Ptr, ValSize))
      for (AliasSet::iterator ASI = AS->begin(), E = AS->end(); ASI != E; ++ASI)
        if (AA.alias(ASI.getPointer(), ASI.getSize(), Ptr, ValSize)
               == AliasAnalysis::MustAlias) {
          // If we found a must alias in the killed set, then this store really
          // is dead.  Remember that the various operands of the store now have
          // fewer users.  At the end we will see if we can delete any values
          // that are dead as part of the store becoming dead.
          if (Instruction *Op = dyn_cast<Instruction>(I->getOperand(0)))
            PotentiallyDeadInsts.insert(Op);
          if (Instruction *Op = dyn_cast<Instruction>(Ptr))
            PotentiallyDeadInsts.insert(Op);

          // Delete it now.
          ++BBI;                        // Don't invalidate iterator.
          BB.getInstList().erase(I);    // Nuke the store!
          ++NumStores;
          MadeChange = true;
          goto BigContinue;
        }

    // Otherwise, this is a non-dead store just add it to the set of dead
    // locations.
    KillLocs.add(cast<StoreInst>(I));
  BigContinue:;
  }

  while (!PotentiallyDeadInsts.empty()) {
    Instruction *I = PotentiallyDeadInsts.back();
    PotentiallyDeadInsts.pop_back();
    DeleteDeadInstructionChains(I, PotentiallyDeadInsts);
  }
  return MadeChange;
}

void DSE::DeleteDeadInstructionChains(Instruction *I,
                                      SetVector<Instruction*> &DeadInsts) {
  // Instruction must be dead.
  if (!I->use_empty() || !isInstructionTriviallyDead(I)) return;

  // Let the alias analysis know that we have nuked a value.
  getAnalysis<AliasAnalysis>().deleteValue(I);

  // See if this made any operands dead.  We do it this way in case the
  // instruction uses the same operand twice.  We don't want to delete a
  // value then reference it.
  while (unsigned NumOps = I->getNumOperands()) {
    Instruction *Op = dyn_cast<Instruction>(I->getOperand(NumOps-1));
    I->op_erase(I->op_end()-1);         // Drop from the operand list.
    
    if (Op) DeadInsts.insert(Op);       // Attempt to nuke it later.
  }
  
  I->getParent()->getInstList().erase(I);
  ++NumOther;
}
