//===-- Constants.cpp - Implement Constant nodes -----------------*- C++ -*--=//
//
// This file implements the Constant* classes...
//
//===----------------------------------------------------------------------===//

#define __STDC_LIMIT_MACROS           // Get defs for INT64_MAX and friends...
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/iMemory.h"
#include "llvm/SymbolTable.h"
#include "llvm/Module.h"
#include "llvm/SlotCalculator.h"
#include "Support/StringExtras.h"
#include <algorithm>

using std::map;
using std::pair;
using std::make_pair;
using std::vector;
using std::cerr;
using std::endl;

ConstantBool *ConstantBool::True  = new ConstantBool(true);
ConstantBool *ConstantBool::False = new ConstantBool(false);


//===----------------------------------------------------------------------===//
//                              Constant Class
//===----------------------------------------------------------------------===//

// Specialize setName to take care of symbol table majik
void Constant::setName(const std::string &Name, SymbolTable *ST) {
  assert(ST && "Type::setName - Must provide symbol table argument!");

  if (Name.size()) ST->insert(Name, this);
}

// Static constructor to create a '0' constant of arbitrary type...
Constant *Constant::getNullValue(const Type *Ty) {
  switch (Ty->getPrimitiveID()) {
  case Type::BoolTyID:   return ConstantBool::get(false);
  case Type::SByteTyID:
  case Type::ShortTyID:
  case Type::IntTyID:
  case Type::LongTyID:   return ConstantSInt::get(Ty, 0);

  case Type::UByteTyID:
  case Type::UShortTyID:
  case Type::UIntTyID:
  case Type::ULongTyID:  return ConstantUInt::get(Ty, 0);

  case Type::FloatTyID:
  case Type::DoubleTyID: return ConstantFP::get(Ty, 0);

  case Type::PointerTyID: 
    return ConstantPointerNull::get(cast<PointerType>(Ty));
  default:
    return 0;
  }
}

void Constant::destroyConstantImpl() {
  // When a Constant is destroyed, there may be lingering
  // references to the constant by other constants in the constant pool.  These
  // constants are implicitly dependant on the module that is being deleted,
  // but they don't know that.  Because we only find out when the CPV is
  // deleted, we must now notify all of our users (that should only be
  // Constants) that they are, in fact, invalid now and should be deleted.
  //
  while (!use_empty()) {
    Value *V = use_back();
#ifndef NDEBUG      // Only in -g mode...
    if (!isa<Constant>(V)) {
      std::cerr << "While deleting: ";
      dump();
      std::cerr << "\nUse still stuck around after Def is destroyed: ";
      V->dump();
      std::cerr << "\n";
    }
#endif
    assert(isa<Constant>(V) && "References remain to Constant being destroyed");
    Constant *CPV = cast<Constant>(V);
    CPV->destroyConstant();

    // The constant should remove itself from our use list...
    assert((use_empty() || use_back() != V) && "Constant not removed!");
  }

  // Value has no outstanding references it is safe to delete it now...
  delete this;
}

//===----------------------------------------------------------------------===//
//                            ConstantXXX Classes
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
//                             Normal Constructors

ConstantBool::ConstantBool(bool V) : Constant(Type::BoolTy) {
  Val = V;
}

ConstantInt::ConstantInt(const Type *Ty, uint64_t V) : Constant(Ty) {
  Val.Unsigned = V;
}

ConstantSInt::ConstantSInt(const Type *Ty, int64_t V) : ConstantInt(Ty, V) {
  assert(isValueValidForType(Ty, V) && "Value too large for type!");
}

ConstantUInt::ConstantUInt(const Type *Ty, uint64_t V) : ConstantInt(Ty, V) {
  assert(isValueValidForType(Ty, V) && "Value too large for type!");
}

ConstantFP::ConstantFP(const Type *Ty, double V) : Constant(Ty) {
  assert(isValueValidForType(Ty, V) && "Value too large for type!");
  Val = V;
}

