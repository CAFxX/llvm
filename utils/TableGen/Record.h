//===- Record.h - Classes to represent Table Records ------------*- C++ -*-===//
//
//
//===----------------------------------------------------------------------===//

#ifndef RECORD_H
#define RECORD_H

#include <string>
#include <vector>
#include <map>
#include <iostream>
class Init;
class UnsetInit;
class BitInit;
class BitsInit;
class IntInit;
class StringInit;
class ListInit;
class DefInit;
class TypedInit;
class VarInit;
class FieldInit;
class VarBitInit;
class Record;

//===----------------------------------------------------------------------===//
//  Type Classes
//===----------------------------------------------------------------------===//

struct RecTy {
  virtual ~RecTy() {}

  virtual Init *convertValue( UnsetInit *UI) { return 0; }
  virtual Init *convertValue(   BitInit *BI) { return 0; }
  virtual Init *convertValue(  BitsInit *BI) { return 0; }
  virtual Init *convertValue(   IntInit *II) { return 0; }
  virtual Init *convertValue(StringInit *SI) { return 0; }
  virtual Init *convertValue(  ListInit *LI) { return 0; }
  virtual Init *convertValue(VarBitInit *VB) { return 0; }
  virtual Init *convertValue(   DefInit *DI) { return 0; }
  virtual Init *convertValue( TypedInit *TI) { return 0; }
  virtual Init *convertValue(   VarInit *VI) {
    return convertValue((TypedInit*)VI);
  }
  virtual Init *convertValue( FieldInit *FI) {
    return convertValue((TypedInit*)FI);
  }

  virtual void print(std::ostream &OS) const = 0;
  void dump() const;
};

inline std::ostream &operator<<(std::ostream &OS, const RecTy &Ty) {
  Ty.print(OS);
  return OS;
}


/// BitRecTy - 'bit' - Represent a single bit
///
struct BitRecTy : public RecTy {
  Init *convertValue(UnsetInit *UI) { return (Init*)UI; }
  Init *convertValue(BitInit *BI) { return (Init*)BI; }
  Init *convertValue(BitsInit *BI);
  Init *convertValue(IntInit *II);
  Init *convertValue(TypedInit *VI);
  Init *convertValue(VarBitInit *VB) { return (Init*)VB; }

  void print(std::ostream &OS) const { OS << "bit"; }
};


/// BitsRecTy - 'bits<n>' - Represent a fixed number of bits
///
class BitsRecTy : public RecTy {
  unsigned Size;
public:
  BitsRecTy(unsigned Sz) : Size(Sz) {}

  unsigned getNumBits() const { return Size; }

  Init *convertValue(UnsetInit *UI);
  Init *convertValue(BitInit *UI);
  Init *convertValue(BitsInit *BI);
  Init *convertValue(IntInit *II);
  Init *convertValue(TypedInit *VI);

  void print(std::ostream &OS) const { OS << "bits<" << Size << ">"; }
};


/// IntRecTy - 'int' - Represent an integer value of no particular size
///
struct IntRecTy : public RecTy {
  Init *convertValue(UnsetInit *UI) { return (Init*)UI; }
  Init *convertValue(IntInit *II) { return (Init*)II; }
  Init *convertValue(BitsInit *BI);
  Init *convertValue(TypedInit *TI);

  void print(std::ostream &OS) const { OS << "int"; }
};

/// StringRecTy - 'string' - Represent an string value
///
struct StringRecTy : public RecTy {
  Init *convertValue(UnsetInit *UI) { return (Init*)UI; }
  Init *convertValue(StringInit *SI) { return (Init*)SI; }
  Init *convertValue(TypedInit *VI);
  void print(std::ostream &OS) const { OS << "string"; }
};

/// ListRecTy - 'list<class>' - Represent a list defs, all of which must be
/// derived from the specified class.
///
class ListRecTy : public RecTy {
  Record *Class;
public:
  ListRecTy(Record *C) : Class(C) {}
  Init *convertValue(UnsetInit *UI) { return (Init*)UI; }
  Init *convertValue(ListInit *LI);
  
  void print(std::ostream &OS) const;
};

/// RecordRecTy - '<classname>' - Represent an instance of a class, such as:
/// (R32 X = EAX).
///
class RecordRecTy : public RecTy {
  Record *Rec;
public:
  RecordRecTy(Record *R) : Rec(R) {}

  Record *getRecord() const { return Rec; }

