//===- InstructionCombining.cpp - Combine multiple instructions -------------=//
//
// InstructionCombining - Combine instructions to form fewer, simple
//   instructions.  This pass does not modify the CFG, and has a tendancy to
//   make instructions dead, so a subsequent DIE pass is useful.  This pass is
//   where algebraic simplification happens.
//
// This pass combines things like:
//    %Y = add int 1, %X
//    %Z = add int 1, %Y
// into:
//    %Z = add int 2, %X
//
// This is a simple worklist driven algorithm.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/InstructionCombining.h"
#include "llvm/ConstantHandling.h"
#include "llvm/iMemory.h"
#include "llvm/iOther.h"
#include "llvm/iOperators.h"
#include "llvm/Pass.h"
#include "llvm/Support/InstIterator.h"
#include "llvm/Support/InstVisitor.h"
#include "../TransformInternals.h"


namespace {
  class InstCombiner : public FunctionPass,
                       public InstVisitor<InstCombiner, Instruction*> {
    // Worklist of all of the instructions that need to be simplified.
    std::vector<Instruction*> WorkList;

    void AddUsesToWorkList(Instruction *I) {
      // The instruction was simplified, add all users of the instruction to
      // the work lists because they might get more simplified now...
      //
      for (Value::use_iterator UI = I->use_begin(), UE = I->use_end();
           UI != UE; ++UI)
        WorkList.push_back(cast<Instruction>(*UI));
    }

  public:
    const char *getPassName() const { return "Instruction Combining"; }

    virtual bool runOnFunction(Function *F);

    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.preservesCFG();
    }

    // Visitation implementation - Implement instruction combining for different
    // instruction types.  The semantics are as follows:
    // Return Value:
    //    null        - No change was made
    //     I          - Change was made, I is still valid
    //   otherwise    - Change was made, replace I with returned instruction
    //   

    Instruction *visitAdd(BinaryOperator *I);
    Instruction *visitSub(BinaryOperator *I);
    Instruction *visitMul(BinaryOperator *I);
    Instruction *visitDiv(BinaryOperator *I);
    Instruction *visitRem(BinaryOperator *I);
    Instruction *visitAnd(BinaryOperator *I);
    Instruction *visitOr (BinaryOperator *I);
    Instruction *visitXor(BinaryOperator *I);
    Instruction *visitSetCondInst(BinaryOperator *I);
    Instruction *visitShiftInst(Instruction *I);
    Instruction *visitCastInst(CastInst *CI);
    Instruction *visitGetElementPtrInst(GetElementPtrInst *GEP);
    Instruction *visitMemAccessInst(MemAccessInst *MAI);

    // visitInstruction - Specify what to return for unhandled instructions...
    Instruction *visitInstruction(Instruction *I) { return 0; }
  };
}



// Make sure that this instruction has a constant on the right hand side if it
// has any constant arguments.  If not, fix it an return true.
//
static bool SimplifyBinOp(BinaryOperator *I) {
  if (isa<Constant>(I->getOperand(0)) && !isa<Constant>(I->getOperand(1)))
    return !I->swapOperands();
  return false;
}

Instruction *InstCombiner::visitAdd(BinaryOperator *I) {
  if (I->use_empty()) return 0;       // Don't fix dead add instructions...
  bool Changed = SimplifyBinOp(I);
  Value *Op1 = I->getOperand(0);

  // Simplify add instructions with a constant RHS...
  if (Constant *Op2 = dyn_cast<Constant>(I->getOperand(1))) {
    // Eliminate 'add int %X, 0'
    if (I->getType()->isIntegral() && Op2->isNullValue()) {
      AddUsesToWorkList(I);         // Add all modified instrs to worklist
      I->replaceAllUsesWith(Op1);
      return I;
    }
 
    if (BinaryOperator *IOp1 = dyn_cast<BinaryOperator>(Op1)) {
      Changed |= SimplifyBinOp(IOp1);
      
      if (IOp1->getOpcode() == Instruction::Add &&
          isa<Constant>(IOp1->getOperand(1))) {
        // Fold:
        //    %Y = add int %X, 1
        //    %Z = add int %Y, 1
        // into:
        //    %Z = add int %X, 2
        //
        if (Constant *Val = *Op2 + *cast<Constant>(IOp1->getOperand(1))) {
          I->setOperand(0, IOp1->getOperand(0));
          I->setOperand(1, Val);
          return I;
        }
      }
    }
  }

  return Changed ? I : 0;
}

