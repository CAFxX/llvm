//===-- Local.h - Functions to perform local transformations -----*- C++ -*--=//
//
// This family of functions perform various local transformations to the
// program.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UTILS_LOCAL_H
#define LLVM_TRANSFORMS_UTILS_LOCAL_H

#include "llvm/Function.h"
#include "llvm/BasicBlock.h"

//===----------------------------------------------------------------------===//
//  Local constant propogation...
//

// doConstantPropogation - Constant prop a specific instruction.  Returns true
// and potentially moves the iterator if constant propogation was performed.
//
bool doConstantPropogation(BasicBlock *BB, BasicBlock::iterator &I);

// ConstantFoldTerminator - If a terminator instruction is predicated on a
// constant value, convert it into an unconditional branch to the constant
// destination.  This is a nontrivial operation because the successors of this
// basic block must have their PHI nodes updated.
//
bool ConstantFoldTerminator(BasicBlock *BB, BasicBlock::iterator &I,
                            TerminatorInst *T);


//===----------------------------------------------------------------------===//
//  Local dead code elimination...
//

// isInstructionTriviallyDead - Return true if the result produced by the
// instruction is not used, and the instruction has no side effects.
//
bool isInstructionTriviallyDead(Instruction *I);


// dceInstruction - Inspect the instruction at *BBI and figure out if it
// isTriviallyDead.  If so, remove the instruction and update the iterator to
// point to the instruction that immediately succeeded the original instruction.
//
bool dceInstruction(BasicBlock::InstListType &BBIL, BasicBlock::iterator &BBI);


//===----------------------------------------------------------------------===//
//  Control Flow Graph Restructuring...
//

// SimplifyCFG - This function is used to do simplification of a CFG.  For
// example, it adjusts branches to branches to eliminate the extra hop, it
// eliminates unreachable basic blocks, and does other "peephole" optimization
// of the CFG.  It returns true if a modification was made, and returns an 
// iterator that designates the first element remaining after the block that
// was deleted.
//
// WARNING:  The entry node of a method may not be simplified.
//
bool SimplifyCFG(Function::iterator &BBIt);

#endif
