//===- PromoteMemoryToRegister.cpp - Convert allocas to registers ---------===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file promote memory references to be register references.  It promotes
// alloca instructions which only have loads and stores as uses.  An alloca is
// transformed by using dominator frontiers to place PHI nodes, then traversing
// the function in depth-first order to rewrite loads and stores as appropriate.
// This is just the standard SSA construction algorithm to construct "pruned"
// SSA form.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/PromoteMemToReg.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/iMemory.h"
#include "llvm/iPHINode.h"
#include "llvm/Function.h"
#include "llvm/Constant.h"
#include "llvm/Support/CFG.h"
#include "Support/StringExtras.h"

/// isAllocaPromotable - Return true if this alloca is legal for promotion.
/// This is true if there are only loads and stores to the alloca...
///
bool isAllocaPromotable(const AllocaInst *AI, const TargetData &TD) {
  // FIXME: If the memory unit is of pointer or integer type, we can permit
  // assignments to subsections of the memory unit.

  // Only allow direct loads and stores...
  for (Value::use_const_iterator UI = AI->use_begin(), UE = AI->use_end();
       UI != UE; ++UI)     // Loop over all of the uses of the alloca
    if (!isa<LoadInst>(*UI))
      if (const StoreInst *SI = dyn_cast<StoreInst>(*UI)) {
        if (SI->getOperand(0) == AI)
          return false;   // Don't allow a store of the AI, only INTO the AI.
      } else {
        return false;   // Not a load or store?
      }
  
  return true;
}

namespace {
  struct PromoteMem2Reg {
    // Allocas - The alloca instructions being promoted
    std::vector<AllocaInst*> Allocas;
    DominatorTree &DT;
    DominanceFrontier &DF;
    const TargetData &TD;

    // AllocaLookup - Reverse mapping of Allocas
    std::map<AllocaInst*, unsigned>  AllocaLookup;

    // NewPhiNodes - The PhiNodes we're adding.
    std::map<BasicBlock*, std::vector<PHINode*> > NewPhiNodes;

    // Visited - The set of basic blocks the renamer has already visited.
    std::set<BasicBlock*> Visited;

  public:
    PromoteMem2Reg(const std::vector<AllocaInst*> &A, DominatorTree &dt,
                   DominanceFrontier &df, const TargetData &td)
      : Allocas(A), DT(dt), DF(df), TD(td) {}

    void run();

  private:
    void MarkDominatingPHILive(BasicBlock *BB, unsigned AllocaNum,
                               std::set<PHINode*> &DeadPHINodes);
    void PromoteLocallyUsedAlloca(AllocaInst *AI);

    void RenamePass(BasicBlock *BB, BasicBlock *Pred,
                    std::vector<Value*> &IncVals);
    bool QueuePhiNode(BasicBlock *BB, unsigned AllocaIdx, unsigned &Version,
                      std::set<PHINode*> &InsertedPHINodes);
  };
}  // end of anonymous namespace

