//===-- llvm/ConstPoolVals.h - Constant Value nodes --------------*- C++ -*--=//
//
// This file contains the declarations for the ConstPoolVal class and all of
// its subclasses, which represent the different type of constant pool values
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CONSTPOOLVALS_H
#define LLVM_CONSTPOOLVALS_H

#include "llvm/User.h"
#include "llvm/SymTabValue.h"
#include "llvm/Tools/DataTypes.h"
#include <vector>

class ArrayType;
class StructType;

//===----------------------------------------------------------------------===//
//                            ConstPoolVal Class
//===----------------------------------------------------------------------===//

class ConstPoolVal : public User {
  SymTabValue *Parent;

  friend class ValueHolder<ConstPoolVal, SymTabValue, SymTabValue>;
  inline void setParent(SymTabValue *parent) { 
    Parent = parent;
  }

protected:
  inline ConstPoolVal(const Type *Ty, const string &Name = "") 
    : User(Ty, Value::ConstantVal, Name) { Parent = 0; }

public:
  // Specialize setName to handle symbol table majik...
  virtual void setName(const string &name);

  // Static constructor to create a '0' constant of arbitrary type...
  static ConstPoolVal *getNullConstant(const Type *Ty);

  // clone() - Create a copy of 'this' value that is identical in all ways
  // except the following:
  //   * The value has no parent
  //   * The value has no name
  //
  virtual ConstPoolVal *clone() const = 0;

  virtual string getStrValue() const = 0;
  virtual bool equals(const ConstPoolVal *V) const = 0;

  inline const SymTabValue *getParent() const { return Parent; }
  inline       SymTabValue *getParent()       { return Parent; }
  inline const     Value *getParentV() const { return Parent->getSTVParent(); }
  inline           Value *getParentV()       { return Parent->getSTVParent(); }
};



//===----------------------------------------------------------------------===//
//              Classes to represent constant pool variable defs
//===----------------------------------------------------------------------===//

//===---------------------------------------------------------------------------
// ConstPoolBool - Boolean Values
//
class ConstPoolBool : public ConstPoolVal {
  bool Val;
  ConstPoolBool(const ConstPoolBool &CP);
public:
  ConstPoolBool(bool V, const string &Name = "");

  virtual string getStrValue() const;
  virtual bool equals(const ConstPoolVal *V) const;

  virtual ConstPoolVal *clone() const { return new ConstPoolBool(*this); }

  inline bool getValue() const { return Val; }

  // setValue - Be careful... if there is more than one 'use' of this node, then
  // they will ALL see the value that you set...
  //
  inline void setValue(bool v) { Val = v; } 
};


//===---------------------------------------------------------------------------
// ConstPoolInt - Superclass of ConstPoolSInt & ConstPoolUInt, to make dealing
// with integral constants easier.
//
class ConstPoolInt : public ConstPoolVal {
protected:
  union {
    int64_t  Signed;
    uint64_t Unsigned;
  } Val;
  ConstPoolInt(const ConstPoolInt &CP);
public:
  ConstPoolInt(const Type *Ty,  int64_t V, const string &Name = "");
  ConstPoolInt(const Type *Ty, uint64_t V, const string &Name = "");

  virtual bool equals(const ConstPoolVal *V) const;

  // equals - Provide a helper method that can be used to determine if the 
  // constant contained within is equal to a constant.  This only works for very
  // small values, because this is all that can be represented with all types.
  //
  bool equals(unsigned char V) {
    assert(V <= 127 && "equals: Can only be used with very small constants!");
    return Val.Unsigned == V;
  }

  // isIntegral - Equilivent to isSigned() || isUnsigned, but with only a single
  // virtual function invocation.
  //
  virtual bool isIntegral() const { return 1; }

  // ConstPoolInt::get static method: return a constant pool int with the
  // specified value.  as above, we work only with very small values here.
  //
  static ConstPoolInt *get(const Type *Ty, unsigned char V);
};


//===---------------------------------------------------------------------------
// ConstPoolSInt - Signed Integer Values [sbyte, short, int, long]
//
class ConstPoolSInt : public ConstPoolInt {
  ConstPoolSInt(const ConstPoolSInt &CP) : ConstPoolInt(CP) {}
public:
  ConstPoolSInt(const Type *Ty, int64_t V, const string &Name = "");

  virtual ConstPoolVal *clone() const { return new ConstPoolSInt(*this); }

  virtual string getStrValue() const;

  static bool isValueValidForType(const Type *Ty, int64_t V);
  inline int64_t getValue() const { return Val.Signed; }
};


//===---------------------------------------------------------------------------
// ConstPoolUInt - Unsigned Integer Values [ubyte, ushort, uint, ulong]
//
class ConstPoolUInt : public ConstPoolInt {
  ConstPoolUInt(const ConstPoolUInt &CP) : ConstPoolInt(CP) {}
public:
  ConstPoolUInt(const Type *Ty, uint64_t V, const string &Name = "");

  virtual ConstPoolVal *clone() const { return new ConstPoolUInt(*this); }

  virtual string getStrValue() const;

  static bool isValueValidForType(const Type *Ty, uint64_t V);
  inline uint64_t getValue() const { return Val.Unsigned; }
};


//===---------------------------------------------------------------------------
// ConstPoolFP - Floating Point Values [float, double]
//
class ConstPoolFP : public ConstPoolVal {
  double Val;
  ConstPoolFP(const ConstPoolFP &CP);
public:
  ConstPoolFP(const Type *Ty, double V, const string &Name = "");

  virtual ConstPoolVal *clone() const { return new ConstPoolFP(*this); }
  virtual string getStrValue() const;
  virtual bool equals(const ConstPoolVal *V) const;

  static bool isValueValidForType(const Type *Ty, double V);
  inline double getValue() const { return Val; }
};


//===---------------------------------------------------------------------------
// ConstPoolType - Type Declarations
//
class ConstPoolType : public ConstPoolVal {
  const Type *Val;
  ConstPoolType(const ConstPoolType &CPT);
public:
  ConstPoolType(const Type *V, const string &Name = "");

  virtual ConstPoolVal *clone() const { return new ConstPoolType(*this); }
  virtual string getStrValue() const;
  virtual bool equals(const ConstPoolVal *V) const;

  inline const Type *getValue() const { return Val; }
};


//===---------------------------------------------------------------------------
// ConstPoolArray - Constant Array Declarations
//
class ConstPoolArray : public ConstPoolVal {
  ConstPoolArray(const ConstPoolArray &CPT);
public:
  ConstPoolArray(const ArrayType *T, vector<ConstPoolVal*> &V, 
		 const string &Name = "");

  virtual ConstPoolVal *clone() const { return new ConstPoolArray(*this); }
  virtual string getStrValue() const;
  virtual bool equals(const ConstPoolVal *V) const;

  inline const vector<Use> &getValues() const { return Operands; }
};


//===---------------------------------------------------------------------------
// ConstPoolStruct - Constant Struct Declarations
//
class ConstPoolStruct : public ConstPoolVal {
  ConstPoolStruct(const ConstPoolStruct &CPT);
public:
  ConstPoolStruct(const StructType *T, vector<ConstPoolVal*> &V, 
		  const string &Name = "");

  virtual ConstPoolVal *clone() const { return new ConstPoolStruct(*this); }
  virtual string getStrValue() const;
  virtual bool equals(const ConstPoolVal *V) const;

  inline const vector<Use> &getValues() const { return Operands; }
};

#endif
