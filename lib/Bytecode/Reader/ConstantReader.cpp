//===- ReadConst.cpp - Code to constants and constant pools ---------------===//
//
// This file implements functionality to deserialize constants and entire 
// constant pools.
// 
// Note that this library should be as fast as possible, reentrant, and 
// threadsafe!!
//
//===----------------------------------------------------------------------===//

#include "ReaderInternals.h"
#include "llvm/Module.h"
#include "llvm/BasicBlock.h"
#include "llvm/ConstantVals.h"
#include "llvm/GlobalVariable.h"
#include <algorithm>



const Type *BytecodeParser::parseTypeConstant(const uchar *&Buf,
					      const uchar *EndBuf) {
  unsigned PrimType;
  if (read_vbr(Buf, EndBuf, PrimType)) return failure<const Type*>(0);

  const Type *Val = 0;
  if ((Val = Type::getPrimitiveType((Type::PrimitiveID)PrimType)))
    return Val;
  
  switch (PrimType) {
  case Type::MethodTyID: {
    unsigned Typ;
    if (read_vbr(Buf, EndBuf, Typ)) return failure(Val);
    const Type *RetType = getType(Typ);
    if (RetType == 0) return failure(Val);

    unsigned NumParams;
    if (read_vbr(Buf, EndBuf, NumParams)) return failure(Val);

    vector<const Type*> Params;
    while (NumParams--) {
      if (read_vbr(Buf, EndBuf, Typ)) return failure(Val);
      const Type *Ty = getType(Typ);
      if (Ty == 0) return failure(Val);
      Params.push_back(Ty);
    }

    bool isVarArg = Params.size() && Params.back() == Type::VoidTy;
    if (isVarArg) Params.pop_back();

    return MethodType::get(RetType, Params, isVarArg);
  }
  case Type::ArrayTyID: {
    unsigned ElTyp;
    if (read_vbr(Buf, EndBuf, ElTyp)) return failure(Val);
    const Type *ElementType = getType(ElTyp);
    if (ElementType == 0) return failure(Val);

    int NumElements;
    if (read_vbr(Buf, EndBuf, NumElements)) return failure(Val);

    BCR_TRACE(5, "Array Type Constant #" << ElTyp << " size=" 
              << NumElements << endl);
    return ArrayType::get(ElementType, NumElements);
  }
  case Type::StructTyID: {
    unsigned Typ;
    vector<const Type*> Elements;

    if (read_vbr(Buf, EndBuf, Typ)) return failure(Val);
    while (Typ) {         // List is terminated by void/0 typeid
      const Type *Ty = getType(Typ);
      if (Ty == 0) return failure(Val);
      Elements.push_back(Ty);
      
      if (read_vbr(Buf, EndBuf, Typ)) return failure(Val);
    }

    return StructType::get(Elements);
  }
  case Type::PointerTyID: {
    unsigned ElTyp;
    if (read_vbr(Buf, EndBuf, ElTyp)) return failure(Val);
    BCR_TRACE(5, "Pointer Type Constant #" << (ElTyp-14) << endl);
    const Type *ElementType = getType(ElTyp);
    if (ElementType == 0) return failure(Val);
    return PointerType::get(ElementType);
  }

  case Type::OpaqueTyID: {
    return OpaqueType::get();
  }

  default:
    cerr << __FILE__ << ":" << __LINE__ << ": Don't know how to deserialize"
         << " primitive Type " << PrimType << "\n";
    return failure(Val);
  }
}

// refineAbstractType - The callback method is invoked when one of the
// elements of TypeValues becomes more concrete...
//
void BytecodeParser::refineAbstractType(const DerivedType *OldType, 
					const Type *NewType) {
  if (OldType == NewType &&
      OldType->isAbstract()) return;  // Type is modified, but same

  TypeValuesListTy::iterator I = find(MethodTypeValues.begin(), 
				      MethodTypeValues.end(), OldType);
  if (I == MethodTypeValues.end()) {
    I = find(ModuleTypeValues.begin(), ModuleTypeValues.end(), OldType);
    assert(I != ModuleTypeValues.end() && 
	   "Can't refine a type I don't know about!");
  }

  if (OldType == NewType) {
    assert(!OldType->isAbstract());
    I->removeUserFromConcrete();
  } else {
    *I = NewType;  // Update to point to new, more refined type.
  }
}



