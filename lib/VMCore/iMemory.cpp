//===-- iMemory.cpp - Implement Memory instructions -----------------------===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file implements the various memory related classes defined in iMemory.h
//
//===----------------------------------------------------------------------===//

#include "llvm/iMemory.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
using namespace llvm;

void AllocationInst::init(const Type *Ty, Value *ArraySize, unsigned iTy)
{
  // ArraySize defaults to 1.
  if (!ArraySize) ArraySize = ConstantUInt::get(Type::UIntTy, 1);

  Operands.reserve(1);
  assert(ArraySize->getType() == Type::UIntTy &&
         "Malloc/Allocation array size != UIntTy!");

  Operands.push_back(Use(ArraySize, this));
}

AllocationInst::AllocationInst(const Type *Ty, Value *ArraySize, unsigned iTy, 
                               const std::string &Name,
                               Instruction *InsertBefore)
  : Instruction(PointerType::get(Ty), iTy, Name, InsertBefore) {
  init(Ty, ArraySize, iTy);
}

AllocationInst::AllocationInst(const Type *Ty, Value *ArraySize, unsigned iTy, 
                               const std::string &Name,
                               BasicBlock *InsertAtEnd)
  : Instruction(PointerType::get(Ty), iTy, Name, InsertAtEnd) {
  init(Ty, ArraySize, iTy);
}

bool AllocationInst::isArrayAllocation() const {
  return getOperand(0) != ConstantUInt::get(Type::UIntTy, 1);
}

const Type *AllocationInst::getAllocatedType() const {
  return getType()->getElementType();
}

AllocaInst::AllocaInst(const AllocaInst &AI)
  : AllocationInst(AI.getType()->getElementType(), (Value*)AI.getOperand(0),
                   Instruction::Alloca) {
}

MallocInst::MallocInst(const MallocInst &MI)
  : AllocationInst(MI.getType()->getElementType(), (Value*)MI.getOperand(0),
                   Instruction::Malloc) {
}

//===----------------------------------------------------------------------===//
//                             FreeInst Implementation
//===----------------------------------------------------------------------===//

void FreeInst::init(Value *Ptr)
{
  assert(Ptr && isa<PointerType>(Ptr->getType()) && "Can't free nonpointer!");
  Operands.reserve(1);
  Operands.push_back(Use(Ptr, this));
}

FreeInst::FreeInst(Value *Ptr, Instruction *InsertBefore)
  : Instruction(Type::VoidTy, Free, "", InsertBefore) {
  init(Ptr);
}

FreeInst::FreeInst(Value *Ptr, BasicBlock *InsertAtEnd)
  : Instruction(Type::VoidTy, Free, "", InsertAtEnd) {
  init(Ptr);
}


//===----------------------------------------------------------------------===//
//                           LoadInst Implementation
//===----------------------------------------------------------------------===//

void LoadInst::init(Value *Ptr) {
  assert(Ptr && isa<PointerType>(Ptr->getType()) && 
         "Ptr must have pointer type.");
  Operands.reserve(1);
  Operands.push_back(Use(Ptr, this));
}

LoadInst::LoadInst(Value *Ptr, const std::string &Name, Instruction *InsertBef)
  : Instruction(cast<PointerType>(Ptr->getType())->getElementType(),
                Load, Name, InsertBef), Volatile(false) {
  init(Ptr);
}

LoadInst::LoadInst(Value *Ptr, const std::string &Name, BasicBlock *InsertAE)
  : Instruction(cast<PointerType>(Ptr->getType())->getElementType(),
                Load, Name, InsertAE), Volatile(false) {
  init(Ptr);
}

LoadInst::LoadInst(Value *Ptr, const std::string &Name, bool isVolatile,
                   Instruction *InsertBef)
  : Instruction(cast<PointerType>(Ptr->getType())->getElementType(),
                Load, Name, InsertBef), Volatile(isVolatile) {
  init(Ptr);
}

LoadInst::LoadInst(Value *Ptr, const std::string &Name, bool isVolatile,
                   BasicBlock *InsertAE)
  : Instruction(cast<PointerType>(Ptr->getType())->getElementType(),
                Load, Name, InsertAE), Volatile(isVolatile) {
  init(Ptr);
}


//===----------------------------------------------------------------------===//
//                           StoreInst Implementation
//===----------------------------------------------------------------------===//

