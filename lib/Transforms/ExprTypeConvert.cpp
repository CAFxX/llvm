//===- ExprTypeConvert.cpp - Code to change an LLVM Expr Type ---------------=//
//
// This file implements the part of level raising that checks to see if it is
// possible to coerce an entire expression tree into a different type.  If
// convertable, other routines from this file will do the conversion.
//
//===----------------------------------------------------------------------===//

#include "TransformInternals.h"
#include "llvm/Method.h"
#include "llvm/iOther.h"
#include "llvm/iPHINode.h"
#include "llvm/iMemory.h"
#include "llvm/ConstantVals.h"
#include "llvm/Transforms/Scalar/ConstantHandling.h"
#include "llvm/Transforms/Scalar/DCE.h"
#include "llvm/Analysis/Expressions.h"
#include "Support/STLExtras.h"
#include <map>
#include <algorithm>
#include <iostream>
using std::cerr;

#include "llvm/Assembly/Writer.h"

//#define DEBUG_EXPR_CONVERT 1

static bool OperandConvertableToType(User *U, Value *V, const Type *Ty,
                                     ValueTypeCache &ConvertedTypes);

static void ConvertOperandToType(User *U, Value *OldVal, Value *NewVal,
                                 ValueMapCache &VMC);

// AllIndicesZero - Return true if all of the indices of the specified memory
// access instruction are zero, indicating an effectively nil offset to the 
// pointer value.
//
static bool AllIndicesZero(const MemAccessInst *MAI) {
  for (User::const_op_iterator S = MAI->idx_begin(), E = MAI->idx_end();
       S != E; ++S)
    if (!isa<Constant>(*S) || !cast<Constant>(*S)->isNullValue())
      return false;
  return true;
}


// Peephole Malloc instructions: we take a look at the use chain of the
// malloc instruction, and try to find out if the following conditions hold:
//   1. The malloc is of the form: 'malloc [sbyte], uint <constant>'
//   2. The only users of the malloc are cast & add instructions
//   3. Of the cast instructions, there is only one destination pointer type
//      [RTy] where the size of the pointed to object is equal to the number
//      of bytes allocated.
//
// If these conditions hold, we convert the malloc to allocate an [RTy]
// element.  TODO: This comment is out of date WRT arrays
//
static bool MallocConvertableToType(MallocInst *MI, const Type *Ty,
                                    ValueTypeCache &CTMap) {
  if (!isa<PointerType>(Ty)) return false;   // Malloc always returns pointers

  // Deal with the type to allocate, not the pointer type...
  Ty = cast<PointerType>(Ty)->getElementType();
  if (!Ty->isSized()) return false;      // Can only alloc something with a size

  // Analyze the number of bytes allocated...
  analysis::ExprType Expr = analysis::ClassifyExpression(MI->getArraySize());

  // Get information about the base datatype being allocated, before & after
  unsigned ReqTypeSize = TD.getTypeSize(Ty);
  unsigned OldTypeSize = TD.getTypeSize(MI->getType()->getElementType());

  // Must have a scale or offset to analyze it...
  if (!Expr.Offset && !Expr.Scale) return false;

  // Get the offset and scale of the allocation...
  int OffsetVal = Expr.Offset ? getConstantValue(Expr.Offset) : 0;
  int ScaleVal = Expr.Scale ? getConstantValue(Expr.Scale) : (Expr.Var ? 1 : 0);
  if (ScaleVal < 0 || OffsetVal < 0) {
    cerr << "malloc of a negative number???\n";
    return false;
  }

  // The old type might not be of unit size, take old size into consideration
  // here...
  unsigned Offset = (unsigned)OffsetVal * OldTypeSize;
  unsigned Scale  = (unsigned)ScaleVal  * OldTypeSize;
  
  // In order to be successful, both the scale and the offset must be a multiple
  // of the requested data type's size.
  //
  if (Offset/ReqTypeSize*ReqTypeSize != Offset ||
      Scale/ReqTypeSize*ReqTypeSize != Scale)
    return false;   // Nope.

  return true;
}

static Instruction *ConvertMallocToType(MallocInst *MI, const Type *Ty,
                                        const std::string &Name,
                                        ValueMapCache &VMC){
  BasicBlock *BB = MI->getParent();
  BasicBlock::iterator It = BB->end();

  // Analyze the number of bytes allocated...
  analysis::ExprType Expr = analysis::ClassifyExpression(MI->getArraySize());

  const PointerType *AllocTy = cast<PointerType>(Ty);
  const Type *ElType = AllocTy->getElementType();

  unsigned DataSize = TD.getTypeSize(ElType);
  unsigned OldTypeSize = TD.getTypeSize(MI->getType()->getElementType());

  // Get the offset and scale coefficients that we are allocating...
  int OffsetVal = (Expr.Offset ? getConstantValue(Expr.Offset) : 0);
  int ScaleVal = Expr.Scale ? getConstantValue(Expr.Scale) : (Expr.Var ? 1 : 0);

  // The old type might not be of unit size, take old size into consideration
  // here...
  unsigned Offset = (unsigned)OffsetVal * OldTypeSize / DataSize;
  unsigned Scale  = (unsigned)ScaleVal  * OldTypeSize / DataSize;

  // Locate the malloc instruction, because we may be inserting instructions
  It = find(BB->getInstList().begin(), BB->getInstList().end(), MI);

  // If we have a scale, apply it first...
  if (Expr.Var) {
    // Expr.Var is not neccesarily unsigned right now, insert a cast now.
    if (Expr.Var->getType() != Type::UIntTy) {
      Instruction *CI = new CastInst(Expr.Var, Type::UIntTy);
      if (Expr.Var->hasName()) CI->setName(Expr.Var->getName()+"-uint");
      It = BB->getInstList().insert(It, CI)+1;
      Expr.Var = CI;
    }

    if (Scale != 1) {
      Instruction *ScI =
        BinaryOperator::create(Instruction::Mul, Expr.Var,
                               ConstantUInt::get(Type::UIntTy, Scale));
      if (Expr.Var->hasName()) ScI->setName(Expr.Var->getName()+"-scl");
      It = BB->getInstList().insert(It, ScI)+1;
      Expr.Var = ScI;
    }

  } else {
    // If we are not scaling anything, just make the offset be the "var"...
    Expr.Var = ConstantUInt::get(Type::UIntTy, Offset);
    Offset = 0; Scale = 1;
  }

  // If we have an offset now, add it in...
  if (Offset != 0) {
    assert(Expr.Var && "Var must be nonnull by now!");

    Instruction *AddI =
      BinaryOperator::create(Instruction::Add, Expr.Var,
                             ConstantUInt::get(Type::UIntTy, Offset));
    if (Expr.Var->hasName()) AddI->setName(Expr.Var->getName()+"-off");
    It = BB->getInstList().insert(It, AddI)+1;
    Expr.Var = AddI;
  }

  Instruction *NewI = new MallocInst(AllocTy, Expr.Var, Name);

  assert(AllocTy == Ty);
  return NewI;
}


