//===- SimplifyCFG.cpp - Code to perform CFG simplification ---------------===//
//
// SimplifyCFG - This function is used to do simplification of a CFG.  For
// example, it adjusts branches to branches to eliminate the extra hop, it
// eliminates unreachable basic blocks, and does other "peephole" optimization
// of the CFG.  It returns true if a modification was made, and returns an 
// iterator that designates the first element remaining after the block that
// was deleted.
//
// WARNING:  The entry node of a function may not be simplified.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Constant.h"
#include "llvm/iPHINode.h"
#include "llvm/Support/CFG.h"
#include <algorithm>
#include <functional>

// PropogatePredecessors - This gets "Succ" ready to have the predecessors from
// "BB".  This is a little tricky because "Succ" has PHI nodes, which need to
// have extra slots added to them to hold the merge edges from BB's
// predecessors.  This function returns true (failure) if the Succ BB already
// has a predecessor that is a predecessor of BB.
//
// Assumption: Succ is the single successor for BB.
//
static bool PropogatePredecessorsForPHIs(BasicBlock *BB, BasicBlock *Succ) {
  assert(*succ_begin(BB) == Succ && "Succ is not successor of BB!");
  assert(isa<PHINode>(Succ->front()) && "Only works on PHId BBs!");

  // If there is more than one predecessor, and there are PHI nodes in
  // the successor, then we need to add incoming edges for the PHI nodes
  //
  const std::vector<BasicBlock*> BBPreds(pred_begin(BB), pred_end(BB));

  // Check to see if one of the predecessors of BB is already a predecessor of
  // Succ.  If so, we cannot do the transformation!
  //
  for (pred_iterator PI = pred_begin(Succ), PE = pred_end(Succ);
       PI != PE; ++PI) {
    if (find(BBPreds.begin(), BBPreds.end(), *PI) != BBPreds.end())
      return true;
  }

  // Loop over all of the PHI nodes in the successor BB
  for (BasicBlock::iterator I = Succ->begin();
       PHINode *PN = dyn_cast<PHINode>(&*I); ++I) {
    Value *OldVal = PN->removeIncomingValue(BB);
    assert(OldVal && "No entry in PHI for Pred BB!");

    for (std::vector<BasicBlock*>::const_iterator PredI = BBPreds.begin(), 
	   End = BBPreds.end(); PredI != End; ++PredI) {
      // Add an incoming value for each of the new incoming values...
      PN->addIncoming(OldVal, *PredI);
    }
  }
  return false;
}


// SimplifyCFG - This function is used to do simplification of a CFG.  For
// example, it adjusts branches to branches to eliminate the extra hop, it
// eliminates unreachable basic blocks, and does other "peephole" optimization
// of the CFG.  It returns true if a modification was made, and returns an 
// iterator that designates the first element remaining after the block that
// was deleted.
//
// WARNING:  The entry node of a function may not be simplified.
//
bool SimplifyCFG(BasicBlock *BB) {
  Function *M = BB->getParent();

  assert(BB && BB->getParent() && "Block not embedded in function!");
  assert(BB->getTerminator() && "Degenerate basic block encountered!");
  assert(&BB->getParent()->front() != BB && "Can't Simplify entry block!");


  // Remove basic blocks that have no predecessors... which are unreachable.
  if (pred_begin(BB) == pred_end(BB) &&
      !BB->hasConstantReferences()) {
    //cerr << "Removing BB: \n" << BB;

    // Loop through all of our successors and make sure they know that one
    // of their predecessors is going away.
    for_each(succ_begin(BB), succ_end(BB),
	     std::bind2nd(std::mem_fun(&BasicBlock::removePredecessor), BB));

    while (!BB->empty()) {
      Instruction &I = BB->back();
      // If this instruction is used, replace uses with an arbitrary
      // constant value.  Because control flow can't get here, we don't care
      // what we replace the value with.  Note that since this block is 
      // unreachable, and all values contained within it must dominate their
      // uses, that all uses will eventually be removed.
      if (!I.use_empty()) 
        // Make all users of this instruction reference the constant instead
        I.replaceAllUsesWith(Constant::getNullValue(I.getType()));
      
      // Remove the instruction from the basic block
      BB->getInstList().pop_back();
    }
    M->getBasicBlockList().erase(BB);
    return true;
  }

  // Check to see if this block has no instructions and only a single 
  // successor.  If so, replace block references with successor.
  succ_iterator SI(succ_begin(BB));
  if (SI != succ_end(BB) && ++SI == succ_end(BB)) {  // One succ?
    if (BB->front().isTerminator()) {   // Terminator is the only instruction!
      BasicBlock *Succ = *succ_begin(BB); // There is exactly one successor
     
      if (Succ != BB) {   // Arg, don't hurt infinite loops!
        // If our successor has PHI nodes, then we need to update them to
        // include entries for BB's predecessors, not for BB itself.
        // Be careful though, if this transformation fails (returns true) then
        // we cannot do this transformation!
        //
	if (!isa<PHINode>(Succ->front()) ||
            !PropogatePredecessorsForPHIs(BB, Succ)) {

          //cerr << "Killing Trivial BB: \n" << BB;

          BB->replaceAllUsesWith(Succ);
          std::string OldName = BB->getName();

          // Delete the old basic block...
          M->getBasicBlockList().erase(BB);
	
          if (!OldName.empty() && !Succ->hasName())  // Transfer name if we can
            Succ->setName(OldName);
          
          //cerr << "Function after removal: \n" << M;
          return true;
	}
      }
    }
  }

  // Merge basic blocks into their predecessor if there is only one distinct
  // pred, and if there is only one distinct successor of the predecessor, and
  // if there are no PHI nodes.
  //
  if (!isa<PHINode>(BB->front()) && !BB->hasConstantReferences()) {
    pred_iterator PI(pred_begin(BB)), PE(pred_end(BB));
    BasicBlock *OnlyPred = *PI++;
    for (; PI != PE; ++PI)  // Search all predecessors, see if they are all same
      if (*PI != OnlyPred) {
        OnlyPred = 0;       // There are multiple different predecessors...
        break;
      }
  
    BasicBlock *OnlySucc = 0;
    if (OnlyPred && OnlyPred != BB) {   // Don't break self loops
      // Check to see if there is only one distinct successor...
      succ_iterator SI(succ_begin(OnlyPred)), SE(succ_end(OnlyPred));
      OnlySucc = BB;
      for (; SI != SE; ++SI)
        if (*SI != OnlySucc) {
          OnlySucc = 0;     // There are multiple distinct successors!
          break;
        }
    }

    if (OnlySucc) {
      //cerr << "Merging: " << BB << "into: " << OnlyPred;
      TerminatorInst *Term = OnlyPred->getTerminator();

      // Delete the unconditional branch from the predecessor...
      OnlyPred->getInstList().pop_back();
      
      // Move all definitions in the succecessor to the predecessor...
      OnlyPred->getInstList().splice(OnlyPred->end(), BB->getInstList());
                                     
      // Make all PHI nodes that refered to BB now refer to Pred as their
      // source...
      BB->replaceAllUsesWith(OnlyPred);

      std::string OldName = BB->getName();

      // Erase basic block from the function... 
      M->getBasicBlockList().erase(BB);

      // Inherit predecessors name if it exists...
      if (!OldName.empty() && !OnlyPred->hasName())
        OnlyPred->setName(OldName);
      
      return true;
    }
  }
  
  return false;
}