Instruction *InstCombiner::visitSub(BinaryOperator *I) {
  if (I->use_empty()) return 0;       // Don't fix dead add instructions...
  Value *Op0 = I->getOperand(0), *Op1 = I->getOperand(1);

  if (Op0 == Op1) {         // sub X, X  -> 0
    AddUsesToWorkList(I);         // Add all modified instrs to worklist
    I->replaceAllUsesWith(Constant::getNullValue(I->getType()));
    return I;
  }

  // If this is a subtract instruction with a constant RHS, convert it to an add
  // instruction of a negative constant
  //
  if (Constant *Op2 = dyn_cast<Constant>(Op1))
    if (Constant *RHS = *Constant::getNullValue(I->getType()) - *Op2) // 0 - RHS
      return BinaryOperator::create(Instruction::Add, Op0, RHS, I->getName());

  return 0;
}

Instruction *InstCombiner::visitMul(BinaryOperator *I) {
  if (I->use_empty()) return 0;       // Don't fix dead instructions...
  bool Changed = SimplifyBinOp(I);
  Value *Op1 = I->getOperand(0);

  // Simplify add instructions with a constant RHS...
  if (Constant *Op2 = dyn_cast<Constant>(I->getOperand(1))) {
    if (I->getType()->isIntegral() && cast<ConstantInt>(Op2)->equalsInt(1)){
      // Eliminate 'mul int %X, 1'
      AddUsesToWorkList(I);         // Add all modified instrs to worklist
      I->replaceAllUsesWith(Op1);
      return I;

    } else if (I->getType()->isIntegral() &&
               cast<ConstantInt>(Op2)->equalsInt(2)) {
      // Convert 'mul int %X, 2' to 'add int %X, %X'
      return BinaryOperator::create(Instruction::Add, Op1, Op1, I->getName());

    } else if (Op2->isNullValue()) {
      // Eliminate 'mul int %X, 0'
      AddUsesToWorkList(I);         // Add all modified instrs to worklist
      I->replaceAllUsesWith(Op2);   // Set this value to zero directly
      return I;
    }
  }

  return Changed ? I : 0;
}


Instruction *InstCombiner::visitDiv(BinaryOperator *I) {
  if (I->use_empty()) return 0;       // Don't fix dead instructions...

  // div X, 1 == X
  if (ConstantInt *RHS = dyn_cast<ConstantInt>(I->getOperand(1)))
    if (RHS->equalsInt(1)) {
      AddUsesToWorkList(I);         // Add all modified instrs to worklist
      I->replaceAllUsesWith(I->getOperand(0));
      return I;
    }
  return 0;
}


Instruction *InstCombiner::visitRem(BinaryOperator *I) {
  if (I->use_empty()) return 0;       // Don't fix dead instructions...

  // rem X, 1 == 0
  if (ConstantInt *RHS = dyn_cast<ConstantInt>(I->getOperand(1)))
    if (RHS->equalsInt(1)) {
      AddUsesToWorkList(I);         // Add all modified instrs to worklist
      I->replaceAllUsesWith(Constant::getNullValue(I->getType()));
      return I;
    }
  return 0;
}

static Constant *getMaxValue(const Type *Ty) {
  assert(Ty == Type::BoolTy || Ty->isIntegral());
  if (Ty == Type::BoolTy)
    return ConstantBool::True;

  // Calculate -1 casted to the right type...
  unsigned TypeBits = Ty->getPrimitiveSize()*8;
  uint64_t Val = (uint64_t)-1LL;       // All ones
  Val >>= 64-TypeBits;                 // Shift out unwanted 1 bits...

  if (Ty->isSigned())
    return ConstantSInt::get(Ty, (int64_t)Val);
  else if (Ty->isUnsigned())
    return ConstantUInt::get(Ty, Val);

  return 0;
}


Instruction *InstCombiner::visitAnd(BinaryOperator *I) {
  if (I->use_empty()) return 0;       // Don't fix dead instructions...
  bool Changed = SimplifyBinOp(I);
  Value *Op0 = I->getOperand(0), *Op1 = I->getOperand(1);

  // and X, X = X   and X, 0 == 0
  if (Op0 == Op1 || Op1 == Constant::getNullValue(I->getType())) {
    AddUsesToWorkList(I);         // Add all modified instrs to worklist
    I->replaceAllUsesWith(Op1);
    return I;
  }

  // and X, -1 == X
  if (Constant *RHS = dyn_cast<Constant>(Op1))
    if (RHS == getMaxValue(I->getType())) {
      AddUsesToWorkList(I);         // Add all modified instrs to worklist
      I->replaceAllUsesWith(Op0);
      return I;
    }

  return Changed ? I : 0;
}