// ExpressionConvertableToType - Return true if it is possible
bool ExpressionConvertableToType(Value *V, const Type *Ty,
                                 ValueTypeCache &CTMap) {
  if (V->getType() == Ty) return true;  // Expression already correct type!

  // Expression type must be holdable in a register.
  if (!Ty->isFirstClassType())
    return false;
  
  ValueTypeCache::iterator CTMI = CTMap.find(V);
  if (CTMI != CTMap.end()) return CTMI->second == Ty;

  CTMap[V] = Ty;

  Instruction *I = dyn_cast<Instruction>(V);
  if (I == 0) {
    // It's not an instruction, check to see if it's a constant... all constants
    // can be converted to an equivalent value (except pointers, they can't be
    // const prop'd in general).  We just ask the constant propogator to see if
    // it can convert the value...
    //
    if (Constant *CPV = dyn_cast<Constant>(V))
      if (ConstantFoldCastInstruction(CPV, Ty))
        return true;  // Don't worry about deallocating, it's a constant.

    return false;              // Otherwise, we can't convert!
  }

  switch (I->getOpcode()) {
  case Instruction::Cast:
    // We can convert the expr if the cast destination type is losslessly
    // convertable to the requested type.
    if (!Ty->isLosslesslyConvertableTo(I->getType())) return false;
#if 1
    // We also do not allow conversion of a cast that casts from a ptr to array
    // of X to a *X.  For example: cast [4 x %List *] * %val to %List * *
    //
    if (PointerType *SPT = dyn_cast<PointerType>(I->getOperand(0)->getType()))
      if (PointerType *DPT = dyn_cast<PointerType>(I->getType()))
        if (ArrayType *AT = dyn_cast<ArrayType>(SPT->getElementType()))
          if (AT->getElementType() == DPT->getElementType())
            return false;
#endif
    break;

  case Instruction::Add:
  case Instruction::Sub:
    if (!ExpressionConvertableToType(I->getOperand(0), Ty, CTMap) ||
        !ExpressionConvertableToType(I->getOperand(1), Ty, CTMap))
      return false;
    break;
  case Instruction::Shr:
    if (Ty->isSigned() != V->getType()->isSigned()) return false;
    // FALL THROUGH
  case Instruction::Shl:
    if (!ExpressionConvertableToType(I->getOperand(0), Ty, CTMap))
      return false;
    break;

  case Instruction::Load: {
    LoadInst *LI = cast<LoadInst>(I);
    if (LI->hasIndices() && !AllIndicesZero(LI)) {
      // We can't convert a load expression if it has indices... unless they are
      // all zero.
      return false;
    }

    if (!ExpressionConvertableToType(LI->getPointerOperand(),
                                     PointerType::get(Ty), CTMap))
      return false;
    break;                                     
  }
  case Instruction::PHINode: {
    PHINode *PN = cast<PHINode>(I);
    for (unsigned i = 0; i < PN->getNumIncomingValues(); ++i)
      if (!ExpressionConvertableToType(PN->getIncomingValue(i), Ty, CTMap))
        return false;
    break;
  }

  case Instruction::Malloc:
    if (!MallocConvertableToType(cast<MallocInst>(I), Ty, CTMap))
      return false;
    break;

#if 1
  case Instruction::GetElementPtr: {
    // GetElementPtr's are directly convertable to a pointer type if they have
    // a number of zeros at the end.  Because removing these values does not
    // change the logical offset of the GEP, it is okay and fair to remove them.
    // This can change this:
    //   %t1 = getelementptr %Hosp * %hosp, ubyte 4, ubyte 0  ; <%List **>
    //   %t2 = cast %List * * %t1 to %List *
    // into
    //   %t2 = getelementptr %Hosp * %hosp, ubyte 4           ; <%List *>
    // 
    GetElementPtrInst *GEP = cast<GetElementPtrInst>(I);
    const PointerType *PTy = dyn_cast<PointerType>(Ty);
    if (!PTy) return false;  // GEP must always return a pointer...
    const Type *PVTy = PTy->getElementType();

    // Check to see if there are zero elements that we can remove from the
    // index array.  If there are, check to see if removing them causes us to
    // get to the right type...
    //
    std::vector<Value*> Indices = GEP->copyIndices();
    const Type *BaseType = GEP->getPointerOperand()->getType();
    const Type *ElTy = 0;

    while (!Indices.empty() && isa<ConstantUInt>(Indices.back()) &&
           cast<ConstantUInt>(Indices.back())->getValue() == 0) {
      Indices.pop_back();
      ElTy = GetElementPtrInst::getIndexedType(BaseType, Indices, true);
      if (ElTy == PVTy)
        break;  // Found a match!!
      ElTy = 0;
    }

    if (ElTy) break;   // Found a number of zeros we can strip off!

    // Otherwise, we can convert a GEP from one form to the other iff the
    // current gep is of the form 'getelementptr sbyte*, unsigned N
    // and we could convert this to an appropriate GEP for the new type.
    //
    if (GEP->getNumOperands() == 2 &&
        GEP->getOperand(1)->getType() == Type::UIntTy &&
        GEP->getType() == PointerType::get(Type::SByteTy)) {

      // Do not Check to see if our incoming pointer can be converted
      // to be a ptr to an array of the right type... because in more cases than
      // not, it is simply not analyzable because of pointer/array
      // discrepencies.  To fix this, we will insert a cast before the GEP.
      //

      // Check to see if 'N' is an expression that can be converted to
      // the appropriate size... if so, allow it.
      //
      std::vector<Value*> Indices;
      const Type *ElTy = ConvertableToGEP(PTy, I->getOperand(1), Indices);
      if (ElTy == PVTy) {
        if (!ExpressionConvertableToType(I->getOperand(0),
                                         PointerType::get(ElTy), CTMap))
          return false;  // Can't continue, ExConToTy might have polluted set!
        break;
      }
    }

    // Otherwise, it could be that we have something like this:
    //     getelementptr [[sbyte] *] * %reg115, uint %reg138    ; [sbyte]**
    // and want to convert it into something like this:
    //     getelemenptr [[int] *] * %reg115, uint %reg138      ; [int]**
    //
    if (GEP->getNumOperands() == 2 && 
        GEP->getOperand(1)->getType() == Type::UIntTy &&
        TD.getTypeSize(PTy->getElementType()) == 
        TD.getTypeSize(GEP->getType()->getElementType())) {
      const PointerType *NewSrcTy = PointerType::get(PVTy);
      if (!ExpressionConvertableToType(I->getOperand(0), NewSrcTy, CTMap))
        return false;
      break;
    }

    return false;   // No match, maybe next time.
  }
#endif

  default:
    return false;
  }

  // Expressions are only convertable if all of the users of the expression can
  // have this value converted.  This makes use of the map to avoid infinite
  // recursion.
  //
  for (Value::use_iterator It = I->use_begin(), E = I->use_end(); It != E; ++It)
    if (!OperandConvertableToType(*It, I, Ty, CTMap))
      return false;

  return true;
}