// parseTypeConstants - We have to use this wierd code to handle recursive
// types.  We know that recursive types will only reference the current slab of
// values in the type plane, but they can forward reference types before they
// have been read.  For example, Type #0 might be '{ Ty#1 }' and Type #1 might
// be 'Ty#0*'.  When reading Type #0, type number one doesn't exist.  To fix
// this ugly problem, we pesimistically insert an opaque type for each type we
// are about to read.  This means that forward references will resolve to
// something and when we reread the type later, we can replace the opaque type
// with a new resolved concrete type.
//
bool BytecodeParser::parseTypeConstants(const uchar *&Buf, const uchar *EndBuf,
					TypeValuesListTy &Tab,
					unsigned NumEntries) {
  assert(Tab.size() == 0 && "should not have read type constants in before!");

  // Insert a bunch of opaque types to be resolved later...
  for (unsigned i = 0; i < NumEntries; ++i)
    Tab.push_back(PATypeHandle<Type>(OpaqueType::get(), this));

  // Loop through reading all of the types.  Forward types will make use of the
  // opaque types just inserted.
  //
  for (unsigned i = 0; i < NumEntries; ++i) {
    const Type *NewTy = parseTypeConstant(Buf, EndBuf), *OldTy = Tab[i].get();
    if (NewTy == 0) return failure(true);
    BCR_TRACE(4, "#" << i << ": Read Type Constant: '" << NewTy <<
              "' Replacing: " << OldTy << "\n");

    // Don't insertValue the new type... instead we want to replace the opaque
    // type with the new concrete value...
    //

    // Refine the abstract type to the new type.  This causes all uses of the
    // abstract type to use the newty.  This also will cause the opaque type
    // to be deleted...
    //
    cast<DerivedType>(Tab[i].get())->refineAbstractTypeTo(NewTy);

    // This should have replace the old opaque type with the new type in the
    // value table... or with a preexisting type that was already in the system
    assert(Tab[i] != OldTy && "refineAbstractType didn't work!");
  }

  BCR_TRACE(5, "Resulting types:\n");
  for (unsigned i = 0; i < NumEntries; ++i) {
    BCR_TRACE(5, cast<const Type>(Tab[i]) << "\n");
  }
  return false;
}