Instruction *InstCombiner::visitOr(BinaryOperator *I) {
  if (I->use_empty()) return 0;       // Don't fix dead instructions...
  bool Changed = SimplifyBinOp(I);
  Value *Op0 = I->getOperand(0), *Op1 = I->getOperand(1);

  // or X, X = X   or X, 0 == X
  if (Op0 == Op1 || Op1 == Constant::getNullValue(I->getType())) {
    AddUsesToWorkList(I);         // Add all modified instrs to worklist
    I->replaceAllUsesWith(Op0);
    return I;
  }

  // or X, -1 == -1
  if (Constant *RHS = dyn_cast<Constant>(Op1))
    if (RHS == getMaxValue(I->getType())) {
      AddUsesToWorkList(I);         // Add all modified instrs to worklist
      I->replaceAllUsesWith(Op1);
      return I;
    }

  return Changed ? I : 0;
}



Instruction *InstCombiner::visitXor(BinaryOperator *I) {
  if (I->use_empty()) return 0;       // Don't fix dead instructions...
  bool Changed = SimplifyBinOp(I);
  Value *Op0 = I->getOperand(0), *Op1 = I->getOperand(1);

  // xor X, X = 0
  if (Op0 == Op1) {
    AddUsesToWorkList(I);         // Add all modified instrs to worklist
    I->replaceAllUsesWith(Constant::getNullValue(I->getType()));
    return I;
  }

  // xor X, 0 == X
  if (Op1 == Constant::getNullValue(I->getType())) {
    AddUsesToWorkList(I);         // Add all modified instrs to worklist
    I->replaceAllUsesWith(Op0);
    return I;
  }

  return Changed ? I : 0;
}

Instruction *InstCombiner::visitSetCondInst(BinaryOperator *I) {
  if (I->use_empty()) return 0;       // Don't fix dead instructions...
  bool Changed = SimplifyBinOp(I);

  // setcc X, X
  if (I->getOperand(0) == I->getOperand(1)) {
    bool NewVal = I->getOpcode() == Instruction::SetEQ ||
                  I->getOpcode() == Instruction::SetGE ||
                  I->getOpcode() == Instruction::SetLE;
    AddUsesToWorkList(I);         // Add all modified instrs to worklist
    I->replaceAllUsesWith(ConstantBool::get(NewVal));
    return I;
  }

  return Changed ? I : 0;
}



Instruction *InstCombiner::visitShiftInst(Instruction *I) {
  if (I->use_empty()) return 0;       // Don't fix dead instructions...
  assert(I->getOperand(1)->getType() == Type::UByteTy);
  Value *Op0 = I->getOperand(0), *Op1 = I->getOperand(1);

  // shl X, 0 == X and shr X, 0 == X
  // shl 0, X == 0 and shr 0, X == 0
  if (Op1 == Constant::getNullValue(Type::UByteTy) ||
      Op0 == Constant::getNullValue(Op0->getType())) {
    AddUsesToWorkList(I);         // Add all modified instrs to worklist
    I->replaceAllUsesWith(Op0);
    return I;
  }

  // shl int X, 32 = 0 and shr sbyte Y, 9 = 0, ... just don't eliminate shr of
  // a signed value.
  //
  if (ConstantUInt *CUI = dyn_cast<ConstantUInt>(Op1)) {
    unsigned TypeBits = Op0->getType()->getPrimitiveSize()*8;
    if (CUI->getValue() >= TypeBits &&
        !(Op0->getType()->isSigned() && I->getOpcode() == Instruction::Shr)) {
      AddUsesToWorkList(I);         // Add all modified instrs to worklist
      I->replaceAllUsesWith(Constant::getNullValue(Op0->getType()));
      return I;
    }
  }
  return 0;
}


// isEliminableCastOfCast - Return true if it is valid to eliminate the CI
// instruction.
//
static inline bool isEliminableCastOfCast(const CastInst *CI,
                                          const CastInst *CSrc) {
  assert(CI->getOperand(0) == CSrc);
  const Type *SrcTy = CSrc->getOperand(0)->getType();
  const Type *MidTy = CSrc->getType();
  const Type *DstTy = CI->getType();

  // It is legal to eliminate the instruction if casting A->B->A
  if (SrcTy == DstTy) return true;

  // Allow free casting and conversion of sizes as long as the sign doesn't
  // change...
  if (SrcTy->isSigned() == MidTy->isSigned() &&
      MidTy->isSigned() == DstTy->isSigned())
    return true;

  // Otherwise, we cannot succeed.  Specifically we do not want to allow things
  // like:  short -> ushort -> uint, because this can create wrong results if
  // the input short is negative!
  //
  return false;
}