Value *ConvertExpressionToType(Value *V, const Type *Ty, ValueMapCache &VMC) {
  if (V->getType() == Ty) return V;  // Already where we need to be?

  ValueMapCache::ExprMapTy::iterator VMCI = VMC.ExprMap.find(V);
  if (VMCI != VMC.ExprMap.end()) {
    assert(VMCI->second->getType() == Ty);

    if (Instruction *I = dyn_cast<Instruction>(V))
      ValueHandle IHandle(VMC, I);  // Remove I if it is unused now!

    return VMCI->second;
  }

#ifdef DEBUG_EXPR_CONVERT
  cerr << "CETT: " << (void*)V << " " << V;
#endif

  Instruction *I = dyn_cast<Instruction>(V);
  if (I == 0)
    if (Constant *CPV = cast<Constant>(V)) {
      // Constants are converted by constant folding the cast that is required.
      // We assume here that all casts are implemented for constant prop.
      Value *Result = ConstantFoldCastInstruction(CPV, Ty);
      assert(Result && "ConstantFoldCastInstruction Failed!!!");
      assert(Result->getType() == Ty && "Const prop of cast failed!");

      // Add the instruction to the expression map
      VMC.ExprMap[V] = Result;
      return Result;
    }


  BasicBlock *BB = I->getParent();
  BasicBlock::InstListType &BIL = BB->getInstList();
  std::string Name = I->getName();  if (!Name.empty()) I->setName("");
  Instruction *Res;     // Result of conversion

  ValueHandle IHandle(VMC, I);  // Prevent I from being removed!
  
  Constant *Dummy = Constant::getNullConstant(Ty);

  switch (I->getOpcode()) {
  case Instruction::Cast:
    Res = new CastInst(I->getOperand(0), Ty, Name);
    break;
    
  case Instruction::Add:
  case Instruction::Sub:
    Res = BinaryOperator::create(cast<BinaryOperator>(I)->getOpcode(),
                                 Dummy, Dummy, Name);
    VMC.ExprMap[I] = Res;   // Add node to expression eagerly

    Res->setOperand(0, ConvertExpressionToType(I->getOperand(0), Ty, VMC));
    Res->setOperand(1, ConvertExpressionToType(I->getOperand(1), Ty, VMC));
    break;

  case Instruction::Shl:
  case Instruction::Shr:
    Res = new ShiftInst(cast<ShiftInst>(I)->getOpcode(), Dummy,
                        I->getOperand(1), Name);
    VMC.ExprMap[I] = Res;
    Res->setOperand(0, ConvertExpressionToType(I->getOperand(0), Ty, VMC));
    break;

  case Instruction::Load: {
    LoadInst *LI = cast<LoadInst>(I);
    assert(!LI->hasIndices() || AllIndicesZero(LI));

    Res = new LoadInst(Constant::getNullConstant(PointerType::get(Ty)), Name);
    VMC.ExprMap[I] = Res;
    Res->setOperand(0, ConvertExpressionToType(LI->getPointerOperand(),
                                               PointerType::get(Ty), VMC));
    assert(Res->getOperand(0)->getType() == PointerType::get(Ty));
    assert(Ty == Res->getType());
    assert(Res->getType()->isFirstClassType() && "Load of structure or array!");
    break;
  }

  case Instruction::PHINode: {
    PHINode *OldPN = cast<PHINode>(I);
    PHINode *NewPN = new PHINode(Ty, Name);

    VMC.ExprMap[I] = NewPN;   // Add node to expression eagerly
    while (OldPN->getNumOperands()) {
      BasicBlock *BB = OldPN->getIncomingBlock(0);
      Value *OldVal = OldPN->getIncomingValue(0);
      ValueHandle OldValHandle(VMC, OldVal);
      OldPN->removeIncomingValue(BB);
      Value *V = ConvertExpressionToType(OldVal, Ty, VMC);
      NewPN->addIncoming(V, BB);
    }
    Res = NewPN;
    break;
  }

  case Instruction::Malloc: {
    Res = ConvertMallocToType(cast<MallocInst>(I), Ty, Name, VMC);
    break;
  }

  case Instruction::GetElementPtr: {
    // GetElementPtr's are directly convertable to a pointer type if they have
    // a number of zeros at the end.  Because removing these values does not
    // change the logical offset of the GEP, it is okay and fair to remove them.
    // This can change this:
    //   %t1 = getelementptr %Hosp * %hosp, ubyte 4, ubyte 0  ; <%List **>
    //   %t2 = cast %List * * %t1 to %List *
    // into
    //   %t2 = getelementptr %Hosp * %hosp, ubyte 4           ; <%List *>
    // 
    GetElementPtrInst *GEP = cast<GetElementPtrInst>(I);

    // Check to see if there are zero elements that we can remove from the
    // index array.  If there are, check to see if removing them causes us to
    // get to the right type...
    //
    std::vector<Value*> Indices = GEP->copyIndices();
    const Type *BaseType = GEP->getPointerOperand()->getType();
    const Type *PVTy = cast<PointerType>(Ty)->getElementType();
    Res = 0;
    while (!Indices.empty() && isa<ConstantUInt>(Indices.back()) &&
           cast<ConstantUInt>(Indices.back())->getValue() == 0) {
      Indices.pop_back();
      if (GetElementPtrInst::getIndexedType(BaseType, Indices, true) == PVTy) {
        if (Indices.size() == 0) {
          Res = new CastInst(GEP->getPointerOperand(), BaseType); // NOOP
        } else {
          Res = new GetElementPtrInst(GEP->getPointerOperand(), Indices, Name);
        }
        break;
      }
    }

    if (Res == 0 && GEP->getNumOperands() == 2 &&
        GEP->getOperand(1)->getType() == Type::UIntTy &&
        GEP->getType() == PointerType::get(Type::SByteTy)) {
      
      // Otherwise, we can convert a GEP from one form to the other iff the
      // current gep is of the form 'getelementptr [sbyte]*, unsigned N
      // and we could convert this to an appropriate GEP for the new type.
      //
      const PointerType *NewSrcTy = PointerType::get(PVTy);
      BasicBlock::iterator It = find(BIL.begin(), BIL.end(), I);

      // Check to see if 'N' is an expression that can be converted to
      // the appropriate size... if so, allow it.
      //
      std::vector<Value*> Indices;
      const Type *ElTy = ConvertableToGEP(NewSrcTy, I->getOperand(1),
                                          Indices, &It);
      if (ElTy) {        
        assert(ElTy == PVTy && "Internal error, setup wrong!");
        Res = new GetElementPtrInst(Constant::getNullConstant(NewSrcTy),
                                    Indices, Name);
        VMC.ExprMap[I] = Res;
        Res->setOperand(0, ConvertExpressionToType(I->getOperand(0),
                                                   NewSrcTy, VMC));
      }
    }

    // Otherwise, it could be that we have something like this:
    //     getelementptr [[sbyte] *] * %reg115, uint %reg138    ; [sbyte]**
    // and want to convert it into something like this:
    //     getelemenptr [[int] *] * %reg115, uint %reg138      ; [int]**
    //
    if (Res == 0) {
      const PointerType *NewSrcTy = PointerType::get(PVTy);
      Res = new GetElementPtrInst(Constant::getNullConstant(NewSrcTy),
                                  GEP->copyIndices(), Name);
      VMC.ExprMap[I] = Res;
      Res->setOperand(0, ConvertExpressionToType(I->getOperand(0),
                                                 NewSrcTy, VMC));
    }


    assert(Res && "Didn't find match!");
    break;   // No match, maybe next time.
  }

  default:
    assert(0 && "Expression convertable, but don't know how to convert?");
    return 0;
  }

  assert(Res->getType() == Ty && "Didn't convert expr to correct type!");

  BasicBlock::iterator It = find(BIL.begin(), BIL.end(), I);
  assert(It != BIL.end() && "Instruction not in own basic block??");
  BIL.insert(It, Res);

  // Add the instruction to the expression map
  VMC.ExprMap[I] = Res;

  // Expressions are only convertable if all of the users of the expression can
  // have this value converted.  This makes use of the map to avoid infinite
  // recursion.
  //
  unsigned NumUses = I->use_size();
  for (unsigned It = 0; It < NumUses; ) {
    unsigned OldSize = NumUses;
    ConvertOperandToType(*(I->use_begin()+It), I, Res, VMC);
    NumUses = I->use_size();
    if (NumUses == OldSize) ++It;
  }

#ifdef DEBUG_EXPR_CONVERT
  cerr << "ExpIn: " << (void*)I << " " << I
       << "ExpOut: " << (void*)Res << " " << Res;
#endif

  if (I->use_empty()) {
#ifdef DEBUG_EXPR_CONVERT
    cerr << "EXPR DELETING: " << (void*)I << " " << I;
#endif
    BIL.remove(I);
    VMC.OperandsMapped.erase(I);
    VMC.ExprMap.erase(I);
    delete I;
  }

  return Res;
}