void PromoteMem2Reg::run() {
  Function &F = *DF.getRoot()->getParent();

  for (unsigned AllocaNum = 0; AllocaNum != Allocas.size(); ++AllocaNum) {
    AllocaInst *AI = Allocas[AllocaNum];

    assert(isAllocaPromotable(AI, TD) &&
           "Cannot promote non-promotable alloca!");
    assert(AI->getParent()->getParent() == &F &&
           "All allocas should be in the same function, which is same as DF!");

    if (AI->use_empty()) {
      // If there are no uses of the alloca, just delete it now.
      AI->getParent()->getInstList().erase(AI);

      // Remove the alloca from the Allocas list, since it has been processed
      Allocas[AllocaNum] = Allocas.back();
      Allocas.pop_back();
      --AllocaNum;
      continue;
    }

    // Calculate the set of read and write-locations for each alloca.  This is
    // analogous to counting the number of 'uses' and 'definitions' of each
    // variable.
    std::vector<BasicBlock*> DefiningBlocks;
    std::vector<BasicBlock*> UsingBlocks;

    BasicBlock *OnlyBlock = 0;
    bool OnlyUsedInOneBlock = true;

    // As we scan the uses of the alloca instruction, keep track of stores, and
    // decide whether all of the loads and stores to the alloca are within the
    // same basic block.
    for (Value::use_iterator U =AI->use_begin(), E = AI->use_end(); U != E;++U){
      Instruction *User = cast<Instruction>(*U);
      if (StoreInst *SI = dyn_cast<StoreInst>(User)) {
        // Remember the basic blocks which define new values for the alloca
        DefiningBlocks.push_back(SI->getParent());
      } else {
        // Otherwise it must be a load instruction, keep track of variable reads
        UsingBlocks.push_back(cast<LoadInst>(User)->getParent());
      }

      if (OnlyUsedInOneBlock) {
        if (OnlyBlock == 0)
          OnlyBlock = User->getParent();
        else if (OnlyBlock != User->getParent())
          OnlyUsedInOneBlock = false;
      }
    }

    // If the alloca is only read and written in one basic block, just perform a
    // linear sweep over the block to eliminate it.
    if (OnlyUsedInOneBlock) {
      PromoteLocallyUsedAlloca(AI);

      // Remove the alloca from the Allocas list, since it has been processed
      Allocas[AllocaNum] = Allocas.back();
      Allocas.pop_back();
      --AllocaNum;
      continue;
    }

    // Compute the locations where PhiNodes need to be inserted.  Look at the
    // dominance frontier of EACH basic-block we have a write in.
    //
    unsigned CurrentVersion = 0;
    std::set<PHINode*> InsertedPHINodes;
    while (!DefiningBlocks.empty()) {
      BasicBlock *BB = DefiningBlocks.back();
      DefiningBlocks.pop_back();

      // Look up the DF for this write, add it to PhiNodes
      DominanceFrontier::const_iterator it = DF.find(BB);
      if (it != DF.end()) {
        const DominanceFrontier::DomSetType &S = it->second;
        for (DominanceFrontier::DomSetType::iterator P = S.begin(),PE = S.end();
             P != PE; ++P)
          if (QueuePhiNode(*P, AllocaNum, CurrentVersion, InsertedPHINodes))
            DefiningBlocks.push_back(*P);
      }
    }

    // Now that we have inserted PHI nodes along the Iterated Dominance Frontier
    // of the writes to the variable, scan through the reads of the variable,
    // marking PHI nodes which are actually necessary as alive (by removing them
    // from the InsertedPHINodes set).  This is not perfect: there may PHI
    // marked alive because of loads which are dominated by stores, but there
    // will be no unmarked PHI nodes which are actually used.
    //
    for (unsigned i = 0, e = UsingBlocks.size(); i != e; ++i)
      MarkDominatingPHILive(UsingBlocks[i], AllocaNum, InsertedPHINodes);
    UsingBlocks.clear();

    // If there are any PHI nodes which are now known to be dead, remove them!
    for (std::set<PHINode*>::iterator I = InsertedPHINodes.begin(),
           E = InsertedPHINodes.end(); I != E; ++I) {
      PHINode *PN = *I;
      std::vector<PHINode*> &BBPNs = NewPhiNodes[PN->getParent()];
      BBPNs[AllocaNum] = 0;

      // Check to see if we just removed the last inserted PHI node from this
      // basic block.  If so, remove the entry for the basic block.
      bool HasOtherPHIs = false;
      for (unsigned i = 0, e = BBPNs.size(); i != e; ++i)
        if (BBPNs[i]) {
          HasOtherPHIs = true;
          break;
        }
      if (!HasOtherPHIs)
        NewPhiNodes.erase(PN->getParent());

      PN->getParent()->getInstList().erase(PN);      
    }

    // Keep the reverse mapping of the 'Allocas' array. 
    AllocaLookup[Allocas[AllocaNum]] = AllocaNum;
  }
  
  if (Allocas.empty())
    return; // All of the allocas must have been trivial!

  // Set the incoming values for the basic block to be null values for all of
  // the alloca's.  We do this in case there is a load of a value that has not
  // been stored yet.  In this case, it will get this null value.
  //
  std::vector<Value *> Values(Allocas.size());
  for (unsigned i = 0, e = Allocas.size(); i != e; ++i)
    Values[i] = Constant::getNullValue(Allocas[i]->getAllocatedType());

  // Walks all basic blocks in the function performing the SSA rename algorithm
  // and inserting the phi nodes we marked as necessary
  //
  RenamePass(F.begin(), 0, Values);

  // The renamer uses the Visited set to avoid infinite loops.  Clear it now.
  Visited.clear();

  // Remove the allocas themselves from the function...
  for (unsigned i = 0, e = Allocas.size(); i != e; ++i) {
    Instruction *A = Allocas[i];

    // If there are any uses of the alloca instructions left, they must be in
    // sections of dead code that were not processed on the dominance frontier.
    // Just delete the users now.
    //
    if (!A->use_empty())
      A->replaceAllUsesWith(Constant::getNullValue(A->getType()));
    A->getParent()->getInstList().erase(A);
  }

  // At this point, the renamer has added entries to PHI nodes for all reachable
  // code.  Unfortunately, there may be blocks which are not reachable, which
  // the renamer hasn't traversed.  If this is the case, the PHI nodes may not
  // have incoming values for all predecessors.  Loop over all PHI nodes we have
  // created, inserting null constants if they are missing any incoming values.
  //
  for (std::map<BasicBlock*, std::vector<PHINode *> >::iterator I = 
         NewPhiNodes.begin(), E = NewPhiNodes.end(); I != E; ++I) {

    std::vector<BasicBlock*> Preds(pred_begin(I->first), pred_end(I->first));
    std::vector<PHINode*> &PNs = I->second;
    assert(!PNs.empty() && "Empty PHI node list??");

    // Only do work here if there the PHI nodes are missing incoming values.  We
    // know that all PHI nodes that were inserted in a block will have the same
    // number of incoming values, so we can just check any PHI node.
    PHINode *FirstPHI;
    for (unsigned i = 0; (FirstPHI = PNs[i]) == 0; ++i)
      /*empty*/;

    if (Preds.size() != FirstPHI->getNumIncomingValues()) {
      // Ok, now we know that all of the PHI nodes are missing entries for some
      // basic blocks.  Start by sorting the incoming predecessors for efficient
      // access.
      std::sort(Preds.begin(), Preds.end());

      // Now we loop through all BB's which have entries in FirstPHI and remove
      // them from the Preds list.
      for (unsigned i = 0, e = FirstPHI->getNumIncomingValues(); i != e; ++i) {
        // Do a log(n) search of the Preds list for the entry we want.
        std::vector<BasicBlock*>::iterator EntIt =
          std::lower_bound(Preds.begin(), Preds.end(),
                           FirstPHI->getIncomingBlock(i));
        assert(EntIt != Preds.end() && *EntIt == FirstPHI->getIncomingBlock(i)&&
               "PHI node has entry for a block which is not a predecessor!");

        // Remove the entry
        Preds.erase(EntIt);
      }

      // At this point, the blocks left in the preds list must have dummy
      // entries inserted into every PHI nodes for the block.
      for (unsigned i = 0, e = PNs.size(); i != e; ++i)
        if (PHINode *PN = PNs[i]) {
          Value *NullVal = Constant::getNullValue(PN->getType());
          for (unsigned pred = 0, e = Preds.size(); pred != e; ++pred)
            PN->addIncoming(NullVal, Preds[pred]);
        }
    }
  }
}