  Init *convertValue(UnsetInit *UI) { return (Init*)UI; }
  Init *convertValue(   DefInit *DI);

  void print(std::ostream &OS) const;
};



//===----------------------------------------------------------------------===//
//  Initializer Classes
//===----------------------------------------------------------------------===//

struct Init {
  virtual ~Init() {}

  /// isComplete - This virtual method should be overridden by values that may
  /// not be completely specified yet.
  virtual bool isComplete() const { return true; }

  /// print - Print out this value.
  virtual void print(std::ostream &OS) const = 0;

  /// dump - Debugging method that may be called through a debugger, just
  /// invokes print on cerr.
  void dump() const;

  /// convertInitializerTo - This virtual function is a simple call-back
  /// function that should be overridden to call the appropriate
  /// RecTy::convertValue method.
  ///
  virtual Init *convertInitializerTo(RecTy *Ty) = 0;

  /// convertInitializerBitRange - This method is used to implement the bitrange
  /// selection operator.  Given an initializer, it selects the specified bits
  /// out, returning them as a new init of bits type.  If it is not legal to use
  /// the bit subscript operator on this initializer, return null.
  ///
  virtual Init *convertInitializerBitRange(const std::vector<unsigned> &Bits) {
    return 0;
  }

  /// getFieldType - This method is used to implement the FieldInit class.
  /// Implementors of this method should return the type of the named field if
  /// they are of record type.
  ///
  virtual RecTy *getFieldType(const std::string &FieldName) const { return 0; }

  /// resolveReferences - This method is used by classes that refer to other
  /// variables which may not be defined at the time they expression is formed.
  /// If a value is set for the variable later, this method will be called on
  /// users of the value to allow the value to propagate out.
  ///
  virtual Init *resolveReferences(Record &R) { return this; }
};

inline std::ostream &operator<<(std::ostream &OS, const Init &I) {
  I.print(OS); return OS;
}


/// UnsetInit - ? - Represents an uninitialized value
///
struct UnsetInit : public Init {
  virtual Init *convertInitializerTo(RecTy *Ty) {
    return Ty->convertValue(this);
  }

  virtual bool isComplete() const { return false; }
  virtual void print(std::ostream &OS) const { OS << "?"; }
};


/// BitInit - true/false - Represent a concrete initializer for a bit.
///
class BitInit : public Init {
  bool Value;
public:
  BitInit(bool V) : Value(V) {}

  bool getValue() const { return Value; }

  virtual Init *convertInitializerTo(RecTy *Ty) {
    return Ty->convertValue(this);
  }

  virtual void print(std::ostream &OS) const { OS << (Value ? "1" : "0"); }
};

/// BitsInit - { a, b, c } - Represents an initializer for a BitsRecTy value.
/// It contains a vector of bits, whose size is determined by the type.
///
class BitsInit : public Init {
  std::vector<Init*> Bits;
public:
  BitsInit(unsigned Size) : Bits(Size) {}

  unsigned getNumBits() const { return Bits.size(); }

  Init *getBit(unsigned Bit) const {
    assert(Bit < Bits.size() && "Bit index out of range!");
    return Bits[Bit];
  }
  void setBit(unsigned Bit, Init *V) {
    assert(Bit < Bits.size() && "Bit index out of range!");
    Bits[Bit] = V;
  }

  virtual Init *convertInitializerTo(RecTy *Ty) {
    return Ty->convertValue(this);
  }
  virtual Init *convertInitializerBitRange(const std::vector<unsigned> &Bits);

  virtual bool isComplete() const {
    for (unsigned i = 0; i != getNumBits(); ++i)
      if (!getBit(i)->isComplete()) return false;
    return true;
  }
  virtual void print(std::ostream &OS) const;

  virtual Init *resolveReferences(Record &R);

  // printXX - Print this bitstream with the specified format, returning true if
  // it is not possible.
  bool printInHex(std::ostream &OS) const;
  bool printAsVariable(std::ostream &OS) const;
  bool printAsUnset(std::ostream &OS) const;
};


/// IntInit - 7 - Represent an initalization by a literal integer value.
///
class IntInit : public Init {
  int Value;
public:
  IntInit(int V) : Value(V) {}

  int getValue() const { return Value; }

  virtual Init *convertInitializerTo(RecTy *Ty) {
    return Ty->convertValue(this);
  }
  virtual Init *convertInitializerBitRange(const std::vector<unsigned> &Bits);

