//===-- llvm/Target/TargetData.h - Data size & alignment routines-*- C++ -*-==//
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

#ifndef LLVM_TARGET_TARGETDATA_H
#define LLVM_TARGET_TARGETDATA_H

#include "llvm/Type.h"

class StructType;
class StructLayout;

class TargetData {
  unsigned char ByteAlignment;         // Defaults to 1 bytes
  unsigned char ShortAlignment;        // Defaults to 2 bytes
  unsigned char IntAlignment;          // Defaults to 4 bytes
  unsigned char LongAlignment;         // Defaults to 8 bytes
  unsigned char FloatAlignment;        // Defaults to 4 bytes
  unsigned char DoubleAlignment;       // Defaults to 8 bytes
  unsigned char PointerSize;           // Defaults to 8 bytes
  unsigned char PointerAlignment;      // Defaults to 8 bytes
  AnnotationID  AID;                   // AID for structure layout annotation
 
  static Annotation *TypeAnFactory(AnnotationID, const Annotable *, void *);
public:
  TargetData(const string &TargetName, unsigned char PtrSize = 8,
	     unsigned char PtrAl = 8, unsigned char DoubleAl = 8,
	     unsigned char FloatAl = 4, unsigned char LongAl = 8, 
	     unsigned char IntAl = 4, unsigned char ShortAl = 2,
	     unsigned char ByteAl = 1);
  ~TargetData();  // Not virtual, do not subclass this class

  unsigned char getByteAlignment()    const { return    ByteAlignment; }
  unsigned char getShortAlignment()   const { return   ShortAlignment; }
  unsigned char getIntAlignment()     const { return     IntAlignment; }
  unsigned char getLongAlignment()    const { return    LongAlignment; }
  unsigned char getFloatAlignment()   const { return   FloatAlignment; }
  unsigned char getDoubleAlignment()  const { return  DoubleAlignment; }
  unsigned char getPointerAlignment() const { return PointerAlignment; }
  unsigned char getPointerSize()      const { return PointerSize; }
  AnnotationID  getStructLayoutAID()  const { return AID; }

  // getTypeSize - Return the number of bytes neccesary to hold the specified
  // type
  unsigned      getTypeSize     (const Type *Ty) const;

  // getTypeAlignment - Return the minimum required alignment for the specified
  // type
  unsigned char getTypeAlignment(const Type *Ty) const;

  // getIndexOffset - return the offset from the beginning of the type for the
  // specified indices.  This is used to implement getElementPtr and load and 
  // stores that include the implicit form of getelementptr.
  //
  unsigned      getIndexedOffset(const Type *Ty, 
				 const vector<ConstPoolVal*> &Indices) const;

  inline const StructLayout *getStructLayout(const StructType *Ty) const {
    return (const StructLayout*)((const Type*)Ty)->getOrCreateAnnotation(AID);
  }
};

// This annotation (attached ONLY to StructType classes) is used to lazily
// calculate structure layout information for a target machine, based on the
// TargetData structure.
//
struct StructLayout : public Annotation {
  vector<unsigned> MemberOffsets;
  unsigned StructSize;
  unsigned StructAlignment;
private:
  friend class TargetData;   // Only TargetData can create this class
  inline StructLayout(const StructType *ST, const TargetData &TD);
};

#endif