ConstantArray::ConstantArray(const ArrayType *T,
                             const std::vector<Constant*> &V) : Constant(T) {
  for (unsigned i = 0; i < V.size(); i++) {
    assert(V[i]->getType() == T->getElementType());
    Operands.push_back(Use(V[i], this));
  }
}

ConstantStruct::ConstantStruct(const StructType *T,
                               const std::vector<Constant*> &V) : Constant(T) {
  const StructType::ElementTypes &ETypes = T->getElementTypes();
  assert(V.size() == ETypes.size() &&
         "Invalid initializer vector for constant structure");
  for (unsigned i = 0; i < V.size(); i++) {
    assert(V[i]->getType() == ETypes[i]);
    Operands.push_back(Use(V[i], this));
  }
}

ConstantPointerRef::ConstantPointerRef(GlobalValue *GV)
  : ConstantPointer(GV->getType()) {
  Operands.push_back(Use(GV, this));
}

ConstantExpr::ConstantExpr(unsigned opCode, Constant *C,  const Type *Ty)
  : Constant(Ty), iType(opCode) {
  Operands.push_back(Use(C, this));
}

ConstantExpr::ConstantExpr(unsigned opCode, Constant* C1,
                           Constant* C2, const Type *Ty)
  : Constant(Ty), iType(opCode) {
  Operands.push_back(Use(C1, this));
  Operands.push_back(Use(C2, this));
}

ConstantExpr::ConstantExpr(unsigned opCode, Constant* C,
                          const std::vector<Value*>& IdxList, const Type *Ty)
  : Constant(Ty), iType(opCode) {
  Operands.reserve(1+IdxList.size());
  Operands.push_back(Use(C, this));
  for (unsigned i = 0, E = IdxList.size(); i != E; ++i)
    Operands.push_back(Use(IdxList[i], this));
}



//===----------------------------------------------------------------------===//
//                           classof implementations

bool ConstantInt::classof(const Constant *CPV) {
  return CPV->getType()->isIntegral() && ! isa<ConstantExpr>(CPV);
}
bool ConstantSInt::classof(const Constant *CPV) {
  return CPV->getType()->isSigned() && ! isa<ConstantExpr>(CPV);
}
bool ConstantUInt::classof(const Constant *CPV) {
  return CPV->getType()->isUnsigned() && ! isa<ConstantExpr>(CPV);
}
bool ConstantFP::classof(const Constant *CPV) {
  const Type *Ty = CPV->getType();
  return ((Ty == Type::FloatTy || Ty == Type::DoubleTy) &&
          ! isa<ConstantExpr>(CPV));
}
bool ConstantArray::classof(const Constant *CPV) {
  return isa<ArrayType>(CPV->getType()) && ! isa<ConstantExpr>(CPV);
}
bool ConstantStruct::classof(const Constant *CPV) {
  return isa<StructType>(CPV->getType()) && ! isa<ConstantExpr>(CPV);
}
bool ConstantPointer::classof(const Constant *CPV) {
  return (isa<PointerType>(CPV->getType()) && ! isa<ConstantExpr>(CPV));
}



//===----------------------------------------------------------------------===//
//                      isValueValidForType implementations

bool ConstantSInt::isValueValidForType(const Type *Ty, int64_t Val) {
  switch (Ty->getPrimitiveID()) {
  default:
    return false;         // These can't be represented as integers!!!

    // Signed types...
  case Type::SByteTyID:
    return (Val <= INT8_MAX && Val >= INT8_MIN);
  case Type::ShortTyID:
    return (Val <= INT16_MAX && Val >= INT16_MIN);
  case Type::IntTyID:
    return (Val <= INT32_MAX && Val >= INT32_MIN);
  case Type::LongTyID:
    return true;          // This is the largest type...
  }
  assert(0 && "WTF?");
  return false;
}

