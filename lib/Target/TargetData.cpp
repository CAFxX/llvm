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
#include "llvm/Constants.h"

static inline void getTypeInfo(const Type *Ty, const TargetData *TD,
			       uint64_t &Size, unsigned char &Alignment);

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
    unsigned TyAlign;
    uint64_t TySize;
    getTypeInfo(Ty, &TD, TySize, A);
    TyAlign = A;

    // Add padding if neccesary to make the data element aligned properly...
    if (StructSize % TyAlign != 0)
      StructSize = (StructSize/TyAlign + 1) * TyAlign;   // Add padding...

    // Keep track of maximum alignment constraint
    StructAlignment = std::max(TyAlign, StructAlignment);

    MemberOffsets.push_back(StructSize);
    StructSize += TySize;                 // Consume space for this data item
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
  assert(isa<StructType>(Ty) && 
	 "Can only create StructLayout annotation on structs!");
  return new StructLayout((const StructType *)Ty, TD);
}

//===----------------------------------------------------------------------===//
//                       TargetData Class Implementation
//===----------------------------------------------------------------------===//

TargetData::TargetData(const std::string &TargetName,
             unsigned char IntRegSize, unsigned char PtrSize,
	     unsigned char PtrAl, unsigned char DoubleAl,
	     unsigned char FloatAl, unsigned char LongAl, 
	     unsigned char IntAl, unsigned char ShortAl,
	     unsigned char ByteAl)
  : AID(AnnotationManager::getID("TargetData::" + TargetName)) {
  AnnotationManager::registerAnnotationFactory(AID, TypeAnFactory, this);

  IntegerRegSize   = IntRegSize;
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
			       uint64_t &Size, unsigned char &Alignment) {
  assert(Ty->isSized() && "Cannot getTypeInfo() on a type that is unsized!");
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

uint64_t TargetData::getTypeSize(const Type *Ty) const {
  uint64_t Size;
  unsigned char Align;
  getTypeInfo(Ty, this, Size, Align);
  return Size;
}

unsigned char TargetData::getTypeAlignment(const Type *Ty) const {
  uint64_t Size;
  unsigned char Align;
  getTypeInfo(Ty, this, Size, Align);
  return Align;
}

uint64_t TargetData::getIndexedOffset(const Type *ptrTy,
				      const std::vector<Value*> &Idx) const {
  const Type *Ty = ptrTy;
  assert(isa<PointerType>(Ty) && "Illegal argument for getIndexedOffset()");
  uint64_t Result = 0;

  for (unsigned CurIDX = 0; CurIDX < Idx.size(); ++CurIDX) {
    if (Idx[CurIDX]->getType() == Type::UIntTy) {
      // Get the array index and the size of each array element.
      // Both must be known constants, or this will fail.
      unsigned arrayIdx = cast<ConstantUInt>(Idx[CurIDX])->getValue();
      uint64_t elementSize = this->getTypeSize(Ty);
      Result += arrayIdx * elementSize;

      // Update Ty to refer to current element
      Ty = cast<SequentialType>(Ty)->getElementType();

    } else if (const StructType *STy = dyn_cast<const StructType>(Ty)) {
      assert(Idx[CurIDX]->getType() == Type::UByteTy && "Illegal struct idx");
      unsigned FieldNo = cast<ConstantUInt>(Idx[CurIDX])->getValue();

      // Get structure layout information...
      const StructLayout *Layout = getStructLayout(STy);

      // Add in the offset, as calculated by the structure layout info...
      assert(FieldNo < Layout->MemberOffsets.size() &&"FieldNo out of range!");
      Result += Layout->MemberOffsets[FieldNo];

      // Update Ty to refer to current element
      Ty = STy->getElementTypes()[FieldNo];

    } else if (isa<const ArrayType>(Ty)) {
      assert(0 && "Loading from arrays not implemented yet!");
    } else {
      assert(0 && "Indexing type that is not struct or array?");
      return 0;                         // Load directly through ptr
    }
  }

  return Result;
}