  virtual void print(std::ostream &OS) const { OS << Value; }
};


/// StringInit - "foo" - Represent an initialization by a string value.
///
class StringInit : public Init {
  std::string Value;
public:
  StringInit(const std::string &V) : Value(V) {}

  virtual Init *convertInitializerTo(RecTy *Ty) {
    return Ty->convertValue(this);
  }

  virtual void print(std::ostream &OS) const { OS << "\"" << Value << "\""; }
};

/// ListInit - [AL, AH, CL] - Represent a list of defs
///
class ListInit : public Init {
  std::vector<Record*> Records;
public:
  ListInit(std::vector<Record*> &Rs) {
    Records.swap(Rs);
  }

  unsigned getSize() const { return Records.size(); }
  Record  *getElement(unsigned i) const {
    assert(i < Records.size() && "List element index out of range!");
    return Records[i];
  }

  virtual Init *convertInitializerTo(RecTy *Ty) {
    return Ty->convertValue(this);
  }

  virtual void print(std::ostream &OS) const;
};


/// TypedInit - This is the common super-class of types that have a specific,
/// explicit, type.
///
class TypedInit : public Init {
  RecTy *Ty;
public:  
  TypedInit(RecTy *T) : Ty(T) {}

  RecTy *getType() const { return Ty; }

  /// resolveBitReference - This method is used to implement
  /// VarBitInit::resolveReferences.  If the bit is able to be resolved, we
  /// simply return the resolved value, otherwise we return this.
  ///
  virtual Init *resolveBitReference(Record &R, unsigned Bit) = 0;
};

/// VarInit - 'Opcode' - Represent a reference to an entire variable object.
///
class VarInit : public TypedInit {
  std::string VarName;
public:
  VarInit(const std::string &VN, RecTy *T) : TypedInit(T), VarName(VN) {}
  
  virtual Init *convertInitializerTo(RecTy *Ty) {
    return Ty->convertValue(this);
  }

  const std::string &getName() const { return VarName; }

  virtual Init *convertInitializerBitRange(const std::vector<unsigned> &Bits);

  virtual Init *resolveBitReference(Record &R, unsigned Bit);

  virtual RecTy *getFieldType(const std::string &FieldName) const;
  
  virtual void print(std::ostream &OS) const { OS << VarName; }
};


/// VarBitInit - Opcode{0} - Represent access to one bit of a variable or field.
///
class VarBitInit : public Init {
  TypedInit *TI;
  unsigned Bit;
public:
  VarBitInit(TypedInit *T, unsigned B) : TI(T), Bit(B) {
    assert(T->getType() && dynamic_cast<BitsRecTy*>(T->getType()) &&
           ((BitsRecTy*)T->getType())->getNumBits() > B &&
           "Illegal VarBitInit expression!");
  }

  virtual Init *convertInitializerTo(RecTy *Ty) {
    return Ty->convertValue(this);
  }

  TypedInit *getVariable() const { return TI; }
  unsigned getBitNum() const { return Bit; }
  
  virtual void print(std::ostream &OS) const {
    TI->print(OS); OS << "{" << Bit << "}";
  }
  virtual Init *resolveReferences(Record &R);
};


/// DefInit - AL - Represent a reference to a 'def' in the description
///
class DefInit : public Init {
  Record *Def;
public:
  DefInit(Record *D) : Def(D) {}
  
  virtual Init *convertInitializerTo(RecTy *Ty) {
    return Ty->convertValue(this);
  }

  Record *getDef() const { return Def; }

  //virtual Init *convertInitializerBitRange(const std::vector<unsigned> &Bits);
  
  virtual void print(std::ostream &OS) const;
};


/// FieldInit - X.Y - Represent a reference to a subfield of a variable
///
class FieldInit : public TypedInit {
  Init *Rec;                // Record we are referring to
  std::string FieldName;    // Field we are accessing
public:
  FieldInit(Init *R, const std::string &FN)
    : TypedInit(R->getFieldType(FN)), Rec(R), FieldName(FN) {
    assert(getType() && "FieldInit with non-record type!");
  }

  virtual Init *convertInitializerTo(RecTy *Ty) {
    return Ty->convertValue(this);
  }

  virtual Init *convertInitializerBitRange(const std::vector<unsigned> &Bits);

  virtual Init *resolveBitReference(Record &R, unsigned Bit);

  virtual void print(std::ostream &OS) const {
    Rec->print(OS); OS << "." << FieldName;
  }
};