StoreInst::StoreInst(Value *Val, Value *Ptr, Instruction *InsertBefore)
  : Instruction(Type::VoidTy, Store, "", InsertBefore), Volatile(false) {
  init(Val, Ptr);
}

StoreInst::StoreInst(Value *Val, Value *Ptr, BasicBlock *InsertAtEnd)
  : Instruction(Type::VoidTy, Store, "", InsertAtEnd), Volatile(false) {
  init(Val, Ptr);
}

StoreInst::StoreInst(Value *Val, Value *Ptr, bool isVolatile, 
                     Instruction *InsertBefore)
  : Instruction(Type::VoidTy, Store, "", InsertBefore), Volatile(isVolatile) {
  init(Val, Ptr);
}

StoreInst::StoreInst(Value *Val, Value *Ptr, bool isVolatile, 
                     BasicBlock *InsertAtEnd)
  : Instruction(Type::VoidTy, Store, "", InsertAtEnd), Volatile(isVolatile) {
  init(Val, Ptr);
}

void StoreInst::init(Value *Val, Value *Ptr) {
  assert(isa<PointerType>(Ptr->getType()) &&
         Val->getType() == cast<PointerType>(Ptr->getType())->getElementType()
         && "Ptr must have pointer type.");

  Operands.reserve(2);
  Operands.push_back(Use(Val, this));
  Operands.push_back(Use(Ptr, this));
}

//===----------------------------------------------------------------------===//
//                       GetElementPtrInst Implementation
//===----------------------------------------------------------------------===//

// checkType - Simple wrapper function to give a better assertion failure
// message on bad indexes for a gep instruction.
//
static inline const Type *checkType(const Type *Ty) {
  assert(Ty && "Invalid indices for type!");
  return Ty;
}

void GetElementPtrInst::init(Value *Ptr, const std::vector<Value*> &Idx)
{
  Operands.reserve(1+Idx.size());
  Operands.push_back(Use(Ptr, this));

  for (unsigned i = 0, E = Idx.size(); i != E; ++i)
    Operands.push_back(Use(Idx[i], this));
}

GetElementPtrInst::GetElementPtrInst(Value *Ptr, const std::vector<Value*> &Idx,
				     const std::string &Name, Instruction *InBe)
  : Instruction(PointerType::get(checkType(getIndexedType(Ptr->getType(),
                                                          Idx, true))),
                GetElementPtr, Name, InBe) {
  init(Ptr, Idx);
}

GetElementPtrInst::GetElementPtrInst(Value *Ptr, const std::vector<Value*> &Idx,
				     const std::string &Name, BasicBlock *IAE)
  : Instruction(PointerType::get(checkType(getIndexedType(Ptr->getType(),
                                                          Idx, true))),
                GetElementPtr, Name, IAE) {
  init(Ptr, Idx);
}

// getIndexedType - Returns the type of the element that would be loaded with
// a load instruction with the specified parameters.
//
// A null type is returned if the indices are invalid for the specified 
// pointer type.
//
const Type* GetElementPtrInst::getIndexedType(const Type *Ptr, 
                                              const std::vector<Value*> &Idx,
                                              bool AllowCompositeLeaf) {
  if (!isa<PointerType>(Ptr)) return 0;   // Type isn't a pointer type!

  // Handle the special case of the empty set index set...
  if (Idx.empty())
    if (AllowCompositeLeaf ||
        cast<PointerType>(Ptr)->getElementType()->isFirstClassType())
      return cast<PointerType>(Ptr)->getElementType();
    else
      return 0;
 
  unsigned CurIdx = 0;
  while (const CompositeType *CT = dyn_cast<CompositeType>(Ptr)) {
    if (Idx.size() == CurIdx) {
      if (AllowCompositeLeaf || CT->isFirstClassType()) return Ptr;
      return 0;   // Can't load a whole structure or array!?!?
    }

    Value *Index = Idx[CurIdx++];
    if (isa<PointerType>(CT) && CurIdx != 1)
      return 0;  // Can only index into pointer types at the first index!
    if (!CT->indexValid(Index)) return 0;
    Ptr = CT->getTypeAtIndex(Index);

    // If the new type forwards to another type, then it is in the middle
    // of being refined to another type (and hence, may have dropped all
    // references to what it was using before).  So, use the new forwarded
    // type.
    if (const Type * Ty = Ptr->getForwardedType()) {
      Ptr = Ty;
    }
  }
  return CurIdx == Idx.size() ? Ptr : 0;
}
