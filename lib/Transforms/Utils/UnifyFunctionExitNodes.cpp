//===- SimplifyCFG.cpp - CFG Simplification Routines -------------*- C++ -*--=//
//
// This file provides several routines that are useful for simplifying CFGs in
// various ways...
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/SimplifyCFG.h"
#include "llvm/BasicBlock.h"
#include "llvm/Method.h"
#include "llvm/iTerminators.h"
#include "llvm/iOther.h"
#include "llvm/Type.h"

// UnifyAllExitNodes - Unify all exit nodes of the CFG by creating a new
// BasicBlock, and converting all returns to unconditional branches to this
// new basic block.  The singular exit node is returned.
//
// If there are no return stmts in the Method, a null pointer is returned.
//
BasicBlock *cfg::UnifyAllExitNodes(Method *M) {
  vector<BasicBlock*> ReturningBlocks;

  // Loop over all of the blocks in a method, tracking all of the blocks that
  // return.
  //
  for(Method::iterator I = M->begin(), E = M->end(); I != E; ++I)
    if ((*I)->getTerminator()->getInstType() == Instruction::Ret)
      ReturningBlocks.push_back(*I);

  if (ReturningBlocks.size() == 0) 
    return 0;                          // No blocks return
  else if (ReturningBlocks.size() == 1)
    return ReturningBlocks.front();    // Already has a single return block

  // Otherwise, we need to insert a new basic block into the method, add a PHI
  // node (if the function returns a value), and convert all of the return 
  // instructions into unconditional branches.
  //
  BasicBlock *NewRetBlock = new BasicBlock("", M);

  if (M->getReturnType() != Type::VoidTy) {
    // If the method doesn't return void... add a PHI node to the block...
    PHINode *PN = new PHINode(M->getReturnType());
    NewRetBlock->getInstList().push_back(PN);

    // Add an incoming element to the PHI node for every return instruction that
    // is merging into this new block...
    for (vector<BasicBlock*>::iterator I = ReturningBlocks.begin(), 
                                       E = ReturningBlocks.end(); I != E; ++I)
      PN->addIncoming((*I)->getTerminator()->getOperand(0), *I);

    // Add a return instruction to return the result of the PHI node...
    NewRetBlock->getInstList().push_back(new ReturnInst(PN));
  } else {
    // If it returns void, just add a return void instruction to the block
    NewRetBlock->getInstList().push_back(new ReturnInst());
  }

  // Loop over all of the blocks, replacing the return instruction with an
  // unconditional branch.
  //
  for (vector<BasicBlock*>::iterator I = ReturningBlocks.begin(), 
                                     E = ReturningBlocks.end(); I != E; ++I) {
    delete (*I)->getInstList().pop_back();  // Remove the return insn
    (*I)->getInstList().push_back(new BranchInst(NewRetBlock));
  }
  return NewRetBlock;
}