// ValueConvertableToType - Return true if it is possible
bool ValueConvertableToType(Value *V, const Type *Ty,
                             ValueTypeCache &ConvertedTypes) {
  ValueTypeCache::iterator I = ConvertedTypes.find(V);
  if (I != ConvertedTypes.end()) return I->second == Ty;
  ConvertedTypes[V] = Ty;

  // It is safe to convert the specified value to the specified type IFF all of
  // the uses of the value can be converted to accept the new typed value.
  //
  if (V->getType() != Ty) {
    for (Value::use_iterator I = V->use_begin(), E = V->use_end(); I != E; ++I)
      if (!OperandConvertableToType(*I, V, Ty, ConvertedTypes))
        return false;
  }

  return true;
}





// OperandConvertableToType - Return true if it is possible to convert operand
// V of User (instruction) U to the specified type.  This is true iff it is
// possible to change the specified instruction to accept this.  CTMap is a map
// of converted types, so that circular definitions will see the future type of
// the expression, not the static current type.
//
static bool OperandConvertableToType(User *U, Value *V, const Type *Ty,
                                     ValueTypeCache &CTMap) {
  //  if (V->getType() == Ty) return true;   // Operand already the right type?

  // Expression type must be holdable in a register.
  if (!Ty->isFirstClassType())
    return false;

  Instruction *I = dyn_cast<Instruction>(U);
  if (I == 0) return false;              // We can't convert!

  switch (I->getOpcode()) {
  case Instruction::Cast:
    assert(I->getOperand(0) == V);
    // We can convert the expr if the cast destination type is losslessly
    // convertable to the requested type.
    // Also, do not change a cast that is a noop cast.  For all intents and
    // purposes it should be eliminated.
    if (!Ty->isLosslesslyConvertableTo(I->getOperand(0)->getType()) ||
        I->getType() == I->getOperand(0)->getType())
      return false;


#if 1
    // We also do not allow conversion of a cast that casts from a ptr to array
    // of X to a *X.  For example: cast [4 x %List *] * %val to %List * *
    //
    if (PointerType *SPT = dyn_cast<PointerType>(I->getOperand(0)->getType()))
      if (PointerType *DPT = dyn_cast<PointerType>(I->getType()))
        if (ArrayType *AT = dyn_cast<ArrayType>(SPT->getElementType()))
          if (AT->getElementType() == DPT->getElementType())
            return false;
#endif
    return true;

  case Instruction::Add:
    if (isa<PointerType>(Ty)) {
      Value *IndexVal = I->getOperand(V == I->getOperand(0) ? 1 : 0);
      std::vector<Value*> Indices;
      if (const Type *ETy = ConvertableToGEP(Ty, IndexVal, Indices)) {
        const Type *RetTy = PointerType::get(ETy);

        // Only successful if we can convert this type to the required type
        if (ValueConvertableToType(I, RetTy, CTMap)) {
          CTMap[I] = RetTy;
          return true;
        }
        // We have to return failure here because ValueConvertableToType could 
        // have polluted our map
        return false;
      }
    }
    // FALLTHROUGH
  case Instruction::Sub: {
    Value *OtherOp = I->getOperand((V == I->getOperand(0)) ? 1 : 0);
    return ValueConvertableToType(I, Ty, CTMap) &&
           ExpressionConvertableToType(OtherOp, Ty, CTMap);
  }
  case Instruction::SetEQ:
  case Instruction::SetNE: {
    Value *OtherOp = I->getOperand((V == I->getOperand(0)) ? 1 : 0);
    return ExpressionConvertableToType(OtherOp, Ty, CTMap);
  }
  case Instruction::Shr:
    if (Ty->isSigned() != V->getType()->isSigned()) return false;
    // FALL THROUGH
  case Instruction::Shl:
    assert(I->getOperand(0) == V);
    return ValueConvertableToType(I, Ty, CTMap);

  case Instruction::Free:
    assert(I->getOperand(0) == V);
    return isa<PointerType>(Ty);    // Free can free any pointer type!

  case Instruction::Load:
    // Cannot convert the types of any subscripts...
    if (I->getOperand(0) != V) return false;

    if (const PointerType *PT = dyn_cast<PointerType>(Ty)) {
      LoadInst *LI = cast<LoadInst>(I);
      
      if (LI->hasIndices() && !AllIndicesZero(LI))
        return false;

      const Type *LoadedTy = PT->getElementType();

      // They could be loading the first element of a composite type...
      if (const CompositeType *CT = dyn_cast<CompositeType>(LoadedTy)) {
        unsigned Offset = 0;     // No offset, get first leaf.
        std::vector<Value*> Indices;  // Discarded...
        LoadedTy = getStructOffsetType(CT, Offset, Indices, false);
        assert(Offset == 0 && "Offset changed from zero???");
      }

      if (!LoadedTy->isFirstClassType())
        return false;

      if (TD.getTypeSize(LoadedTy) != TD.getTypeSize(LI->getType()))
        return false;

      return ValueConvertableToType(LI, LoadedTy, CTMap);
    }
    return false;

  case Instruction::Store: {
    StoreInst *SI = cast<StoreInst>(I);
    if (SI->hasIndices()) return false;

    if (V == I->getOperand(0)) {
      ValueTypeCache::iterator CTMI = CTMap.find(I->getOperand(1));
      if (CTMI != CTMap.end()) {   // Operand #1 is in the table already?
        // If so, check to see if it's Ty*, or, more importantly, if it is a
        // pointer to a structure where the first element is a Ty... this code
        // is neccesary because we might be trying to change the source and
        // destination type of the store (they might be related) and the dest
        // pointer type might be a pointer to structure.  Below we allow pointer
        // to structures where the 0th element is compatible with the value,
        // now we have to support the symmetrical part of this.
        //
        const Type *ElTy = cast<PointerType>(CTMI->second)->getElementType();

        // Already a pointer to what we want?  Trivially accept...
        if (ElTy == Ty) return true;

        // Tricky case now, if the destination is a pointer to structure,
        // obviously the source is not allowed to be a structure (cannot copy
        // a whole structure at a time), so the level raiser must be trying to
        // store into the first field.  Check for this and allow it now:
        //
        if (StructType *SElTy = dyn_cast<StructType>(ElTy)) {
          unsigned Offset = 0;
          std::vector<Value*> Indices;
          ElTy = getStructOffsetType(ElTy, Offset, Indices, false);
          assert(Offset == 0 && "Offset changed!");
          if (ElTy == 0)    // Element at offset zero in struct doesn't exist!
            return false;   // Can only happen for {}*
          
          if (ElTy == Ty)   // Looks like the 0th element of structure is
            return true;    // compatible!  Accept now!

          // Otherwise we know that we can't work, so just stop trying now.
          return false;
        }
      }

      // Can convert the store if we can convert the pointer operand to match
      // the new  value type...
      return ExpressionConvertableToType(I->getOperand(1), PointerType::get(Ty),
                                         CTMap);
    } else if (const PointerType *PT = dyn_cast<PointerType>(Ty)) {
      const Type *ElTy = PT->getElementType();
      assert(V == I->getOperand(1));

      if (isa<StructType>(ElTy)) {
        // We can change the destination pointer if we can store our first
        // argument into the first element of the structure...
        //
        unsigned Offset = 0;
        std::vector<Value*> Indices;
        ElTy = getStructOffsetType(ElTy, Offset, Indices, false);
        assert(Offset == 0 && "Offset changed!");
        if (ElTy == 0)    // Element at offset zero in struct doesn't exist!
          return false;   // Can only happen for {}*
      }

      // Must move the same amount of data...
      if (TD.getTypeSize(ElTy) != TD.getTypeSize(I->getOperand(0)->getType()))
        return false;

      // Can convert store if the incoming value is convertable...
      return ExpressionConvertableToType(I->getOperand(0), ElTy, CTMap);
    }
    return false;
  }

  case Instruction::GetElementPtr:
    if (V != I->getOperand(0) || !isa<PointerType>(Ty)) return false;

    // If we have a two operand form of getelementptr, this is really little
    // more than a simple addition.  As with addition, check to see if the
    // getelementptr instruction can be changed to index into the new type.
    //
    if (I->getNumOperands() == 2) {
      const Type *OldElTy = cast<PointerType>(I->getType())->getElementType();
      unsigned DataSize = TD.getTypeSize(OldElTy);
      Value *Index = I->getOperand(1);
      Instruction *TempScale = 0;

      // If the old data element is not unit sized, we have to create a scale
      // instruction so that ConvertableToGEP will know the REAL amount we are
      // indexing by.  Note that this is never inserted into the instruction
      // stream, so we have to delete it when we're done.
      //
      if (DataSize != 1) {
        TempScale = BinaryOperator::create(Instruction::Mul, Index,
                                           ConstantUInt::get(Type::UIntTy,
                                                             DataSize));
        Index = TempScale;
      }

      // Check to see if the second argument is an expression that can
      // be converted to the appropriate size... if so, allow it.
      //
      std::vector<Value*> Indices;
      const Type *ElTy = ConvertableToGEP(Ty, Index, Indices);
      delete TempScale;   // Free our temporary multiply if we made it

      if (ElTy == 0) return false;  // Cannot make conversion...
      return ValueConvertableToType(I, PointerType::get(ElTy), CTMap);
    }
    return false;

  case Instruction::PHINode: {
    PHINode *PN = cast<PHINode>(I);
    for (unsigned i = 0; i < PN->getNumIncomingValues(); ++i)
      if (!ExpressionConvertableToType(PN->getIncomingValue(i), Ty, CTMap))
        return false;
    return ValueConvertableToType(PN, Ty, CTMap);
  }

  case Instruction::Call: {
    User::op_iterator OI = find(I->op_begin(), I->op_end(), V);
    assert (OI != I->op_end() && "Not using value!");
    unsigned OpNum = OI - I->op_begin();

    // Are we trying to change the method pointer value to a new type?
    if (OpNum == 0) {
      PointerType *PTy = dyn_cast<PointerType>(Ty);
      if (PTy == 0) return false;  // Can't convert to a non-pointer type...
      MethodType *MTy = dyn_cast<MethodType>(PTy->getElementType());
      if (MTy == 0) return false;  // Can't convert to a non ptr to method...

      // Perform sanity checks to make sure that new method type has the
      // correct number of arguments...
      //
      unsigned NumArgs = I->getNumOperands()-1;  // Don't include method ptr

      // Cannot convert to a type that requires more fixed arguments than
      // the call provides...
      //
      if (NumArgs < MTy->getParamTypes().size()) return false;
      
      // Unless this is a vararg method type, we cannot provide more arguments
      // than are desired...
      //
      if (!MTy->isVarArg() && NumArgs > MTy->getParamTypes().size())
        return false;

      // Okay, at this point, we know that the call and the method type match
      // number of arguments.  Now we see if we can convert the arguments
      // themselves.  Note that we do not require operands to be convertable,
      // we can insert casts if they are convertible but not compatible.  The
      // reason for this is that we prefer to have resolved methods but casted
      // arguments if possible.
      //
      const MethodType::ParamTypes &PTs = MTy->getParamTypes();
      for (unsigned i = 0, NA = PTs.size(); i < NA; ++i)
        if (!PTs[i]->isLosslesslyConvertableTo(I->getOperand(i+1)->getType()))
          return false;   // Operands must have compatible types!

      // Okay, at this point, we know that all of the arguments can be
      // converted.  We succeed if we can change the return type if
      // neccesary...
      //
      return ValueConvertableToType(I, MTy->getReturnType(), CTMap);
    }
    
    const PointerType *MPtr = cast<PointerType>(I->getOperand(0)->getType());
    const MethodType *MTy = cast<MethodType>(MPtr->getElementType());
    if (!MTy->isVarArg()) return false;

    if ((OpNum-1) < MTy->getParamTypes().size())
      return false;  // It's not in the varargs section...

    // If we get this far, we know the value is in the varargs section of the
    // method!  We can convert if we don't reinterpret the value...
    //
    return Ty->isLosslesslyConvertableTo(V->getType());
  }
  }
  return false;
}