//===----------------------------------------------------------------------===//
//  High-Level Classes
//===----------------------------------------------------------------------===//

class RecordVal {
  std::string Name;
  RecTy *Ty;
  unsigned Prefix;
  Init *Value;
public:
  RecordVal(const std::string &N, RecTy *T, unsigned P);

  const std::string &getName() const { return Name; }

  unsigned getPrefix() const { return Prefix; }
  RecTy *getType() const { return Ty; }
  Init *getValue() const { return Value; }

  bool setValue(Init *V) {
    if (V) {
      Value = V->convertInitializerTo(Ty);
      return Value == 0;
    }
    Value = 0;
    return false;
  }

  void dump() const;
  void print(std::ostream &OS, bool PrintSem = true) const;
};

inline std::ostream &operator<<(std::ostream &OS, const RecordVal &RV) {
  RV.print(OS << "  ");
  return OS;
}

struct Record {
  const std::string Name;
  std::vector<std::string> TemplateArgs;
  std::vector<RecordVal> Values;
  std::vector<Record*> SuperClasses;
public:

  Record(const std::string &N) : Name(N) {}
  ~Record() {}

  const std::string &getName() const { return Name; }
  const std::vector<std::string> &getTemplateArgs() const {
    return TemplateArgs;
  }
  const std::vector<RecordVal> &getValues() const { return Values; }
  const std::vector<Record*>   &getSuperClasses() const { return SuperClasses; }

  bool isTemplateArg(const std::string &Name) const {
    for (unsigned i = 0, e = TemplateArgs.size(); i != e; ++i)
      if (TemplateArgs[i] == Name) return true;
    return false;
  }

  const RecordVal *getValue(const std::string &Name) const {
    for (unsigned i = 0, e = Values.size(); i != e; ++i)
      if (Values[i].getName() == Name) return &Values[i];
    return 0;
  }
  RecordVal *getValue(const std::string &Name) {
    for (unsigned i = 0, e = Values.size(); i != e; ++i)
      if (Values[i].getName() == Name) return &Values[i];
    return 0;
  }

  void addTemplateArg(const std::string &Name) {
    assert(!isTemplateArg(Name) && "Template arg already defined!");
    TemplateArgs.push_back(Name);
  }

  void addValue(const RecordVal &RV) {
    assert(getValue(RV.getName()) == 0 && "Value already added!");
    Values.push_back(RV);
  }

  bool isSubClassOf(Record *R) const {
    for (unsigned i = 0, e = SuperClasses.size(); i != e; ++i)
      if (SuperClasses[i] == R)
	return true;
    return false;
  }

  void addSuperClass(Record *R) {
    assert(!isSubClassOf(R) && "Already subclassing record!");
    SuperClasses.push_back(R);
  }

  // resolveReferences - If there are any field references that refer to fields
  // that have been filled in, we can propagate the values now.
  //
  void resolveReferences();

  void dump() const;
};

std::ostream &operator<<(std::ostream &OS, const Record &R);

class RecordKeeper {
  std::map<std::string, Record*> Classes, Defs;
public:
  ~RecordKeeper() {
    for (std::map<std::string, Record*>::iterator I = Classes.begin(),
	   E = Classes.end(); I != E; ++I)
      delete I->second;
    for (std::map<std::string, Record*>::iterator I = Defs.begin(),
	   E = Defs.end(); I != E; ++I)
      delete I->second;
  }
  
  const std::map<std::string, Record*> &getClasses() const { return Classes; }
  const std::map<std::string, Record*> &getDefs() const { return Defs; }

  Record *getClass(const std::string &Name) const {
    std::map<std::string, Record*>::const_iterator I = Classes.find(Name);
    return I == Classes.end() ? 0 : I->second;
  }
  Record *getDef(const std::string &Name) const {
    std::map<std::string, Record*>::const_iterator I = Defs.find(Name);
    return I == Defs.end() ? 0 : I->second;
  }
  void addClass(Record *R) {
    assert(getClass(R->getName()) == 0 && "Class already exists!");
    Classes.insert(std::make_pair(R->getName(), R));
  }
  void addDef(Record *R) {
    assert(getDef(R->getName()) == 0 && "Def already exists!");
    Defs.insert(std::make_pair(R->getName(), R));
  }

  void dump() const;
};

std::ostream &operator<<(std::ostream &OS, const RecordKeeper &RK);

extern RecordKeeper Records;

#endif
