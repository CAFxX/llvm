//===-- ConstantWriter.cpp - Functions for writing constants --------------===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file implements the routines for encoding constants to a bytecode 
// stream.
//
//===----------------------------------------------------------------------===//

#include "WriterInternals.h"
#include "llvm/Constants.h"
#include "llvm/SymbolTable.h"
#include "llvm/DerivedTypes.h"
#include "Support/Statistic.h"
using namespace llvm;

void BytecodeWriter::outputType(const Type *T) {
  output_vbr((unsigned)T->getTypeID(), Out);
  
  // That's all there is to handling primitive types...
  if (T->isPrimitiveType()) {
    return;     // We might do this if we alias a prim type: %x = type int
  }

  switch (T->getTypeID()) {   // Handle derived types now.
  case Type::FunctionTyID: {
    const FunctionType *MT = cast<FunctionType>(T);
    int Slot = Table.getSlot(MT->getReturnType());
    assert(Slot != -1 && "Type used but not available!!");
    output_vbr((unsigned)Slot, Out);

    // Output the number of arguments to function (+1 if varargs):
    output_vbr((unsigned)MT->getNumParams()+MT->isVarArg(), Out);

    // Output all of the arguments...
    FunctionType::param_iterator I = MT->param_begin();
    for (; I != MT->param_end(); ++I) {
      Slot = Table.getSlot(*I);
      assert(Slot != -1 && "Type used but not available!!");
      output_vbr((unsigned)Slot, Out);
    }

    // Terminate list with VoidTy if we are a varargs function...
    if (MT->isVarArg())
      output_vbr((unsigned)Type::VoidTyID, Out);
    break;
  }

  case Type::ArrayTyID: {
    const ArrayType *AT = cast<ArrayType>(T);
    int Slot = Table.getSlot(AT->getElementType());
    assert(Slot != -1 && "Type used but not available!!");
    output_vbr((unsigned)Slot, Out);
    //std::cerr << "Type slot = " << Slot << " Type = " << T->getName() << endl;

    output_vbr(AT->getNumElements(), Out);
    break;
  }

  case Type::StructTyID: {
    const StructType *ST = cast<StructType>(T);

    // Output all of the element types...
    for (StructType::element_iterator I = ST->element_begin(),
           E = ST->element_end(); I != E; ++I) {
      int Slot = Table.getSlot(*I);
      assert(Slot != -1 && "Type used but not available!!");
      output_vbr((unsigned)Slot, Out);
    }

    // Terminate list with VoidTy
    output_vbr((unsigned)Type::VoidTyID, Out);
    break;
  }

  case Type::PointerTyID: {
    const PointerType *PT = cast<PointerType>(T);
    int Slot = Table.getSlot(PT->getElementType());
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
    std::cerr << __FILE__ << ":" << __LINE__ << ": Don't know how to serialize"
              << " Type '" << T->getDescription() << "'\n";
    break;
  }
}

void BytecodeWriter::outputConstant(const Constant *CPV) {
  assert((CPV->getType()->isPrimitiveType() || !CPV->isNullValue()) &&
         "Shouldn't output null constants!");

  // We must check for a ConstantExpr before switching by type because
  // a ConstantExpr can be of any type, and has no explicit value.
  // 
  if (const ConstantExpr *CE = dyn_cast<ConstantExpr>(CPV)) {
    // FIXME: Encoding of constant exprs could be much more compact!
    assert(CE->getNumOperands() > 0 && "ConstantExpr with 0 operands");
    output_vbr(CE->getNumOperands(), Out);   // flags as an expr
    output_vbr(CE->getOpcode(), Out);        // flags as an expr
    
    for (User::const_op_iterator OI = CE->op_begin(); OI != CE->op_end(); ++OI){
      int Slot = Table.getSlot(*OI);
      assert(Slot != -1 && "Unknown constant used in ConstantExpr!!");
      output_vbr((unsigned)Slot, Out);
      Slot = Table.getSlot((*OI)->getType());
      output_vbr((unsigned)Slot, Out);
    }
    return;
  } else {
    output_vbr(0U, Out);       // flag as not a ConstantExpr
  }
  
  switch (CPV->getType()->getTypeID()) {
  case Type::BoolTyID:    // Boolean Types
    if (cast<ConstantBool>(CPV)->getValue())
      output_vbr(1U, Out);
    else
      output_vbr(0U, Out);
    break;

  case Type::UByteTyID:   // Unsigned integer types...
  case Type::UShortTyID:
  case Type::UIntTyID:
  case Type::ULongTyID:
    output_vbr(cast<ConstantUInt>(CPV)->getValue(), Out);
    break;

  case Type::SByteTyID:   // Signed integer types...
  case Type::ShortTyID:
  case Type::IntTyID:
  case Type::LongTyID:
    output_vbr(cast<ConstantSInt>(CPV)->getValue(), Out);
    break;

  case Type::ArrayTyID: {
    const ConstantArray *CPA = cast<ConstantArray>(CPV);
    assert(!CPA->isString() && "Constant strings should be handled specially!");

    for (unsigned i = 0; i != CPA->getNumOperands(); ++i) {
      int Slot = Table.getSlot(CPA->getOperand(i));
      assert(Slot != -1 && "Constant used but not available!!");
      output_vbr((unsigned)Slot, Out);
    }
    break;
  }

  case Type::StructTyID: {
    const ConstantStruct *CPS = cast<ConstantStruct>(CPV);
    const std::vector<Use> &Vals = CPS->getValues();

    for (unsigned i = 0; i < Vals.size(); ++i) {
      int Slot = Table.getSlot(Vals[i]);
      assert(Slot != -1 && "Constant used but not available!!");
      output_vbr((unsigned)Slot, Out);
    }
    break;
  }

  case Type::PointerTyID:
    assert(0 && "No non-null, non-constant-expr constants allowed!");
    abort();

  case Type::FloatTyID: {   // Floating point types...
    float Tmp = (float)cast<ConstantFP>(CPV)->getValue();
    output_float(Tmp, Out);
    break;
  }
  case Type::DoubleTyID: {
    double Tmp = cast<ConstantFP>(CPV)->getValue();
    output_double(Tmp, Out);
    break;
  }

  case Type::VoidTyID: 
  case Type::LabelTyID:
  default:
    std::cerr << __FILE__ << ":" << __LINE__ << ": Don't know how to serialize"
              << " type '" << CPV->getType() << "'\n";
    break;
  }
  return;
}

void BytecodeWriter::outputConstantStrings() {
  SlotCalculator::string_iterator I = Table.string_begin();
  SlotCalculator::string_iterator E = Table.string_end();
  if (I == E) return;  // No strings to emit

  // If we have != 0 strings to emit, output them now.  Strings are emitted into
  // the 'void' type plane.
  output_vbr(unsigned(E-I), Out);
  output_vbr(Type::VoidTyID, Out);
    
  // Emit all of the strings.
  for (I = Table.string_begin(); I != E; ++I) {
    const ConstantArray *Str = *I;
    int Slot = Table.getSlot(Str->getType());
    assert(Slot != -1 && "Constant string of unknown type?");
    output_vbr((unsigned)Slot, Out);
    
    // Now that we emitted the type (which indicates the size of the string),
    // emit all of the characters.
    std::string Val = Str->getAsString();
    output_data(Val.c_str(), Val.c_str()+Val.size(), Out);
  }
}