// CastInst simplification
//
Instruction *InstCombiner::visitCastInst(CastInst *CI) {
  // If the user is casting a value to the same type, eliminate this cast
  // instruction...
  if (CI->getType() == CI->getOperand(0)->getType() && !CI->use_empty()) {
    AddUsesToWorkList(CI);         // Add all modified instrs to worklist
    CI->replaceAllUsesWith(CI->getOperand(0));
    return CI;
  }


  // If casting the result of another cast instruction, try to eliminate this
  // one!
  //
  if (CastInst *CSrc = dyn_cast<CastInst>(CI->getOperand(0)))
    if (isEliminableCastOfCast(CI, CSrc)) {
      // This instruction now refers directly to the cast's src operand.  This
      // has a good chance of making CSrc dead.
      CI->setOperand(0, CSrc->getOperand(0));
      return CI;
    }

  return 0;
}



Instruction *InstCombiner::visitGetElementPtrInst(GetElementPtrInst *GEP) {
  // Is it getelementptr %P, uint 0
  // If so, elminate the noop.
  if (GEP->getNumOperands() == 2 && !GEP->use_empty() &&
      GEP->getOperand(1) == Constant::getNullValue(Type::UIntTy)) {
    AddUsesToWorkList(GEP);         // Add all modified instrs to worklist
    GEP->replaceAllUsesWith(GEP->getOperand(0));
    return GEP;
  }

  return visitMemAccessInst(GEP);
}


// Combine Indices - If the source pointer to this mem access instruction is a
// getelementptr instruction, combine the indices of the GEP into this
// instruction
//
Instruction *InstCombiner::visitMemAccessInst(MemAccessInst *MAI) {
  GetElementPtrInst *Src =
    dyn_cast<GetElementPtrInst>(MAI->getPointerOperand());
  if (!Src) return 0;

  std::vector<Value *> Indices;
  
  // Only special case we have to watch out for is pointer arithmetic on the
  // 0th index of MAI. 
  unsigned FirstIdx = MAI->getFirstIndexOperandNumber();
  if (FirstIdx == MAI->getNumOperands() || 
      (FirstIdx == MAI->getNumOperands()-1 &&
       MAI->getOperand(FirstIdx) == ConstantUInt::get(Type::UIntTy, 0))) { 
    // Replace the index list on this MAI with the index on the getelementptr
    Indices.insert(Indices.end(), Src->idx_begin(), Src->idx_end());
  } else if (*MAI->idx_begin() == ConstantUInt::get(Type::UIntTy, 0)) { 
    // Otherwise we can do the fold if the first index of the GEP is a zero
    Indices.insert(Indices.end(), Src->idx_begin(), Src->idx_end());
    Indices.insert(Indices.end(), MAI->idx_begin()+1, MAI->idx_end());
  }

  if (Indices.empty()) return 0;  // Can't do the fold?

  switch (MAI->getOpcode()) {
  case Instruction::GetElementPtr:
    return new GetElementPtrInst(Src->getOperand(0), Indices, MAI->getName());
  case Instruction::Load:
    return new LoadInst(Src->getOperand(0), Indices, MAI->getName());
  case Instruction::Store:
    return new StoreInst(MAI->getOperand(0), Src->getOperand(0), Indices);
  default:
    assert(0 && "Unknown memaccessinst!");
    break;
  }
  abort();
  return 0;
}


bool InstCombiner::runOnFunction(Function *F) {
  bool Changed = false;

  WorkList.insert(WorkList.end(), inst_begin(F), inst_end(F));

  while (!WorkList.empty()) {
    Instruction *I = WorkList.back();  // Get an instruction from the worklist
    WorkList.pop_back();

    // Now that we have an instruction, try combining it to simplify it...
    Instruction *Result = visit(I);
    if (Result) {
      // Should we replace the old instruction with a new one?
      if (Result != I)
        ReplaceInstWithInst(I, Result);

      WorkList.push_back(Result);
      AddUsesToWorkList(Result);
      Changed = true;
    }
  }

  return Changed;
}

Pass *createInstructionCombiningPass() {
  return new InstCombiner();
}