void ConvertValueToNewType(Value *V, Value *NewVal, ValueMapCache &VMC) {
  ValueHandle VH(VMC, V);

  unsigned NumUses = V->use_size();
  for (unsigned It = 0; It < NumUses; ) {
    unsigned OldSize = NumUses;
    ConvertOperandToType(*(V->use_begin()+It), V, NewVal, VMC);
    NumUses = V->use_size();
    if (NumUses == OldSize) ++It;
  }
}



static void ConvertOperandToType(User *U, Value *OldVal, Value *NewVal,
                                 ValueMapCache &VMC) {
  if (isa<ValueHandle>(U)) return;  // Valuehandles don't let go of operands...

  if (VMC.OperandsMapped.count(U)) return;
  VMC.OperandsMapped.insert(U);

  ValueMapCache::ExprMapTy::iterator VMCI = VMC.ExprMap.find(U);
  if (VMCI != VMC.ExprMap.end())
    return;


  Instruction *I = cast<Instruction>(U);  // Only Instructions convertable

  BasicBlock *BB = I->getParent();
  BasicBlock::InstListType &BIL = BB->getInstList();
  std::string Name = I->getName();  if (!Name.empty()) I->setName("");
  Instruction *Res;     // Result of conversion

  //cerr << endl << endl << "Type:\t" << Ty << "\nInst: " << I << "BB Before: " << BB << endl;

  // Prevent I from being removed...
  ValueHandle IHandle(VMC, I);

  const Type *NewTy = NewVal->getType();
  Constant *Dummy = (NewTy != Type::VoidTy) ? 
                  Constant::getNullConstant(NewTy) : 0;

  switch (I->getOpcode()) {
  case Instruction::Cast:
    assert(I->getOperand(0) == OldVal);
    Res = new CastInst(NewVal, I->getType(), Name);
    break;

  case Instruction::Add:
    if (isa<PointerType>(NewTy)) {
      Value *IndexVal = I->getOperand(OldVal == I->getOperand(0) ? 1 : 0);
      std::vector<Value*> Indices;
      BasicBlock::iterator It = find(BIL.begin(), BIL.end(), I);

      if (const Type *ETy = ConvertableToGEP(NewTy, IndexVal, Indices, &It)) {
        // If successful, convert the add to a GEP
        //const Type *RetTy = PointerType::get(ETy);
        // First operand is actually the given pointer...
        Res = new GetElementPtrInst(NewVal, Indices, Name);
        assert(cast<PointerType>(Res->getType())->getElementType() == ETy &&
               "ConvertableToGEP broken!");
        break;
      }
    }
    // FALLTHROUGH

  case Instruction::Sub:
  case Instruction::SetEQ:
  case Instruction::SetNE: {
    Res = BinaryOperator::create(cast<BinaryOperator>(I)->getOpcode(),
                                 Dummy, Dummy, Name);
    VMC.ExprMap[I] = Res;   // Add node to expression eagerly

    unsigned OtherIdx = (OldVal == I->getOperand(0)) ? 1 : 0;
    Value *OtherOp    = I->getOperand(OtherIdx);
    Value *NewOther   = ConvertExpressionToType(OtherOp, NewTy, VMC);

    Res->setOperand(OtherIdx, NewOther);
    Res->setOperand(!OtherIdx, NewVal);
    break;
  }
  case Instruction::Shl:
  case Instruction::Shr:
    assert(I->getOperand(0) == OldVal);
    Res = new ShiftInst(cast<ShiftInst>(I)->getOpcode(), NewVal,
                        I->getOperand(1), Name);
    break;

  case Instruction::Free:            // Free can free any pointer type!
    assert(I->getOperand(0) == OldVal);
    Res = new FreeInst(NewVal);
    break;


  case Instruction::Load: {
    assert(I->getOperand(0) == OldVal && isa<PointerType>(NewVal->getType()));
    const Type *LoadedTy =
      cast<PointerType>(NewVal->getType())->getElementType();

    std::vector<Value*> Indices;
    Indices.push_back(ConstantUInt::get(Type::UIntTy, 0));

    if (const CompositeType *CT = dyn_cast<CompositeType>(LoadedTy)) {
      unsigned Offset = 0;   // No offset, get first leaf.
      LoadedTy = getStructOffsetType(CT, Offset, Indices, false);
    }
    assert(LoadedTy->isFirstClassType());

    Res = new LoadInst(NewVal, Indices, Name);
    assert(Res->getType()->isFirstClassType() && "Load of structure or array!");
    break;
  }

  case Instruction::Store: {
    if (I->getOperand(0) == OldVal) {  // Replace the source value
      const PointerType *NewPT = PointerType::get(NewTy);
      Res = new StoreInst(NewVal, Constant::getNullConstant(NewPT));
      VMC.ExprMap[I] = Res;
      Res->setOperand(1, ConvertExpressionToType(I->getOperand(1), NewPT, VMC));
    } else {                           // Replace the source pointer
      const Type *ValTy = cast<PointerType>(NewTy)->getElementType();
      std::vector<Value*> Indices;

      if (isa<StructType>(ValTy)) {
        unsigned Offset = 0;
        Indices.push_back(ConstantUInt::get(Type::UIntTy, 0));
        ValTy = getStructOffsetType(ValTy, Offset, Indices, false);
        assert(Offset == 0 && ValTy);
      }

      Res = new StoreInst(Constant::getNullConstant(ValTy), NewVal, Indices);
      VMC.ExprMap[I] = Res;
      Res->setOperand(0, ConvertExpressionToType(I->getOperand(0), ValTy, VMC));
    }
    break;
  }


  case Instruction::GetElementPtr: {
    // Convert a one index getelementptr into just about anything that is
    // desired.
    //
    BasicBlock::iterator It = find(BIL.begin(), BIL.end(), I);
    const Type *OldElTy = cast<PointerType>(I->getType())->getElementType();
    unsigned DataSize = TD.getTypeSize(OldElTy);
    Value *Index = I->getOperand(1);

    if (DataSize != 1) {
      // Insert a multiply of the old element type is not a unit size...
      Index = BinaryOperator::create(Instruction::Mul, Index,
                                     ConstantUInt::get(Type::UIntTy, DataSize));
      It = BIL.insert(It, cast<Instruction>(Index))+1;
    }

    // Perform the conversion now...
    //
    std::vector<Value*> Indices;
    const Type *ElTy = ConvertableToGEP(NewVal->getType(), Index, Indices, &It);
    assert(ElTy != 0 && "GEP Conversion Failure!");
    Res = new GetElementPtrInst(NewVal, Indices, Name);
    assert(Res->getType() == PointerType::get(ElTy) &&
           "ConvertableToGet failed!");
  }
#if 0
    if (I->getType() == PointerType::get(Type::SByteTy)) {
      // Convert a getelementptr sbyte * %reg111, uint 16 freely back to
      // anything that is a pointer type...
      //
      BasicBlock::iterator It = find(BIL.begin(), BIL.end(), I);
    
      // Check to see if the second argument is an expression that can
      // be converted to the appropriate size... if so, allow it.
      //
      std::vector<Value*> Indices;
      const Type *ElTy = ConvertableToGEP(NewVal->getType(), I->getOperand(1),
                                          Indices, &It);
      assert(ElTy != 0 && "GEP Conversion Failure!");
      
      Res = new GetElementPtrInst(NewVal, Indices, Name);
    } else {
      // Convert a getelementptr ulong * %reg123, uint %N
      // to        getelementptr  long * %reg123, uint %N
      // ... where the type must simply stay the same size...
      //
      Res = new GetElementPtrInst(NewVal,
                                  cast<GetElementPtrInst>(I)->copyIndices(),
                                  Name);
    }
#endif
    break;

  case Instruction::PHINode: {
    PHINode *OldPN = cast<PHINode>(I);
    PHINode *NewPN = new PHINode(NewTy, Name);
    VMC.ExprMap[I] = NewPN;

    while (OldPN->getNumOperands()) {
      BasicBlock *BB = OldPN->getIncomingBlock(0);
      Value *OldVal = OldPN->getIncomingValue(0);
      OldPN->removeIncomingValue(BB);
      Value *V = ConvertExpressionToType(OldVal, NewTy, VMC);
      NewPN->addIncoming(V, BB);
    }
    Res = NewPN;
    break;
  }

  case Instruction::Call: {
    Value *Meth = I->getOperand(0);
    std::vector<Value*> Params(I->op_begin()+1, I->op_end());

    if (Meth == OldVal) {   // Changing the method pointer?
      PointerType *NewPTy = cast<PointerType>(NewVal->getType());
      MethodType *NewTy = cast<MethodType>(NewPTy->getElementType());
      const MethodType::ParamTypes &PTs = NewTy->getParamTypes();

      // Get an iterator to the call instruction so that we can insert casts for
      // operands if needbe.  Note that we do not require operands to be
      // convertable, we can insert casts if they are convertible but not
      // compatible.  The reason for this is that we prefer to have resolved
      // methods but casted arguments if possible.
      //
      BasicBlock::iterator It = find(BIL.begin(), BIL.end(), I);

      // Convert over all of the call operands to their new types... but only
      // convert over the part that is not in the vararg section of the call.
      //
      for (unsigned i = 0; i < PTs.size(); ++i)
        if (Params[i]->getType() != PTs[i]) {
          // Create a cast to convert it to the right type, we know that this
          // is a lossless cast...
          //
          Params[i] = new CastInst(Params[i], PTs[i], "call.resolve.cast");
          It = BIL.insert(It, cast<Instruction>(Params[i]))+1;
        }
      Meth = NewVal;  // Update call destination to new value

    } else {                   // Changing an argument, must be in vararg area
      std::vector<Value*>::iterator OI =
        find(Params.begin(), Params.end(), OldVal);
      assert (OI != Params.end() && "Not using value!");

      *OI = NewVal;
    }

    Res = new CallInst(Meth, Params, Name);
    break;
  }
  default:
    assert(0 && "Expression convertable, but don't know how to convert?");
    return;
  }

  // If the instruction was newly created, insert it into the instruction
  // stream.
  //
  BasicBlock::iterator It = find(BIL.begin(), BIL.end(), I);
  assert(It != BIL.end() && "Instruction not in own basic block??");
  BIL.insert(It, Res);   // Keep It pointing to old instruction

#ifdef DEBUG_EXPR_CONVERT
  cerr << "COT CREATED: "  << (void*)Res << " " << Res;
  cerr << "In: " << (void*)I << " " << I << "Out: " << (void*)Res << " " << Res;
#endif

  // Add the instruction to the expression map
  VMC.ExprMap[I] = Res;

  if (I->getType() != Res->getType())
    ConvertValueToNewType(I, Res, VMC);
  else {
    for (unsigned It = 0; It < I->use_size(); ) {
      User *Use = *(I->use_begin()+It);
      if (isa<ValueHandle>(Use))            // Don't remove ValueHandles!
        ++It;
      else
        Use->replaceUsesOfWith(I, Res);
    }

    if (I->use_empty()) {
      // Now we just need to remove the old instruction so we don't get infinite
      // loops.  Note that we cannot use DCE because DCE won't remove a store
      // instruction, for example.
      //
#ifdef DEBUG_EXPR_CONVERT
      cerr << "DELETING: " << (void*)I << " " << I;
#endif
      BIL.remove(I);
      VMC.OperandsMapped.erase(I);
      VMC.ExprMap.erase(I);
      delete I;
    } else {
      for (Value::use_iterator UI = I->use_begin(), UE = I->use_end();
           UI != UE; ++UI)
        assert(isa<ValueHandle>((Value*)*UI) &&"Uses of Instruction remain!!!");
    }
  }
}