bool BytecodeParser::parseConstantValue(const uchar *&Buf, const uchar *EndBuf,
                                        const Type *Ty, Constant *&V) {
  switch (Ty->getPrimitiveID()) {
  case Type::BoolTyID: {
    unsigned Val;
    if (read_vbr(Buf, EndBuf, Val)) return failure(true);
    if (Val != 0 && Val != 1) return failure(true);
    V = ConstantBool::get(Val == 1);
    break;
  }

  case Type::UByteTyID:   // Unsigned integer types...
  case Type::UShortTyID:
  case Type::UIntTyID: {
    unsigned Val;
    if (read_vbr(Buf, EndBuf, Val)) return failure(true);
    if (!ConstantUInt::isValueValidForType(Ty, Val)) return failure(true);
    V = ConstantUInt::get(Ty, Val);
    break;
  }

  case Type::ULongTyID: {
    uint64_t Val;
    if (read_vbr(Buf, EndBuf, Val)) return failure(true);
    V = ConstantUInt::get(Ty, Val);
    break;
  }

  case Type::SByteTyID:   // Unsigned integer types...
  case Type::ShortTyID:
  case Type::IntTyID: {
    int Val;
    if (read_vbr(Buf, EndBuf, Val)) return failure(true);
    if (!ConstantSInt::isValueValidForType(Ty, Val)) return failure(true);
    V = ConstantSInt::get(Ty, Val);
    break;
  }

  case Type::LongTyID: {
    int64_t Val;
    if (read_vbr(Buf, EndBuf, Val)) return failure(true);
    V = ConstantSInt::get(Ty, Val);
    break;
  }

  case Type::FloatTyID: {
    float F;
    if (input_data(Buf, EndBuf, &F, &F+1)) return failure(true);
    V = ConstantFP::get(Ty, F);
    break;
  }

  case Type::DoubleTyID: {
    double Val;
    if (input_data(Buf, EndBuf, &Val, &Val+1)) return failure(true);
    V = ConstantFP::get(Ty, Val);
    break;
  }

  case Type::TypeTyID:
    assert(0 && "Type constants should be handled seperately!!!");
    abort();

  case Type::ArrayTyID: {
    const ArrayType *AT = cast<const ArrayType>(Ty);
    unsigned NumElements;
    if (AT->isSized())          // Sized array, # elements stored in type!
      NumElements = (unsigned)AT->getNumElements();
    else                        // Unsized array, # elements stored in stream!
      if (read_vbr(Buf, EndBuf, NumElements)) return failure(true);

    vector<Constant*> Elements;
    while (NumElements--) {   // Read all of the elements of the constant.
      unsigned Slot;
      if (read_vbr(Buf, EndBuf, Slot)) return failure(true);
      Value *V = getValue(AT->getElementType(), Slot, false);
      if (!V || !isa<Constant>(V)) return failure(true);
      Elements.push_back(cast<Constant>(V));
    }
    V = ConstantArray::get(AT, Elements);
    break;
  }

  case Type::StructTyID: {
    const StructType *ST = cast<StructType>(Ty);
    const StructType::ElementTypes &ET = ST->getElementTypes();

    vector<Constant *> Elements;
    for (unsigned i = 0; i < ET.size(); ++i) {
      unsigned Slot;
      if (read_vbr(Buf, EndBuf, Slot)) return failure(true);
      Value *V = getValue(ET[i], Slot, false);
      if (!V || !isa<Constant>(V))
	return failure(true);
      Elements.push_back(cast<Constant>(V));      
    }

    V = ConstantStruct::get(ST, Elements);
    break;
  }    

  case Type::PointerTyID: {
    const PointerType *PT = cast<const PointerType>(Ty);
    unsigned SubClass;
    if (read_vbr(Buf, EndBuf, SubClass)) return failure(true);
    switch (SubClass) {
    case 0:    // ConstantPointerNull value...
      V = ConstantPointerNull::get(PT);
      break;

    case 1: {  // ConstantPointerRef value...
      unsigned Slot;
      if (read_vbr(Buf, EndBuf, Slot)) return failure(true);
      BCR_TRACE(4, "CPPR: Type: '" << Ty << "'  slot: " << Slot << "\n");

      // Check to see if we have already read this global variable yet...
      Value *Val = getValue(PT, Slot, false);
      GlobalValue *GV;
      if (Val) {
	if (!(GV = dyn_cast<GlobalValue>(Val))) return failure(true);
	BCR_TRACE(5, "Value Found in ValueTable!\n");
      } else {         // Nope... see if we have previously forward ref'd it
	GlobalRefsType::iterator I = GlobalRefs.find(make_pair(PT, Slot));
	if (I != GlobalRefs.end()) {
	  BCR_TRACE(5, "Previous forward ref found!\n");
	  GV = I->second;
	} else {
	  BCR_TRACE(5, "Creating new forward ref variable!\n");

	  // Create a placeholder for the global variable reference...
	  GlobalVariable *GVar =
	    new GlobalVariable(PT->getValueType(), false, true);

	  // Keep track of the fact that we have a forward ref to recycle it
	  GlobalRefs.insert(make_pair(make_pair(PT, Slot), GVar));

	  // Must temporarily push this value into the module table...
	  TheModule->getGlobalList().push_back(GVar);
	  GV = GVar;
	}
      }
      
      V = ConstantPointerRef::get(GV);
      break;
    }
    default:
      return failure(true);
    }
    break;
  }

  default:
    cerr << __FILE__ << ":" << __LINE__ 
	 << ": Don't know how to deserialize constant value of type '"
	 << Ty->getName() << "'\n";
    return failure(true);
  }

  return false;
}

bool BytecodeParser::ParseConstantPool(const uchar *&Buf, const uchar *EndBuf,
				       ValueTable &Tab, 
				       TypeValuesListTy &TypeTab) {
  while (Buf < EndBuf) {
    unsigned NumEntries, Typ;

    if (read_vbr(Buf, EndBuf, NumEntries) ||
        read_vbr(Buf, EndBuf, Typ)) return failure(true);
    const Type *Ty = getType(Typ);
    if (Ty == 0) return failure(true);
    BCR_TRACE(3, "Type: '" << Ty << "'  NumEntries: " << NumEntries << "\n");

    if (Typ == Type::TypeTyID) {
      if (parseTypeConstants(Buf, EndBuf, TypeTab, NumEntries)) return true;
    } else {
      for (unsigned i = 0; i < NumEntries; ++i) {
	Constant *I;
	if (parseConstantValue(Buf, EndBuf, Ty, I)) return failure(true);
	BCR_TRACE(4, "Read Constant: '" << I << "'\n");
	if (insertValue(I, Tab) == -1) return failure(true);
      }
    }
  }
  
  if (Buf > EndBuf) return failure(true);
  return false;
}
