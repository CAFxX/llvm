//===-- WriteConst.cpp - Functions for writing constants ---------*- C++ -*--=//
//
// This file implements the routines for encoding constants to a bytecode 
// stream.
//
// Note that the performance of this library is not terribly important, because
// it shouldn't be used by JIT type applications... so it is not a huge focus
// at least.  :)
//
//===----------------------------------------------------------------------===//

#include "WriterInternals.h"
#include "llvm/ConstantVals.h"
#include "llvm/SymbolTable.h"
#include "llvm/DerivedTypes.h"
#include <iostream>
using std::cerr;

void BytecodeWriter::outputType(const Type *T) {
  output_vbr((unsigned)T->getPrimitiveID(), Out);
  
  // That's all there is to handling primitive types...
  if (T->isPrimitiveType())
    return;     // We might do this if we alias a prim type: %x = type int
  
  switch (T->getPrimitiveID()) {   // Handle derived types now.
  case Type::FunctionTyID: {
    const FunctionType *MT = cast<const FunctionType>(T);
    int Slot = Table.getValSlot(MT->getReturnType());
    assert(Slot != -1 && "Type used but not available!!");
    output_vbr((unsigned)Slot, Out);

    // Output the number of arguments to method (+1 if varargs):
    output_vbr(MT->getParamTypes().size()+MT->isVarArg(), Out);

    // Output all of the arguments...
    FunctionType::ParamTypes::const_iterator I = MT->getParamTypes().begin();
    for (; I != MT->getParamTypes().end(); ++I) {
      Slot = Table.getValSlot(*I);
      assert(Slot != -1 && "Type used but not available!!");
      output_vbr((unsigned)Slot, Out);
    }

    // Terminate list with VoidTy if we are a varargs function...
    if (MT->isVarArg())
      output_vbr((unsigned)Type::VoidTy->getPrimitiveID(), Out);
    break;
  }

  case Type::ArrayTyID: {
    const ArrayType *AT = cast<const ArrayType>(T);
    int Slot = Table.getValSlot(AT->getElementType());
    assert(Slot != -1 && "Type used but not available!!");
    output_vbr((unsigned)Slot, Out);
    //cerr << "Type slot = " << Slot << " Type = " << T->getName() << endl;

    output_vbr(AT->getNumElements(), Out);
    break;
  }

  case Type::StructTyID: {
    const StructType *ST = cast<const StructType>(T);

    // Output all of the element types...
    StructType::ElementTypes::const_iterator I = ST->getElementTypes().begin();
    for (; I != ST->getElementTypes().end(); ++I) {
      int Slot = Table.getValSlot(*I);
      assert(Slot != -1 && "Type used but not available!!");
      output_vbr((unsigned)Slot, Out);
    }

    // Terminate list with VoidTy
    output_vbr((unsigned)Type::VoidTy->getPrimitiveID(), Out);
    break;
  }

  case Type::PointerTyID: {
    const PointerType *PT = cast<const PointerType>(T);
    int Slot = Table.getValSlot(PT->getElementType());
    assert(Slot != -1 && "Type used but not available!!");
    output_vbr((unsigned)Slot, Out);
    break;
  }

  case Type::OpaqueTyID: {
    // No need to emit anything, just the count of opaque types is enough.
    break;
  }

  //case Type::PackedTyID:
  default:
    cerr << __FILE__ << ":" << __LINE__ << ": Don't know how to serialize"
	 << " Type '" << T->getDescription() << "'\n";
    break;
  }
}

bool BytecodeWriter::outputConstant(const Constant *CPV) {
  switch (CPV->getType()->getPrimitiveID()) {
  case Type::BoolTyID:    // Boolean Types
    if (cast<const ConstantBool>(CPV)->getValue())
      output_vbr((unsigned)1, Out);
    else
      output_vbr((unsigned)0, Out);
    break;

  case Type::UByteTyID:   // Unsigned integer types...
  case Type::UShortTyID:
  case Type::UIntTyID:
  case Type::ULongTyID:
    output_vbr(cast<const ConstantUInt>(CPV)->getValue(), Out);
    break;

  case Type::SByteTyID:   // Signed integer types...
  case Type::ShortTyID:
  case Type::IntTyID:
  case Type::LongTyID:
    output_vbr(cast<const ConstantSInt>(CPV)->getValue(), Out);
    break;

  case Type::TypeTyID:     // Serialize type type
    assert(0 && "Types should not be in the Constant!");
    break;

  case Type::ArrayTyID: {
    const ConstantArray *CPA = cast<const ConstantArray>(CPV);
    unsigned size = CPA->getValues().size();
    assert(size == cast<ArrayType>(CPA->getType())->getNumElements() && "ConstantArray out of whack!");
    for (unsigned i = 0; i < size; i++) {
      int Slot = Table.getValSlot(CPA->getOperand(i));
      assert(Slot != -1 && "Constant used but not available!!");
      output_vbr((unsigned)Slot, Out);
    }
    break;
  }

  case Type::StructTyID: {
    const ConstantStruct *CPS = cast<const ConstantStruct>(CPV);
    const std::vector<Use> &Vals = CPS->getValues();

    for (unsigned i = 0; i < Vals.size(); ++i) {
      int Slot = Table.getValSlot(Vals[i]);
      assert(Slot != -1 && "Constant used but not available!!");
      output_vbr((unsigned)Slot, Out);
    }      
    break;
  }

  case Type::PointerTyID: {
    const ConstantPointer *CPP = cast<const ConstantPointer>(CPV);
    if (isa<ConstantPointerNull>(CPP)) {
      output_vbr((unsigned)0, Out);
    } else if (const ConstantPointerRef *CPR = 
	                dyn_cast<ConstantPointerRef>(CPP)) {
      output_vbr((unsigned)1, Out);
      int Slot = Table.getValSlot((Value*)CPR->getValue());
      assert(Slot != -1 && "Global used but not available!!");
      output_vbr((unsigned)Slot, Out);
    } else {
      assert(0 && "Unknown ConstantPointer Subclass!");
    }
    break;
  }

  case Type::FloatTyID: {   // Floating point types...
    float Tmp = (float)cast<ConstantFP>(CPV)->getValue();
    output_data(&Tmp, &Tmp+1, Out);
    break;
  }
  case Type::DoubleTyID: {
    double Tmp = cast<ConstantFP>(CPV)->getValue();
    output_data(&Tmp, &Tmp+1, Out);
    break;
  }

  case Type::VoidTyID: 
  case Type::LabelTyID:
  default:
    cerr << __FILE__ << ":" << __LINE__ << ": Don't know how to serialize"
	 << " type '" << CPV->getType()->getName() << "'\n";
    break;
  }
  return false;
}