bool ConstantUInt::isValueValidForType(const Type *Ty, uint64_t Val) {
  switch (Ty->getPrimitiveID()) {
  default:
    return false;         // These can't be represented as integers!!!

    // Unsigned types...
  case Type::UByteTyID:
    return (Val <= UINT8_MAX);
  case Type::UShortTyID:
    return (Val <= UINT16_MAX);
  case Type::UIntTyID:
    return (Val <= UINT32_MAX);
  case Type::ULongTyID:
    return true;          // This is the largest type...
  }
  assert(0 && "WTF?");
  return false;
}

bool ConstantFP::isValueValidForType(const Type *Ty, double Val) {
  switch (Ty->getPrimitiveID()) {
  default:
    return false;         // These can't be represented as floating point!

    // TODO: Figure out how to test if a double can be cast to a float!
  case Type::FloatTyID:
    /*
    return (Val <= UINT8_MAX);
    */
  case Type::DoubleTyID:
    return true;          // This is the largest type...
  }
};

//===----------------------------------------------------------------------===//
//                      Factory Function Implementation

template<class ValType, class ConstantClass>
struct ValueMap {
  typedef pair<const Type*, ValType> ConstHashKey;
  map<ConstHashKey, ConstantClass *> Map;

  inline ConstantClass *get(const Type *Ty, ValType V) {
    map<ConstHashKey,ConstantClass *>::iterator I =
      Map.find(ConstHashKey(Ty, V));
    return (I != Map.end()) ? I->second : 0;
  }

  inline void add(const Type *Ty, ValType V, ConstantClass *CP) {
    Map.insert(make_pair(ConstHashKey(Ty, V), CP));
  }

  inline void remove(ConstantClass *CP) {
    for (map<ConstHashKey,ConstantClass *>::iterator I = Map.begin(),
                                                      E = Map.end(); I != E;++I)
      if (I->second == CP) {
	Map.erase(I);
	return;
      }
  }
};

//---- ConstantUInt::get() and ConstantSInt::get() implementations...
//
static ValueMap<uint64_t, ConstantInt> IntConstants;

ConstantSInt *ConstantSInt::get(const Type *Ty, int64_t V) {
  ConstantSInt *Result = (ConstantSInt*)IntConstants.get(Ty, (uint64_t)V);
  if (!Result)   // If no preexisting value, create one now...
    IntConstants.add(Ty, V, Result = new ConstantSInt(Ty, V));
  return Result;
}

ConstantUInt *ConstantUInt::get(const Type *Ty, uint64_t V) {
  ConstantUInt *Result = (ConstantUInt*)IntConstants.get(Ty, V);
  if (!Result)   // If no preexisting value, create one now...
    IntConstants.add(Ty, V, Result = new ConstantUInt(Ty, V));
  return Result;
}

ConstantInt *ConstantInt::get(const Type *Ty, unsigned char V) {
  assert(V <= 127 && "Can only be used with very small positive constants!");
  if (Ty->isSigned()) return ConstantSInt::get(Ty, V);
  return ConstantUInt::get(Ty, V);
}

//---- ConstantFP::get() implementation...
//
static ValueMap<double, ConstantFP> FPConstants;

ConstantFP *ConstantFP::get(const Type *Ty, double V) {
  ConstantFP *Result = FPConstants.get(Ty, V);
  if (!Result)   // If no preexisting value, create one now...
    FPConstants.add(Ty, V, Result = new ConstantFP(Ty, V));
  return Result;
}

//---- ConstantArray::get() implementation...
//
static ValueMap<std::vector<Constant*>, ConstantArray> ArrayConstants;

ConstantArray *ConstantArray::get(const ArrayType *Ty,
                                  const std::vector<Constant*> &V) {
  ConstantArray *Result = ArrayConstants.get(Ty, V);
  if (!Result)   // If no preexisting value, create one now...
    ArrayConstants.add(Ty, V, Result = new ConstantArray(Ty, V));
  return Result;
}

