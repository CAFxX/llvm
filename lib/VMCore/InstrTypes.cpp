//===-- InstrTypes.cpp - Implement Instruction subclasses -------*- C++ -*-===//
//
// This file implements 
//
//===----------------------------------------------------------------------===//

#include "llvm/iOther.h"
#include "llvm/iPHINode.h"
#include "llvm/Function.h"
#include "llvm/SymbolTable.h"
#include "llvm/Constant.h"
#include "llvm/Type.h"
#include <algorithm>  // find

//===----------------------------------------------------------------------===//
//                            TerminatorInst Class
//===----------------------------------------------------------------------===//

TerminatorInst::TerminatorInst(Instruction::TermOps iType, Instruction *IB) 
  : Instruction(Type::VoidTy, iType, "", IB) {
}

//===----------------------------------------------------------------------===//
//                               PHINode Class
//===----------------------------------------------------------------------===//

PHINode::PHINode(const PHINode &PN)
  : Instruction(PN.getType(), Instruction::PHINode) {
  Operands.reserve(PN.Operands.size());
  for (unsigned i = 0; i < PN.Operands.size(); i+=2) {
    Operands.push_back(Use(PN.Operands[i], this));
    Operands.push_back(Use(PN.Operands[i+1], this));
  }
}

void PHINode::addIncoming(Value *D, BasicBlock *BB) {
  assert(getType() == D->getType() &&
         "All operands to PHI node must be the same type as the PHI node!");
  Operands.push_back(Use(D, this));
  Operands.push_back(Use(BB, this));
}

// removeIncomingValue - Remove an incoming value.  This is useful if a
// predecessor basic block is deleted.
Value *PHINode::removeIncomingValue(const BasicBlock *BB,
                                    bool DeletePHIIfEmpty) {
  op_iterator Idx = find(Operands.begin(), Operands.end(), (const Value*)BB);
  assert(Idx != Operands.end() && "BB not in PHI node!");
  --Idx;  // Back up to value prior to Basic block
  Value *Removed = *Idx;
  Operands.erase(Idx, Idx+2);  // Erase Value and BasicBlock

  // If the PHI node is dead, because it has zero entries, nuke it now.
  if (getNumOperands() == 0 && DeletePHIIfEmpty) {
    // If anyone is using this PHI, make them use a dummy value instead...
    replaceAllUsesWith(Constant::getNullValue(getType()));
    getParent()->getInstList().erase(this);
  }
  return Removed;
}