// MarkDominatingPHILive - Mem2Reg wants to construct "pruned" SSA form, not
// "minimal" SSA form.  To do this, it inserts all of the PHI nodes on the IDF
// as usual (inserting the PHI nodes in the DeadPHINodes set), then processes
// each read of the variable.  For each block that reads the variable, this
// function is called, which removes used PHI nodes from the DeadPHINodes set.
// After all of the reads have been processed, any PHI nodes left in the
// DeadPHINodes set are removed.
//
void PromoteMem2Reg::MarkDominatingPHILive(BasicBlock *BB, unsigned AllocaNum,
                                           std::set<PHINode*> &DeadPHINodes) {
  // Scan the immediate dominators of this block looking for a block which has a
  // PHI node for Alloca num.  If we find it, mark the PHI node as being alive!
  for (DominatorTree::Node *N = DT[BB]; N; N = N->getIDom()) {
    BasicBlock *DomBB = N->getBlock();
    std::map<BasicBlock*, std::vector<PHINode*> >::iterator
      I = NewPhiNodes.find(DomBB);
    if (I != NewPhiNodes.end() && I->second[AllocaNum]) {
      // Ok, we found an inserted PHI node which dominates this value.
      PHINode *DominatingPHI = I->second[AllocaNum];

      // Find out if we previously thought it was dead.
      std::set<PHINode*>::iterator DPNI = DeadPHINodes.find(DominatingPHI);
      if (DPNI != DeadPHINodes.end()) {
        // Ok, until now, we thought this PHI node was dead.  Mark it as being
        // alive/needed.
        DeadPHINodes.erase(DPNI);

        // Now that we have marked the PHI node alive, also mark any PHI nodes
        // which it might use as being alive as well.
        for (pred_iterator PI = pred_begin(DomBB), PE = pred_end(DomBB);
             PI != PE; ++PI)
          MarkDominatingPHILive(*PI, AllocaNum, DeadPHINodes);
      }
    }
  }
}

