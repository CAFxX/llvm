//===-- llvm/iBinary.h - Binary Operator node definitions --------*- C++ -*--=//
//
// This file contains the declarations of all of the Binary Operator classes.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_IBINARY_H
#define LLVM_IBINARY_H

#include "llvm/InstrTypes.h"

//===----------------------------------------------------------------------===//
//                 Classes to represent Binary operators
//===----------------------------------------------------------------------===//
//
// All of these classes are subclasses of the BinaryOperator class...
//

class GenericBinaryInst : public BinaryOperator {
public:
  GenericBinaryInst(BinaryOps Opcode, Value *S1, Value *S2, 
		    const std::string &Name = "")
    : BinaryOperator(Opcode, S1, S2, Name) {
  }
};

class SetCondInst : public BinaryOperator {
  BinaryOps OpType;
public:
  SetCondInst(BinaryOps Opcode, Value *LHS, Value *RHS,
	      const std::string &Name = "");

  /// getInverseCondition - Return the inverse of the current condition opcode.
  /// For example seteq -> setne, setgt -> setle, setlt -> setge, etc...
  ///
  BinaryOps getInverseCondition() const {
    return getInverseCondition(getOpcode());
  }

  /// getInverseCondition - Static version that you can use without an
  /// instruction available.
  ///
  static BinaryOps getInverseCondition(BinaryOps Opcode);

  /// getSwappedCondition - Return the condition opcode that would be the result
  /// of exchanging the two operands of the setcc instruction without changing
  /// the result produced.  Thus, seteq->seteq, setle->setge, setlt->setgt, etc.
  ///
  BinaryOps getSwappedCondition() const {
    return getSwappedCondition(getOpcode());
  }

  /// getSwappedCondition - Static version that you can use without an
  /// instruction available.
  ///
  static BinaryOps getSwappedCondition(BinaryOps Opcode);


  // Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const SetCondInst *) { return true; }
  static inline bool classof(const Instruction *I) {
    return I->getOpcode() == SetEQ || I->getOpcode() == SetNE ||
           I->getOpcode() == SetLE || I->getOpcode() == SetGE ||
           I->getOpcode() == SetLT || I->getOpcode() == SetGT;
  }
  static inline bool classof(const Value *V) {
    return isa<Instruction>(V) && classof(cast<Instruction>(V));
  }
};

#endif
