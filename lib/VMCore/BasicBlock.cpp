//===-- BasicBlock.cpp - Implement BasicBlock related functions --*- C++ -*--=//
//
// This file implements the Method class for the VMCore library.
//
//===----------------------------------------------------------------------===//

#include "llvm/ValueHolderImpl.h"
#include "llvm/BasicBlock.h"
#include "llvm/iTerminators.h"
#include "llvm/Method.h"
#include "llvm/SymbolTable.h"
#include "llvm/Type.h"
#include "llvm/iPHINode.h"
#include "llvm/CodeGen/MachineInstr.h"

// Instantiate Templates - This ugliness is the price we have to pay
// for having a ValueHolderImpl.h file seperate from ValueHolder.h!  :(
//
template class ValueHolder<Instruction, BasicBlock, Method>;

BasicBlock::BasicBlock(const string &name, Method *Parent)
  : Value(Type::LabelTy, Value::BasicBlockVal, name), InstList(this, 0),
    machineInstrVec(new MachineCodeForBasicBlock) {
  if (Parent)
    Parent->getBasicBlocks().push_back(this);
}

BasicBlock::~BasicBlock() {
  dropAllReferences();
  InstList.delete_all();
  delete machineInstrVec;
}

// Specialize setName to take care of symbol table majik
void BasicBlock::setName(const string &name, SymbolTable *ST) {
  Method *P;
  assert((ST == 0 || (!getParent() || ST == getParent()->getSymbolTable())) &&
	 "Invalid symtab argument!");
  if ((P = getParent()) && hasName()) P->getSymbolTable()->remove(this);
  Value::setName(name);
  if (P && hasName()) P->getSymbolTable()->insert(this);
}

void BasicBlock::setParent(Method *parent) { 
  if (getParent() && hasName())
    getParent()->getSymbolTable()->remove(this);

  InstList.setParent(parent);

  if (getParent() && hasName())
    getParent()->getSymbolTableSure()->insert(this);
}

TerminatorInst *BasicBlock::getTerminator() {
  if (InstList.empty()) return 0;
  Instruction *T = InstList.back();
  if (isa<TerminatorInst>(T)) return cast<TerminatorInst>(T);
  return 0;
}

const TerminatorInst *const BasicBlock::getTerminator() const {
  if (InstList.empty()) return 0;
  if (const TerminatorInst *TI = dyn_cast<TerminatorInst>(InstList.back()))
    return TI;
  return 0;
}

void BasicBlock::dropAllReferences() {
  for_each(InstList.begin(), InstList.end(), 
	   std::mem_fun(&Instruction::dropAllReferences));
}

// hasConstantPoolReferences() - This predicate is true if there is a 
// reference to this basic block in the constant pool for this method.  For
// example, if a block is reached through a switch table, that table resides
// in the constant pool, and the basic block is reference from it.
//
bool BasicBlock::hasConstantPoolReferences() const {
  for (use_const_iterator I = use_begin(), E = use_end(); I != E; ++I)
    if (::isa<ConstPoolVal>(*I))
      return true;

  return false;
}

// removePredecessor - This method is used to notify a BasicBlock that the
// specified Predecessor of the block is no longer able to reach it.  This is
// actually not used to update the Predecessor list, but is actually used to 
// update the PHI nodes that reside in the block.  Note that this should be
// called while the predecessor still refers to this block.
//
void BasicBlock::removePredecessor(BasicBlock *Pred) {
  assert(find(pred_begin(), pred_end(), Pred) != pred_end() &&
	 "removePredecessor: BB is not a predecessor!");
  if (!isa<PHINode>(front())) return;   // Quick exit.

  pred_iterator PI(pred_begin()), EI(pred_end());
  unsigned max_idx;

  // Loop over the rest of the predecessors until we run out, or until we find
  // out that there are more than 2 predecessors.
  for (max_idx = 0; PI != EI && max_idx < 3; ++PI, ++max_idx) /*empty*/;

  // If there are exactly two predecessors, then we want to nuke the PHI nodes
  // altogether.
  assert(max_idx != 0 && "PHI Node in block with 0 predecessors!?!?!");
  if (max_idx <= 2) {                // <= Two predecessors BEFORE I remove one?
    // Yup, loop through and nuke the PHI nodes
    while (PHINode *PN = dyn_cast<PHINode>(front())) {
      PN->removeIncomingValue(Pred); // Remove the predecessor first...
      
      assert(PN->getNumIncomingValues() == max_idx-1 && 
	     "PHI node shouldn't have this many values!!!");

      // If the PHI _HAD_ two uses, replace PHI node with its now *single* value
      if (max_idx == 2)
	PN->replaceAllUsesWith(PN->getOperand(0));
      delete getInstList().remove(begin());  // Remove the PHI node
    }
  } else {
    // Okay, now we know that we need to remove predecessor #pred_idx from all
    // PHI nodes.  Iterate over each PHI node fixing them up
    iterator II(begin());
    for (; isa<PHINode>(*II); ++II)
      cast<PHINode>(*II)->removeIncomingValue(Pred);
  }
}


// splitBasicBlock - This splits a basic block into two at the specified
// instruction.  Note that all instructions BEFORE the specified iterator stay
// as part of the original basic block, an unconditional branch is added to 
// the new BB, and the rest of the instructions in the BB are moved to the new
// BB, including the old terminator.  This invalidates the iterator.
//
// Note that this only works on well formed basic blocks (must have a 
// terminator), and 'I' must not be the end of instruction list (which would
// cause a degenerate basic block to be formed, having a terminator inside of
// the basic block). 
//
BasicBlock *BasicBlock::splitBasicBlock(iterator I) {
  assert(getTerminator() && "Can't use splitBasicBlock on degenerate BB!");
  assert(I != InstList.end() && 
	 "Trying to get me to create degenerate basic block!");

  BasicBlock *New = new BasicBlock("", getParent());

  // Go from the end of the basic block through to the iterator pointer, moving
  // to the new basic block...
  Instruction *Inst = 0;
  do {
    iterator EndIt = end();
    Inst = InstList.remove(--EndIt);                  // Remove from end
    New->InstList.push_front(Inst);                   // Add to front
  } while (Inst != *I);   // Loop until we move the specified instruction.

  // Add a branch instruction to the newly formed basic block.
  InstList.push_back(new BranchInst(New));
  return New;
}