// PromoteLocallyUsedAlloca - Many allocas are only used within a single basic
// block.  If this is the case, avoid traversing the CFG and inserting a lot of
// potentially useless PHI nodes by just performing a single linear pass over
// the basic block using the Alloca.
//
void PromoteMem2Reg::PromoteLocallyUsedAlloca(AllocaInst *AI) {
  assert(!AI->use_empty() && "There are no uses of the alloca!");

  // Uses of the uninitialized memory location shall get zero...
  Value *CurVal = Constant::getNullValue(AI->getAllocatedType());
  
  BasicBlock *BB = cast<Instruction>(AI->use_back())->getParent();

  for (BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E; ) {
    Instruction *Inst = I++;
    if (LoadInst *LI = dyn_cast<LoadInst>(Inst)) {
      if (LI->getOperand(0) == AI) {
        // Loads just return the "current value"...
        LI->replaceAllUsesWith(CurVal);
        BB->getInstList().erase(LI);
      }
    } else if (StoreInst *SI = dyn_cast<StoreInst>(Inst)) {
      if (SI->getOperand(1) == AI) {
        // Loads just update the "current value"...
        CurVal = SI->getOperand(0);
        BB->getInstList().erase(SI);
      }
    }
  }

  // After traversing the basic block, there should be no more uses of the
  // alloca, remove it now.
  assert(AI->use_empty() && "Uses of alloca from more than one BB??");
  AI->getParent()->getInstList().erase(AI);
}