// ConstantArray::get(const string&) - Return an array that is initialized to
// contain the specified string.  A null terminator is added to the specified
// string so that it may be used in a natural way...
//
ConstantArray *ConstantArray::get(const std::string &Str) {
  std::vector<Constant*> ElementVals;

  for (unsigned i = 0; i < Str.length(); ++i)
    ElementVals.push_back(ConstantSInt::get(Type::SByteTy, Str[i]));

  // Add a null terminator to the string...
  ElementVals.push_back(ConstantSInt::get(Type::SByteTy, 0));

  ArrayType *ATy = ArrayType::get(Type::SByteTy, Str.length()+1);
  return ConstantArray::get(ATy, ElementVals);
}


// destroyConstant - Remove the constant from the constant table...
//
void ConstantArray::destroyConstant() {
  ArrayConstants.remove(this);
  destroyConstantImpl();
}

//---- ConstantStruct::get() implementation...
//
static ValueMap<std::vector<Constant*>, ConstantStruct> StructConstants;

ConstantStruct *ConstantStruct::get(const StructType *Ty,
                                    const std::vector<Constant*> &V) {
  ConstantStruct *Result = StructConstants.get(Ty, V);
  if (!Result)   // If no preexisting value, create one now...
    StructConstants.add(Ty, V, Result = new ConstantStruct(Ty, V));
  return Result;
}

// destroyConstant - Remove the constant from the constant table...
//
void ConstantStruct::destroyConstant() {
  StructConstants.remove(this);
  destroyConstantImpl();
}

//---- ConstantPointerNull::get() implementation...
//
static ValueMap<char, ConstantPointerNull> NullPtrConstants;

ConstantPointerNull *ConstantPointerNull::get(const PointerType *Ty) {
  ConstantPointerNull *Result = NullPtrConstants.get(Ty, 0);
  if (!Result)   // If no preexisting value, create one now...
    NullPtrConstants.add(Ty, 0, Result = new ConstantPointerNull(Ty));
  return Result;
}

//---- ConstantPointerRef::get() implementation...
//
ConstantPointerRef *ConstantPointerRef::get(GlobalValue *GV) {
  assert(GV->getParent() && "Global Value must be attached to a module!");
  
  // The Module handles the pointer reference sharing...
  return GV->getParent()->getConstantPointerRef(GV);
}

//---- ConstantExpr::get() implementations...
// Return NULL on invalid expressions.
//
typedef pair<unsigned, vector<Constant*> > ExprMapKeyType;
static ValueMap<const ExprMapKeyType, ConstantExpr> ExprConstants;

ConstantExpr*
ConstantExpr::get(unsigned opCode, Constant *C, const Type *Ty) {

  // Look up the constant in the table first to ensure uniqueness
  vector<Constant*> argVec(1, C);
  const ExprMapKeyType& key = make_pair(opCode, argVec);
  ConstantExpr* result = ExprConstants.get(Ty, key);
  if (result)
    return result;
  
  // Its not in the table so create a new one and put it in the table.
  // Check the operands for consistency first
  if (opCode != Instruction::Cast &&
      (opCode < Instruction::FirstUnaryOp ||
       opCode >= Instruction::NumUnaryOps)) {
    std::cerr << "Invalid opcode " << ConstantExpr::getOpcodeName(opCode)
         << " in unary constant expression" << std::endl;
    return NULL;       // Not Cast or other unary opcode
  }
  // type of operand will not match result for Cast operation
  if (opCode != Instruction::Cast && Ty != C->getType()) {
    cerr << "Type of operand in unary constant expression should match result" << endl;
    return NULL;
  }
  
  result = new ConstantExpr(opCode, C, Ty);
  ExprConstants.add(Ty, key, result);
  return result;
}

