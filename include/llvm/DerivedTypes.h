//===-- llvm/DerivedTypes.h - Classes for handling data types ----*- C++ -*--=//
//
// This file contains the declarations of classes that represent "derived 
// types".  These are things like "arrays of x" or "structure of x, y, z" or
// "method returning x taking (y,z) as parameters", etc...
//
// The implementations of these classes live in the Type.cpp file.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_DERIVED_TYPES_H
#define LLVM_DERIVED_TYPES_H

#include "llvm/Type.h"
#include "llvm/Codegen/TargetMachine.h"
#include "vector"

class TargetMachine;


// Future derived types: SIMD packed format


class MethodType : public Type {
public:
  typedef vector<const Type*> ParamTypes;
private:
  const Type *ResultType;
  ParamTypes ParamTys;

  MethodType(const MethodType &);                   // Do not implement
  const MethodType &operator=(const MethodType &);  // Do not implement
protected:
  // This should really be private, but it squelches a bogus warning
  // from GCC to make them protected:  warning: `class MethodType' only 
  // defines private constructors and has no friends

  // Private ctor - Only can be created by a static member...
  MethodType(const Type *Result, const vector<const Type*> &Params, 
             const string &Name);
public:

  inline const Type *getReturnType() const { return ResultType; }
  inline const ParamTypes &getParamTypes() const { return ParamTys; }

  static const MethodType *getMethodType(const Type *Result, 
                                         const ParamTypes &Params);
  static const MethodType *get(const Type *Result, const ParamTypes &Params) {
    return getMethodType(Result, Params);
  }
};



class ArrayType : public Type {
private:
  const Type *ElementType;
  int NumElements;       // >= 0 for sized array, -1 for unbounded/unknown array

  ArrayType(const ArrayType &);                   // Do not implement
  const ArrayType &operator=(const ArrayType &);  // Do not implement
protected:
  // This should really be private, but it squelches a bogus warning
  // from GCC to make them protected:  warning: `class ArrayType' only 
  // defines private constructors and has no friends


  // Private ctor - Only can be created by a static member...
  ArrayType(const Type *ElType, int NumEl, const string &Name);
public:

  inline const Type *getElementType() const { return ElementType; }
  inline int         getNumElements() const { return NumElements; }

  inline bool isSized()   const { return NumElements >= 0; }
  inline bool isUnsized() const { return NumElements == -1; }

  static const ArrayType *getArrayType(const Type *ElementType, 
				       int NumElements = -1);
  static const ArrayType *get(const Type *ElementType, int NumElements = -1) {
    return getArrayType(ElementType, NumElements);
  }
};

class StructType : public Type {
public:
  typedef vector<const Type*> ElementTypes;

private:
  ElementTypes ETypes;
  struct StructSizeAndOffsetInfo {
    int storageSize;			// -1 until the value is computd
    vector<int> memberOffsets;		// -1 until values are computed 
    const TargetMachine* targetInfo;
  }
  *layoutCache;
  
private:
  StructType(const StructType &);                   // Do not implement
  const StructType &operator=(const StructType &);  // Do not implement
  
protected:
  // This should really be private, but it squelches a bogus warning
  // from GCC to make them protected:  warning: `class StructType' only 
  // defines private constructors and has no friends

  // Private ctor - Only can be created by a static member...
  StructType(const vector<const Type*> &Types, const string &Name);
  
  // Reset cached info so it will be computed when first requested
  void ResetCachedInfo() const;
  
public:

  inline const ElementTypes &getElementTypes() const { return ETypes; }
  static const StructType *getStructType(const ElementTypes &Params);
  static const StructType *get(const ElementTypes &Params) {
    return getStructType(Params);
  }
  unsigned int		   getStorageSize(const TargetMachine& tmi) const;
  unsigned int		   getElementOffset(int i, const TargetMachine& tmi) const;
};


inline unsigned int
StructType::getStorageSize(const TargetMachine& tmi) const
{
  if (layoutCache->targetInfo != NULL && ! (* layoutCache->targetInfo == tmi))
    {// target machine has changed (hey it could happen). discard cached info.
      ResetCachedInfo();
      layoutCache->targetInfo = &tmi;
    }
  
  if (layoutCache->storageSize < 0)
    {
      layoutCache->storageSize = tmi.findOptimalStorageSize(this);
      assert(layoutCache->storageSize >= 0);
    }
  
  return layoutCache->storageSize;
}


inline unsigned int
StructType::getElementOffset(int i, const TargetMachine& tmi) const
{
  if (layoutCache->targetInfo != NULL && ! (* layoutCache->targetInfo == tmi))
    {// target machine has changed (hey it could happen). discard cached info.
      ResetCachedInfo();
    }
  
  if (layoutCache->memberOffsets[i] < 0)
    {
      layoutCache->targetInfo = &tmi;	// remember which target was used
      
      unsigned int* offsetVec = tmi.findOptimalMemberOffsets(this);
      for (unsigned i=0, N=layoutCache->memberOffsets.size(); i < N; i++)
	{
	  layoutCache->memberOffsets[i] = offsetVec[i];
	  assert(layoutCache->memberOffsets[i] >= 0);
	}
      delete[] offsetVec; 
    }
  
  return layoutCache->memberOffsets[i];
}


inline void
StructType::ResetCachedInfo() const
{
  layoutCache->storageSize = -1;
  layoutCache->memberOffsets.insert(layoutCache->memberOffsets.begin(),
				    ETypes.size(), -1);
  layoutCache->targetInfo = NULL;
}


class PointerType : public Type {
private:
  const Type *ValueType;

  PointerType(const PointerType &);                   // Do not implement
  const PointerType &operator=(const PointerType &);  // Do not implement
protected:
  // This should really be private, but it squelches a bogus warning
  // from GCC to make them protected:  warning: `class PointerType' only 
  // defines private constructors and has no friends


  // Private ctor - Only can be created by a static member...
  PointerType(const Type *ElType);
public:

  inline const Type *getValueType() const { return ValueType; }


  static const PointerType *getPointerType(const Type *ElementType);
  static const PointerType *get(const Type *ElementType) {
    return getPointerType(ElementType);
  }
};

#endif