ValueHandle::ValueHandle(ValueMapCache &VMC, Value *V)
  : Instruction(Type::VoidTy, UserOp1, ""), Cache(VMC) {
#ifdef DEBUG_EXPR_CONVERT
  //cerr << "VH AQUIRING: " << (void*)V << " " << V;
#endif
  Operands.push_back(Use(V, this));
}

static void RecursiveDelete(ValueMapCache &Cache, Instruction *I) {
  if (!I || !I->use_empty()) return;

  assert(I->getParent() && "Inst not in basic block!");

#ifdef DEBUG_EXPR_CONVERT
  //cerr << "VH DELETING: " << (void*)I << " " << I;
#endif

  for (User::op_iterator OI = I->op_begin(), OE = I->op_end(); 
       OI != OE; ++OI)
    if (Instruction *U = dyn_cast<Instruction>(*OI)) {
      *OI = 0;
      RecursiveDelete(Cache, U);
    }

  I->getParent()->getInstList().remove(I);

  Cache.OperandsMapped.erase(I);
  Cache.ExprMap.erase(I);
  delete I;
}

ValueHandle::~ValueHandle() {
  if (Operands[0]->use_size() == 1) {
    Value *V = Operands[0];
    Operands[0] = 0;   // Drop use!

    // Now we just need to remove the old instruction so we don't get infinite
    // loops.  Note that we cannot use DCE because DCE won't remove a store
    // instruction, for example.
    //
    RecursiveDelete(Cache, dyn_cast<Instruction>(V));
  } else {
#ifdef DEBUG_EXPR_CONVERT
    //cerr << "VH RELEASING: " << (void*)Operands[0].get() << " " << Operands[0]->use_size() << " " << Operands[0];
#endif
  }
}