ConstantExpr*
ConstantExpr::get(unsigned opCode, Constant *C1, Constant *C2,const Type *Ty) {

  // Look up the constant in the table first to ensure uniqueness
  vector<Constant*> argVec(1, C1); argVec.push_back(C2);
  const ExprMapKeyType& key = make_pair(opCode, argVec);
  ConstantExpr* result = ExprConstants.get(Ty, key);
  if (result)
    return result;
  
  // Its not in the table so create a new one and put it in the table.
  // Check the operands for consistency first
  if (opCode < Instruction::FirstBinaryOp ||
      opCode >= Instruction::NumBinaryOps) {
    cerr << "Invalid opcode " << ConstantExpr::getOpcodeName(opCode)
         << " in binary constant expression" << endl;
    return NULL;
  }
  if (Ty != C1->getType() || Ty != C2->getType()) {
    cerr << "Types of both operands in binary constant expression should match result" << endl;
    return NULL;
  }
  
  result = new ConstantExpr(opCode, C1, C2, Ty);
  ExprConstants.add(Ty, key, result);
  return result;
}

ConstantExpr*
ConstantExpr::get(unsigned opCode, Constant*C,
                  const std::vector<Value*>& idxList, const Type *Ty) {

  // Look up the constant in the table first to ensure uniqueness
  vector<Constant*> argVec(1, C);
  for(vector<Value*>::const_iterator VI=idxList.begin(), VE=idxList.end();
      VI != VE; ++VI)
    if (Constant *C = dyn_cast<Constant>(*VI))
        argVec.push_back(C);
    else {
      cerr << "Non-constant index in constant GetElementPtr expr";
      return NULL;
    }
  
  const ExprMapKeyType& key = make_pair(opCode, argVec);
  ConstantExpr* result = ExprConstants.get(Ty, key);
  if (result)
    return result;
  
  // Its not in the table so create a new one and put it in the table.
  // Check the operands for consistency first
  // Must be a getElementPtr.  Check for valid getElementPtr expression.
  // 
  if (opCode != Instruction::GetElementPtr) {
    cerr << "operator other than GetElementPtr used with an index list" << endl;
    return NULL;
  }
  if (!isa<ConstantPointer>(C)) {
    cerr << "Constant GelElementPtr expression using something other than a constant pointer" << endl;
    return NULL;
  }
  if (!isa<PointerType>(Ty)) {
    cerr << "Non-pointer type for constant GelElementPtr expression" << endl;
    return NULL;
  }
  const Type* fldType = GetElementPtrInst::getIndexedType(C->getType(),
                                                          idxList, true);
  if (!fldType) {
    cerr << "Invalid index list for constant GelElementPtr expression" << endl;
    return NULL;
  }
  if (cast<PointerType>(Ty)->getElementType() != fldType) {
    cerr << "Type for constant GelElementPtr expression does not match field type" << endl;
    return NULL;
  }
  
  result = new ConstantExpr(opCode, C, idxList, Ty);
  ExprConstants.add(Ty, key, result);
  return result;
}

// destroyConstant - Remove the constant from the constant table...
//
void ConstantExpr::destroyConstant() {
  ExprConstants.remove(this);
  destroyConstantImpl();
}

const char*
ConstantExpr::getOpcodeName(unsigned opCode) {
  return Instruction::getOpcodeName(opCode);
}


//---- ConstantPointerRef::mutateReferences() implementation...
//
unsigned
ConstantPointerRef::mutateReferences(Value* OldV, Value *NewV) {
  assert(getValue() == OldV && "Cannot mutate old value if I'm not using it!");
  GlobalValue* NewGV = cast<GlobalValue>(NewV);
  getValue()->getParent()->mutateConstantPointerRef(getValue(), NewGV);
  Operands[0] = NewGV;
  return 1;
}


//---- ConstantPointerExpr::mutateReferences() implementation...
//
unsigned
ConstantExpr::mutateReferences(Value* OldV, Value *NewV) {
  unsigned numReplaced = 0;
  Constant* NewC = cast<Constant>(NewV);
  for (unsigned i=0, N = getNumOperands(); i < N; ++i)
    if (Operands[i] == OldV) {
      ++numReplaced;
      Operands[i] = NewC;
    }
  return numReplaced;
}
