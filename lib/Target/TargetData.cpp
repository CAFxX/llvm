//===-- TargetData.cpp - Data size & alignment routines --------------------==//
//
// This file defines target properties related to datatype size/offset/alignment
// information.  It uses lazy annotations to cache information about how 
// structure types are laid out and used.
//
// This structure should be created once, filled in if the defaults are not
// correct and then passed around by const&.  None of the members functions
// require modification to the object.
//
//===----------------------------------------------------------------------===//

#include "llvm/Target/TargetData.h"
#include "llvm/DerivedTypes.h"
#include "llvm/ConstPoolVals.h"

static inline void getTypeInfo(const Type *Ty, const TargetData *TD,
			       unsigned &Size, unsigned char &Alignment);

//===----------------------------------------------------------------------===//
// Support for StructLayout Annotation
//===----------------------------------------------------------------------===//

StructLayout::StructLayout(const StructType *ST, const TargetData &TD) 
  : Annotation(TD.getStructLayoutAID()) {
  StructAlignment = 0;
  StructSize = 0;

  // Loop over each of the elements, placing them in memory...
  for (StructType::ElementTypes::const_iterator
	 TI = ST->getElementTypes().begin(), 
	 TE = ST->getElementTypes().end(); TI != TE; ++TI) {
    const Type *Ty = *TI;
    unsigned char A;
    unsigned TySize, TyAlign;
    getTypeInfo(Ty, &TD, TySize, A);  TyAlign = A;

    // Add padding if neccesary to make the data element aligned properly...
    if (StructSize % TyAlign != 0)
      StructSize = (StructSize/TyAlign + 1) * TyAlign;   // Add padding...

    // Keep track of maximum alignment constraint
    StructAlignment = max(TyAlign, StructAlignment);

    MemberOffsets.push_back(StructSize);
    StructSize += TySize;                 // Consume space for this data item...
  }

  // Add padding to the end of the struct so that it could be put in an array
  // and all array elements would be aligned correctly.
  if (StructSize % StructAlignment != 0)
    StructSize = (StructSize/StructAlignment + 1) * StructAlignment;

  if (StructSize == 0) {
    StructSize = 1;           // Empty struct is 1 byte
    StructAlignment = 1;
  }
}

Annotation *TargetData::TypeAnFactory(AnnotationID AID, const Annotable *T,
				      void *D) {
  const TargetData &TD = *(const TargetData*)D;
  assert(AID == TD.AID && "Target data annotation ID mismatch!");
  const Type *Ty = cast<const Type>((const Value *)T);
  assert(Ty->isStructType() && 
	 "Can only create StructLayout annotation on structs!");
  return new StructLayout((const StructType *)Ty, TD);
}

//===----------------------------------------------------------------------===//
//                       TargetData Class Implementation
//===----------------------------------------------------------------------===//

TargetData::TargetData(const string &TargetName, unsigned char PtrSize = 8,
	     unsigned char PtrAl = 8, unsigned char DoubleAl = 8,
	     unsigned char FloatAl = 4, unsigned char LongAl = 8, 
	     unsigned char IntAl = 4, unsigned char ShortAl = 2,
	     unsigned char ByteAl = 1)
  : AID(AnnotationManager::getID("TargetData::" + TargetName)) {
  AnnotationManager::registerAnnotationFactory(AID, TypeAnFactory, this);

  PointerSize      = PtrSize;
  PointerAlignment = PtrAl;
  DoubleAlignment  = DoubleAl;
  FloatAlignment   = FloatAl;
  LongAlignment    = LongAl;
  IntAlignment     = IntAl;
  ShortAlignment   = ShortAl;
  ByteAlignment    = ByteAl;
}

TargetData::~TargetData() {
  AnnotationManager::registerAnnotationFactory(AID, 0);   // Deregister factory
}

static inline void getTypeInfo(const Type *Ty, const TargetData *TD,
			       unsigned &Size, unsigned char &Alignment) {
  switch (Ty->getPrimitiveID()) {
  case Type::VoidTyID:
  case Type::BoolTyID:
  case Type::UByteTyID:
  case Type::SByteTyID:  Size = 1; Alignment = TD->getByteAlignment(); return;
  case Type::UShortTyID:
  case Type::ShortTyID:  Size = 2; Alignment = TD->getShortAlignment(); return;
  case Type::UIntTyID:
  case Type::IntTyID:    Size = 4; Alignment = TD->getIntAlignment(); return;
  case Type::ULongTyID:
  case Type::LongTyID:   Size = 8; Alignment = TD->getLongAlignment(); return;
  case Type::FloatTyID:  Size = 4; Alignment = TD->getFloatAlignment(); return;
  case Type::DoubleTyID: Size = 8; Alignment = TD->getDoubleAlignment(); return;
  case Type::LabelTyID:
  case Type::PointerTyID:
    Size = TD->getPointerSize(); Alignment = TD->getPointerAlignment();
    return;
  case Type::ArrayTyID: {
    const ArrayType *ATy = (const ArrayType *)Ty;
    assert(ATy->isSized() && "Can't get TypeInfo of an unsized array!");
    getTypeInfo(ATy->getElementType(), TD, Size, Alignment);
    Size *= ATy->getNumElements();
    return;
  }
  case Type::StructTyID: {
    // Get the layout annotation... which is lazily created on demand.
    const StructLayout *Layout = TD->getStructLayout((const StructType*)Ty);
    Size = Layout->StructSize; Alignment = Layout->StructAlignment;
    return;
  }
    
  case Type::TypeTyID:
  default:
    assert(0 && "Bad type for getTypeInfo!!!");
    return;
  }
}

unsigned TargetData::getTypeSize(const Type *Ty) const {
  unsigned Size; unsigned char Align;
  getTypeInfo(Ty, this, Size, Align);
  return Size;
}

unsigned char TargetData::getTypeAlignment(const Type *Ty) const {
  unsigned Size; unsigned char Align;
  getTypeInfo(Ty, this, Size, Align);
  return Align;
}

unsigned TargetData::getIndexedOffset(const Type *ptrTy,
				      const vector<ConstPoolVal*> &Idx) const {
  const PointerType *PtrTy = cast<const PointerType>(ptrTy);
  unsigned Result = 0;

  // Get the type pointed to...
  const Type *Ty = PtrTy->getValueType();

  for (unsigned CurIDX = 0; CurIDX < Idx.size(); ++CurIDX) {
    if (const StructType *STy = dyn_cast<const StructType>(Ty)) {
      assert(Idx[CurIDX]->getType() == Type::UByteTy && "Illegal struct idx");
      unsigned FieldNo = ((ConstPoolUInt*)Idx[CurIDX])->getValue();

      // Get structure layout information...
      const StructLayout *Layout = getStructLayout(STy);

      // Add in the offset, as calculated by the structure layout info...
      assert(FieldNo < Layout->MemberOffsets.size() && "FieldNo out of range!");
      Result += Layout->MemberOffsets[FieldNo];
      
      // Update Ty to refer to current element
      Ty = STy->getElementTypes()[FieldNo];

    } else if (const ArrayType *ATy = dyn_cast<const ArrayType>(Ty)) {
      assert(0 && "Loading from arrays not implemented yet!");
    } else {
      assert(0 && "Indexing type that is not struct or array?");
      return 0;                         // Load directly through ptr
    }
  }

  return Result;
}
