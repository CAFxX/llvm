//===-- iCall.cpp - Implement the call & invoke instructions -----*- C++ -*--=//
//
// This file implements the call and invoke instructions.
//
//===----------------------------------------------------------------------===//

#include "llvm/iOther.h"
#include "llvm/iTerminators.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Method.h"

//===----------------------------------------------------------------------===//
//                        CallInst Implementation
//===----------------------------------------------------------------------===//

CallInst::CallInst(Value *Meth, const std::vector<Value*> &params, 
                   const std::string &Name) 
  : Instruction(cast<FunctionType>(cast<PointerType>(Meth->getType())
				 ->getElementType())->getReturnType(),
		Instruction::Call, Name) {
  Operands.reserve(1+params.size());
  Operands.push_back(Use(Meth, this));

  const FunctionType *MTy = 
    cast<FunctionType>(cast<PointerType>(Meth->getType())->getElementType());

  const FunctionType::ParamTypes &PL = MTy->getParamTypes();
  assert((params.size() == PL.size()) || 
	 (MTy->isVarArg() && params.size() >= PL.size()) &&
	 "Calling a function with bad signature");
  for (unsigned i = 0; i < params.size(); i++)
    Operands.push_back(Use(params[i], this));
}

CallInst::CallInst(const CallInst &CI) 
  : Instruction(CI.getType(), Instruction::Call) {
  Operands.reserve(CI.Operands.size());
  for (unsigned i = 0; i < CI.Operands.size(); ++i)
    Operands.push_back(Use(CI.Operands[i], this));
}

//===----------------------------------------------------------------------===//
//                        InvokeInst Implementation
//===----------------------------------------------------------------------===//

InvokeInst::InvokeInst(Value *Meth, BasicBlock *IfNormal, \
		       BasicBlock *IfException,
                       const std::vector<Value*> &params,
		       const std::string &Name)
  : TerminatorInst(cast<FunctionType>(cast<PointerType>(Meth->getType())
				    ->getElementType())->getReturnType(),
		   Instruction::Invoke, Name) {
  Operands.reserve(3+params.size());
  Operands.push_back(Use(Meth, this));
  Operands.push_back(Use(IfNormal, this));
  Operands.push_back(Use(IfException, this));
  const FunctionType *MTy = 
    cast<FunctionType>(cast<PointerType>(Meth->getType())->getElementType());
  
  const FunctionType::ParamTypes &PL = MTy->getParamTypes();
  assert((params.size() == PL.size()) || 
	 (MTy->isVarArg() && params.size() > PL.size()) &&
	 "Calling a function with bad signature");
  
  for (unsigned i = 0; i < params.size(); i++)
    Operands.push_back(Use(params[i], this));
}

InvokeInst::InvokeInst(const InvokeInst &CI) 
  : TerminatorInst(CI.getType(), Instruction::Invoke) {
  Operands.reserve(CI.Operands.size());
  for (unsigned i = 0; i < CI.Operands.size(); ++i)
    Operands.push_back(Use(CI.Operands[i], this));
}