// QueuePhiNode - queues a phi-node to be added to a basic-block for a specific
// Alloca returns true if there wasn't already a phi-node for that variable
//
bool PromoteMem2Reg::QueuePhiNode(BasicBlock *BB, unsigned AllocaNo,
                                  unsigned &Version,
                                  std::set<PHINode*> &InsertedPHINodes) {
  // Look up the basic-block in question
  std::vector<PHINode*> &BBPNs = NewPhiNodes[BB];
  if (BBPNs.empty()) BBPNs.resize(Allocas.size());

  // If the BB already has a phi node added for the i'th alloca then we're done!
  if (BBPNs[AllocaNo]) return false;

  // Create a PhiNode using the dereferenced type... and add the phi-node to the
  // BasicBlock.
  BBPNs[AllocaNo] = new PHINode(Allocas[AllocaNo]->getAllocatedType(),
                                Allocas[AllocaNo]->getName() + "." +
                                        utostr(Version++), BB->begin());
  InsertedPHINodes.insert(BBPNs[AllocaNo]);
  return true;
}


// RenamePass - Recursively traverse the CFG of the function, renaming loads and
// stores to the allocas which we are promoting.  IncomingVals indicates what
// value each Alloca contains on exit from the predecessor block Pred.
//
void PromoteMem2Reg::RenamePass(BasicBlock *BB, BasicBlock *Pred,
                                std::vector<Value*> &IncomingVals) {

  // If this BB needs a PHI node, update the PHI node for each variable we need
  // PHI nodes for.
  std::map<BasicBlock*, std::vector<PHINode *> >::iterator
    BBPNI = NewPhiNodes.find(BB);
  if (BBPNI != NewPhiNodes.end()) {
    std::vector<PHINode *> &BBPNs = BBPNI->second;
    for (unsigned k = 0; k != BBPNs.size(); ++k)
      if (PHINode *PN = BBPNs[k]) {
        // Add this incoming value to the PHI node.
        PN->addIncoming(IncomingVals[k], Pred);

        // The currently active variable for this block is now the PHI.
        IncomingVals[k] = PN;
      }
  }

  // don't revisit nodes
  if (Visited.count(BB)) return;
  
  // mark as visited
  Visited.insert(BB);

  for (BasicBlock::iterator II = BB->begin(); !isa<TerminatorInst>(II); ) {
    Instruction *I = II++; // get the instruction, increment iterator

    if (LoadInst *LI = dyn_cast<LoadInst>(I)) {
      if (AllocaInst *Src = dyn_cast<AllocaInst>(LI->getPointerOperand())) {
        std::map<AllocaInst*, unsigned>::iterator AI = AllocaLookup.find(Src);
        if (AI != AllocaLookup.end()) {
          Value *V = IncomingVals[AI->second];

          // walk the use list of this load and replace all uses with r
          LI->replaceAllUsesWith(V);
          BB->getInstList().erase(LI);
        }
      }
    } else if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
      // Delete this instruction and mark the name as the current holder of the
      // value
      if (AllocaInst *Dest = dyn_cast<AllocaInst>(SI->getPointerOperand())) {
        std::map<AllocaInst *, unsigned>::iterator ai = AllocaLookup.find(Dest);
        if (ai != AllocaLookup.end()) {
          // what value were we writing?
          IncomingVals[ai->second] = SI->getOperand(0);
          BB->getInstList().erase(SI);
        }
      }
    }
  }

  // Recurse to our successors.
  TerminatorInst *TI = BB->getTerminator();
  for (unsigned i = 0; i != TI->getNumSuccessors(); i++) {
    std::vector<Value*> OutgoingVals(IncomingVals);
    RenamePass(TI->getSuccessor(i), BB, OutgoingVals);
  }
}

/// PromoteMemToReg - Promote the specified list of alloca instructions into
/// scalar registers, inserting PHI nodes as appropriate.  This function makes
/// use of DominanceFrontier information.  This function does not modify the CFG
/// of the function at all.  All allocas must be from the same function.
///
void PromoteMemToReg(const std::vector<AllocaInst*> &Allocas,
                     DominatorTree &DT, DominanceFrontier &DF,
                     const TargetData &TD) {
  // If there is nothing to do, bail out...
  if (Allocas.empty()) return;
  PromoteMem2Reg(Allocas, DT, DF, TD).run();
}
