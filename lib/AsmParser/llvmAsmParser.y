//===-- llvmAsmParser.y - Parser for llvm assembly files ---------*- C++ -*--=//
//
//  This file implements the bison parser for LLVM assembly languages files.
//
//===------------------------------------------------------------------------=//

%{
#include "ParserInternals.h"
#include "llvm/Assembly/Parser.h"
#include "llvm/SymbolTable.h"
#include "llvm/Module.h"
#include "llvm/GlobalVariable.h"
#include "llvm/Function.h"
#include "llvm/BasicBlock.h"
#include "llvm/DerivedTypes.h"
#include "llvm/iTerminators.h"
#include "llvm/iMemory.h"
#include "llvm/iPHINode.h"
#include "llvm/Argument.h"
#include "Support/STLExtras.h"
#include "Support/DepthFirstIterator.h"
#include <list>
#include <utility>            // Get definition of pair class
#include <algorithm>
#include <stdio.h>            // This embarasment is due to our flex lexer...
#include <iostream>
using std::list;
using std::vector;
using std::pair;
using std::map;
using std::pair;
using std::make_pair;
using std::cerr;
using std::string;

int yyerror(const char *ErrorMsg); // Forward declarations to prevent "implicit
int yylex();                       // declaration" of xxx warnings.
int yyparse();

static Module *ParserResult;
string CurFilename;

// DEBUG_UPREFS - Define this symbol if you want to enable debugging output
// relating to upreferences in the input stream.
//
//#define DEBUG_UPREFS 1
#ifdef DEBUG_UPREFS
#define UR_OUT(X) cerr << X
#else
#define UR_OUT(X)
#endif

// This contains info used when building the body of a method.  It is destroyed
// when the method is completed.
//
typedef vector<Value *> ValueList;           // Numbered defs
static void ResolveDefinitions(vector<ValueList> &LateResolvers,
                               vector<ValueList> *FutureLateResolvers = 0);

static struct PerModuleInfo {
  Module *CurrentModule;
  vector<ValueList>    Values;     // Module level numbered definitions
  vector<ValueList>    LateResolveValues;
  vector<PATypeHolder> Types;
  map<ValID, PATypeHolder> LateResolveTypes;

  // GlobalRefs - This maintains a mapping between <Type, ValID>'s and forward
  // references to global values.  Global values may be referenced before they
  // are defined, and if so, the temporary object that they represent is held
  // here.  This is used for forward references of ConstantPointerRefs.
  //
  typedef map<pair<const PointerType *, ValID>, GlobalVariable*> GlobalRefsType;
  GlobalRefsType GlobalRefs;

  void ModuleDone() {
    // If we could not resolve some methods at method compilation time (calls to
    // methods before they are defined), resolve them now...  Types are resolved
    // when the constant pool has been completely parsed.
    //
    ResolveDefinitions(LateResolveValues);

    // Check to make sure that all global value forward references have been
    // resolved!
    //
    if (!GlobalRefs.empty()) {
      string UndefinedReferences = "Unresolved global references exist:\n";
      
      for (GlobalRefsType::iterator I = GlobalRefs.begin(), E =GlobalRefs.end();
           I != E; ++I) {
        UndefinedReferences += "  " + I->first.first->getDescription() + " " +
                               I->first.second.getName() + "\n";
      }
      ThrowException(UndefinedReferences);
    }

    Values.clear();         // Clear out method local definitions
    Types.clear();
    CurrentModule = 0;
  }


  // DeclareNewGlobalValue - Called every type a new GV has been defined.  This
  // is used to remove things from the forward declaration map, resolving them
  // to the correct thing as needed.
  //
  void DeclareNewGlobalValue(GlobalValue *GV, ValID D) {
    // Check to see if there is a forward reference to this global variable...
    // if there is, eliminate it and patch the reference to use the new def'n.
    GlobalRefsType::iterator I = GlobalRefs.find(make_pair(GV->getType(), D));

    if (I != GlobalRefs.end()) {
      GlobalVariable *OldGV = I->second;   // Get the placeholder...
      I->first.second.destroy();  // Free string memory if neccesary
      
      // Loop over all of the uses of the GlobalValue.  The only thing they are
      // allowed to be at this point is ConstantPointerRef's.
      assert(OldGV->use_size() == 1 && "Only one reference should exist!");
      while (!OldGV->use_empty()) {
	User *U = OldGV->use_back();  // Must be a ConstantPointerRef...
	ConstantPointerRef *CPPR = cast<ConstantPointerRef>(U);
	assert(CPPR->getValue() == OldGV && "Something isn't happy");
	
	// Change the const pool reference to point to the real global variable
	// now.  This should drop a use from the OldGV.
	CPPR->mutateReference(GV);
      }
    
      // Remove GV from the module...
      CurrentModule->getGlobalList().remove(OldGV);
      delete OldGV;                        // Delete the old placeholder

      // Remove the map entry for the global now that it has been created...
      GlobalRefs.erase(I);
    }
  }

} CurModule;

static struct PerFunctionInfo {
  Function *CurrentFunction;     // Pointer to current method being created

  vector<ValueList> Values;      // Keep track of numbered definitions
  vector<ValueList> LateResolveValues;
  vector<PATypeHolder> Types;
  map<ValID, PATypeHolder> LateResolveTypes;
  bool isDeclare;                // Is this method a forward declararation?

  inline PerFunctionInfo() {
    CurrentFunction = 0;
    isDeclare = false;
  }

  inline ~PerFunctionInfo() {}

  inline void FunctionStart(Function *M) {
    CurrentFunction = M;
  }

  void FunctionDone() {
    // If we could not resolve some blocks at parsing time (forward branches)
    // resolve the branches now...
    ResolveDefinitions(LateResolveValues, &CurModule.LateResolveValues);

    Values.clear();         // Clear out method local definitions
    Types.clear();
    CurrentFunction = 0;
    isDeclare = false;
  }
} CurMeth;  // Info for the current method...

static bool inFunctionScope() { return CurMeth.CurrentFunction != 0; }


//===----------------------------------------------------------------------===//
//               Code to handle definitions of all the types
//===----------------------------------------------------------------------===//

static int InsertValue(Value *D, vector<ValueList> &ValueTab = CurMeth.Values) {
  if (D->hasName()) return -1;           // Is this a numbered definition?

  // Yes, insert the value into the value table...
  unsigned type = D->getType()->getUniqueID();
  if (ValueTab.size() <= type)
    ValueTab.resize(type+1, ValueList());
  //printf("Values[%d][%d] = %d\n", type, ValueTab[type].size(), D);
  ValueTab[type].push_back(D);
  return ValueTab[type].size()-1;
}

// TODO: FIXME when Type are not const
static void InsertType(const Type *Ty, vector<PATypeHolder> &Types) {
  Types.push_back(Ty);
}

static const Type *getTypeVal(const ValID &D, bool DoNotImprovise = false) {
  switch (D.Type) {
  case 0: {                 // Is it a numbered definition?
    unsigned Num = (unsigned)D.Num;

    // Module constants occupy the lowest numbered slots...
    if (Num < CurModule.Types.size()) 
      return CurModule.Types[Num];

    Num -= CurModule.Types.size();

    // Check that the number is within bounds...
    if (Num <= CurMeth.Types.size())
      return CurMeth.Types[Num];
    break;
  }
  case 1: {                // Is it a named definition?
    string Name(D.Name);
    SymbolTable *SymTab = 0;
    if (inFunctionScope()) SymTab = CurMeth.CurrentFunction->getSymbolTable();
    Value *N = SymTab ? SymTab->lookup(Type::TypeTy, Name) : 0;

    if (N == 0) {
      // Symbol table doesn't automatically chain yet... because the method
      // hasn't been added to the module...
      //
      SymTab = CurModule.CurrentModule->getSymbolTable();
      if (SymTab)
        N = SymTab->lookup(Type::TypeTy, Name);
      if (N == 0) break;
    }

    D.destroy();  // Free old strdup'd memory...
    return cast<const Type>(N);
  }
  default:
    ThrowException("Invalid symbol type reference!");
  }

  // If we reached here, we referenced either a symbol that we don't know about
  // or an id number that hasn't been read yet.  We may be referencing something
  // forward, so just create an entry to be resolved later and get to it...
  //
  if (DoNotImprovise) return 0;  // Do we just want a null to be returned?

  map<ValID, PATypeHolder> &LateResolver = inFunctionScope() ? 
    CurMeth.LateResolveTypes : CurModule.LateResolveTypes;
  
  map<ValID, PATypeHolder>::iterator I = LateResolver.find(D);
  if (I != LateResolver.end()) {
    return I->second;
  }

  Type *Typ = OpaqueType::get();
  LateResolver.insert(make_pair(D, Typ));
  return Typ;
}

static Value *lookupInSymbolTable(const Type *Ty, const string &Name) {
  SymbolTable *SymTab = 
    inFunctionScope() ? CurMeth.CurrentFunction->getSymbolTable() : 0;
  Value *N = SymTab ? SymTab->lookup(Ty, Name) : 0;

  if (N == 0) {
    // Symbol table doesn't automatically chain yet... because the method
    // hasn't been added to the module...
    //
    SymTab = CurModule.CurrentModule->getSymbolTable();
    if (SymTab)
      N = SymTab->lookup(Ty, Name);
  }

  return N;
}

// getValNonImprovising - Look up the value specified by the provided type and
// the provided ValID.  If the value exists and has already been defined, return
// it.  Otherwise return null.
//
static Value *getValNonImprovising(const Type *Ty, const ValID &D) {
  if (isa<FunctionType>(Ty))
    ThrowException("Functions are not values and "
                   "must be referenced as pointers");

  switch (D.Type) {
  case ValID::NumberVal: {                 // Is it a numbered definition?
    unsigned type = Ty->getUniqueID();
    unsigned Num = (unsigned)D.Num;

    // Module constants occupy the lowest numbered slots...
    if (type < CurModule.Values.size()) {
      if (Num < CurModule.Values[type].size()) 
        return CurModule.Values[type][Num];

      Num -= CurModule.Values[type].size();
    }

    // Make sure that our type is within bounds
    if (CurMeth.Values.size() <= type) return 0;

    // Check that the number is within bounds...
    if (CurMeth.Values[type].size() <= Num) return 0;
  
    return CurMeth.Values[type][Num];
  }

  case ValID::NameVal: {                // Is it a named definition?
    Value *N = lookupInSymbolTable(Ty, string(D.Name));
    if (N == 0) return 0;

    D.destroy();  // Free old strdup'd memory...
    return N;
  }

  // Check to make sure that "Ty" is an integral type, and that our 
  // value will fit into the specified type...
  case ValID::ConstSIntVal:    // Is it a constant pool reference??
    if (Ty == Type::BoolTy) {  // Special handling for boolean data
      return ConstantBool::get(D.ConstPool64 != 0);
    } else {
      if (!ConstantSInt::isValueValidForType(Ty, D.ConstPool64))
	ThrowException("Symbolic constant pool value '" +
		       itostr(D.ConstPool64) + "' is invalid for type '" + 
		       Ty->getDescription() + "'!");
      return ConstantSInt::get(Ty, D.ConstPool64);
    }

  case ValID::ConstUIntVal:     // Is it an unsigned const pool reference?
    if (!ConstantUInt::isValueValidForType(Ty, D.UConstPool64)) {
      if (!ConstantSInt::isValueValidForType(Ty, D.ConstPool64)) {
	ThrowException("Integral constant pool reference is invalid!");
      } else {     // This is really a signed reference.  Transmogrify.
	return ConstantSInt::get(Ty, D.ConstPool64);
      }
    } else {
      return ConstantUInt::get(Ty, D.UConstPool64);
    }

  case ValID::ConstFPVal:        // Is it a floating point const pool reference?
    if (!ConstantFP::isValueValidForType(Ty, D.ConstPoolFP))
      ThrowException("FP constant invalid for type!!");
    return ConstantFP::get(Ty, D.ConstPoolFP);
    
  case ValID::ConstNullVal:      // Is it a null value?
    if (!Ty->isPointerType())
      ThrowException("Cannot create a a non pointer null!");
    return ConstantPointerNull::get(cast<PointerType>(Ty));
    
  default:
    assert(0 && "Unhandled case!");
    return 0;
  }   // End of switch

  assert(0 && "Unhandled case!");
  return 0;
}


// getVal - This function is identical to getValNonImprovising, except that if a
// value is not already defined, it "improvises" by creating a placeholder var
// that looks and acts just like the requested variable.  When the value is
// defined later, all uses of the placeholder variable are replaced with the
// real thing.
//
static Value *getVal(const Type *Ty, const ValID &D) {
  assert(Ty != Type::TypeTy && "Should use getTypeVal for types!");

  // See if the value has already been defined...
  Value *V = getValNonImprovising(Ty, D);
  if (V) return V;

  // If we reached here, we referenced either a symbol that we don't know about
  // or an id number that hasn't been read yet.  We may be referencing something
  // forward, so just create an entry to be resolved later and get to it...
  //
  Value *d = 0;
  switch (Ty->getPrimitiveID()) {
  case Type::LabelTyID:  d = new   BBPlaceHolder(Ty, D); break;
  default:               d = new ValuePlaceHolder(Ty, D); break;
  }

  assert(d != 0 && "How did we not make something?");
  if (inFunctionScope())
    InsertValue(d, CurMeth.LateResolveValues);
  else 
    InsertValue(d, CurModule.LateResolveValues);
  return d;
}


//===----------------------------------------------------------------------===//
//              Code to handle forward references in instructions
//===----------------------------------------------------------------------===//
//
// This code handles the late binding needed with statements that reference
// values not defined yet... for example, a forward branch, or the PHI node for
// a loop body.
//
// This keeps a table (CurMeth.LateResolveValues) of all such forward references
// and back patchs after we are done.
//

// ResolveDefinitions - If we could not resolve some defs at parsing 
// time (forward branches, phi functions for loops, etc...) resolve the 
// defs now...
//
static void ResolveDefinitions(vector<ValueList> &LateResolvers,
                               vector<ValueList> *FutureLateResolvers = 0) {
  // Loop over LateResolveDefs fixing up stuff that couldn't be resolved
  for (unsigned ty = 0; ty < LateResolvers.size(); ty++) {
    while (!LateResolvers[ty].empty()) {
      Value *V = LateResolvers[ty].back();
      assert(!isa<Type>(V) && "Types should be in LateResolveTypes!");

      LateResolvers[ty].pop_back();
      ValID &DID = getValIDFromPlaceHolder(V);

      Value *TheRealValue = getValNonImprovising(Type::getUniqueIDType(ty),DID);
      if (TheRealValue) {
        V->replaceAllUsesWith(TheRealValue);
        delete V;
      } else if (FutureLateResolvers) {
        // Functions have their unresolved items forwarded to the module late
        // resolver table
        InsertValue(V, *FutureLateResolvers);
      } else {
	if (DID.Type == 1)
	  ThrowException("Reference to an invalid definition: '" +DID.getName()+
			 "' of type '" + V->getType()->getDescription() + "'",
			 getLineNumFromPlaceHolder(V));
	else
	  ThrowException("Reference to an invalid definition: #" +
			 itostr(DID.Num) + " of type '" + 
			 V->getType()->getDescription() + "'",
			 getLineNumFromPlaceHolder(V));
      }
    }
  }

  LateResolvers.clear();
}

// ResolveTypeTo - A brand new type was just declared.  This means that (if
// name is not null) things referencing Name can be resolved.  Otherwise, things
// refering to the number can be resolved.  Do this now.
//
static void ResolveTypeTo(char *Name, const Type *ToTy) {
  vector<PATypeHolder> &Types = inFunctionScope() ? 
     CurMeth.Types : CurModule.Types;

   ValID D;
   if (Name) D = ValID::create(Name);
   else      D = ValID::create((int)Types.size());

   map<ValID, PATypeHolder> &LateResolver = inFunctionScope() ? 
     CurMeth.LateResolveTypes : CurModule.LateResolveTypes;
  
   map<ValID, PATypeHolder>::iterator I = LateResolver.find(D);
   if (I != LateResolver.end()) {
     cast<DerivedType>(I->second.get())->refineAbstractTypeTo(ToTy);
     LateResolver.erase(I);
   }
}

// ResolveTypes - At this point, all types should be resolved.  Any that aren't
// are errors.
//
static void ResolveTypes(map<ValID, PATypeHolder> &LateResolveTypes) {
  if (!LateResolveTypes.empty()) {
    const ValID &DID = LateResolveTypes.begin()->first;

    if (DID.Type == ValID::NameVal)
      ThrowException("Reference to an invalid type: '" +DID.getName() + "'");
    else
      ThrowException("Reference to an invalid type: #" + itostr(DID.Num));
  }
}


// setValueName - Set the specified value to the name given.  The name may be
// null potentially, in which case this is a noop.  The string passed in is
// assumed to be a malloc'd string buffer, and is freed by this function.
//
// This function returns true if the value has already been defined, but is
// allowed to be redefined in the specified context.  If the name is a new name
// for the typeplane, false is returned.
//
static bool setValueName(Value *V, char *NameStr) {
  if (NameStr == 0) return false;
  
  string Name(NameStr);           // Copy string
  free(NameStr);                  // Free old string

  if (V->getType() == Type::VoidTy) 
    ThrowException("Can't assign name '" + Name + 
		   "' to a null valued instruction!");

  SymbolTable *ST = inFunctionScope() ? 
    CurMeth.CurrentFunction->getSymbolTableSure() : 
    CurModule.CurrentModule->getSymbolTableSure();

  Value *Existing = ST->lookup(V->getType(), Name);
  if (Existing) {    // Inserting a name that is already defined???
    // There is only one case where this is allowed: when we are refining an
    // opaque type.  In this case, Existing will be an opaque type.
    if (const Type *Ty = dyn_cast<const Type>(Existing)) {
      if (OpaqueType *OpTy = dyn_cast<OpaqueType>(Ty)) {
	// We ARE replacing an opaque type!
	OpTy->refineAbstractTypeTo(cast<Type>(V));
	return true;
      }
    }

    // Otherwise, we are a simple redefinition of a value, check to see if it
    // is defined the same as the old one...
    if (const Type *Ty = dyn_cast<const Type>(Existing)) {
      if (Ty == cast<const Type>(V)) return true;  // Yes, it's equal.
      // cerr << "Type: " << Ty->getDescription() << " != "
      //      << cast<const Type>(V)->getDescription() << "!\n";
    } else if (GlobalVariable *EGV = dyn_cast<GlobalVariable>(Existing)) {
      // We are allowed to redefine a global variable in two circumstances:
      // 1. If at least one of the globals is uninitialized or 
      // 2. If both initializers have the same value.
      //
      // This can only be done if the const'ness of the vars is the same.
      //
      if (GlobalVariable *GV = dyn_cast<GlobalVariable>(V)) {
        if (EGV->isConstant() == GV->isConstant() &&
            (!EGV->hasInitializer() || !GV->hasInitializer() ||
             EGV->getInitializer() == GV->getInitializer())) {

          // Make sure the existing global version gets the initializer!
          if (GV->hasInitializer() && !EGV->hasInitializer())
            EGV->setInitializer(GV->getInitializer());
          
	  delete GV;     // Destroy the duplicate!
          return true;   // They are equivalent!
        }
      }
    }
    ThrowException("Redefinition of value named '" + Name + "' in the '" +
		   V->getType()->getDescription() + "' type plane!");
  }

  V->setName(Name, ST);
  return false;
}


//===----------------------------------------------------------------------===//
// Code for handling upreferences in type names...
//

// TypeContains - Returns true if Ty contains E in it.
//
static bool TypeContains(const Type *Ty, const Type *E) {
  return find(df_begin(Ty), df_end(Ty), E) != df_end(Ty);
}


static vector<pair<unsigned, OpaqueType *> > UpRefs;

static PATypeHolder HandleUpRefs(const Type *ty) {
  PATypeHolder Ty(ty);
  UR_OUT("Type '" << ty->getDescription() << 
         "' newly formed.  Resolving upreferences.\n" <<
         UpRefs.size() << " upreferences active!\n");
  for (unsigned i = 0; i < UpRefs.size(); ) {
    UR_OUT("  UR#" << i << " - TypeContains(" << Ty->getDescription() << ", " 
	   << UpRefs[i].second->getDescription() << ") = " 
	   << (TypeContains(Ty, UpRefs[i].second) ? "true" : "false") << endl);
    if (TypeContains(Ty, UpRefs[i].second)) {
      unsigned Level = --UpRefs[i].first;   // Decrement level of upreference
      UR_OUT("  Uplevel Ref Level = " << Level << endl);
      if (Level == 0) {                     // Upreference should be resolved! 
	UR_OUT("  * Resolving upreference for "
               << UpRefs[i].second->getDescription() << endl;
	       string OldName = UpRefs[i].second->getDescription());
	UpRefs[i].second->refineAbstractTypeTo(Ty);
	UpRefs.erase(UpRefs.begin()+i);     // Remove from upreference list...
	UR_OUT("  * Type '" << OldName << "' refined upreference to: "
	       << (const void*)Ty << ", " << Ty->getDescription() << endl);
	continue;
      }
    }

    ++i;                                  // Otherwise, no resolve, move on...
  }
  // FIXME: TODO: this should return the updated type
  return Ty;
}


//===----------------------------------------------------------------------===//
//            RunVMAsmParser - Define an interface to this parser
//===----------------------------------------------------------------------===//
//
Module *RunVMAsmParser(const string &Filename, FILE *F) {
  llvmAsmin = F;
  CurFilename = Filename;
  llvmAsmlineno = 1;      // Reset the current line number...

  CurModule.CurrentModule = new Module();  // Allocate a new module to read
  yyparse();       // Parse the file.
  Module *Result = ParserResult;
  llvmAsmin = stdin;    // F is about to go away, don't use it anymore...
  ParserResult = 0;

  return Result;
}

%}

%union {
  Module                           *ModuleVal;
  Function                         *FunctionVal;
  std::pair<Argument*, char*>      *ArgVal;
  BasicBlock                       *BasicBlockVal;
  TerminatorInst                   *TermInstVal;
  Instruction                      *InstVal;
  Constant                         *ConstVal;

  const Type                       *PrimType;
  PATypeHolder                     *TypeVal;
  Value                            *ValueVal;

  std::list<std::pair<Argument*,char*> > *ArgList;
  std::vector<Value*>              *ValueList;
  std::list<PATypeHolder>          *TypeList;
  std::list<std::pair<Value*,
                      BasicBlock*> > *PHIList; // Represent the RHS of PHI node
  std::vector<std::pair<Constant*, BasicBlock*> > *JumpTable;
  std::vector<Constant*>           *ConstVector;

  int64_t                           SInt64Val;
  uint64_t                          UInt64Val;
  int                               SIntVal;
  unsigned                          UIntVal;
  double                            FPVal;
  bool                              BoolVal;

  char                             *StrVal;   // This memory is strdup'd!
  ValID                             ValIDVal; // strdup'd memory maybe!

  Instruction::UnaryOps             UnaryOpVal;
  Instruction::BinaryOps            BinaryOpVal;
  Instruction::TermOps              TermOpVal;
  Instruction::MemoryOps            MemOpVal;
  Instruction::OtherOps             OtherOpVal;
}

%type <ModuleVal>     Module FunctionList
%type <FunctionVal>   Function FunctionProto FunctionHeader BasicBlockList
%type <BasicBlockVal> BasicBlock InstructionList
%type <TermInstVal>   BBTerminatorInst
%type <InstVal>       Inst InstVal MemoryInst
%type <ConstVal>      ConstVal
%type <ConstVector>   ConstVector
%type <ArgList>       ArgList ArgListH
%type <ArgVal>        ArgVal
%type <PHIList>       PHIList
%type <ValueList>     ValueRefList ValueRefListE  // For call param lists
%type <ValueList>     IndexList                   // For GEP derived indices
%type <TypeList>      TypeListI ArgTypeListI
%type <JumpTable>     JumpTable
%type <BoolVal>       GlobalType OptInternal      // GLOBAL or CONSTANT? Intern?

// ValueRef - Unresolved reference to a definition or BB
%type <ValIDVal>      ValueRef ConstValueRef SymbolicValueRef
%type <ValueVal>      ResolvedVal            // <type> <valref> pair
// Tokens and types for handling constant integer values
//
// ESINT64VAL - A negative number within long long range
%token <SInt64Val> ESINT64VAL

// EUINT64VAL - A positive number within uns. long long range
%token <UInt64Val> EUINT64VAL
%type  <SInt64Val> EINT64VAL

%token  <SIntVal>   SINTVAL   // Signed 32 bit ints...
%token  <UIntVal>   UINTVAL   // Unsigned 32 bit ints...
%type   <SIntVal>   INTVAL
%token  <FPVal>     FPVAL     // Float or Double constant

// Built in types...
%type  <TypeVal> Types TypesV UpRTypes UpRTypesV
%type  <PrimType> SIntType UIntType IntType FPType PrimType   // Classifications
%token <PrimType> VOID BOOL SBYTE UBYTE SHORT USHORT INT UINT LONG ULONG
%token <PrimType> FLOAT DOUBLE TYPE LABEL

%token <StrVal>     VAR_ID LABELSTR STRINGCONSTANT
%type  <StrVal>  OptVAR_ID OptAssign


%token IMPLEMENTATION TRUE FALSE BEGINTOK END DECLARE GLOBAL CONSTANT UNINIT
%token TO EXCEPT DOTDOTDOT STRING NULL_TOK CONST INTERNAL OPAQUE

// Basic Block Terminating Operators 
%token <TermOpVal> RET BR SWITCH

// Unary Operators 
%type  <UnaryOpVal> UnaryOps  // all the unary operators
%token <UnaryOpVal> NOT

// Binary Operators 
%type  <BinaryOpVal> BinaryOps  // all the binary operators
%token <BinaryOpVal> ADD SUB MUL DIV REM AND OR XOR
%token <BinaryOpVal> SETLE SETGE SETLT SETGT SETEQ SETNE  // Binary Comarators

// Memory Instructions
%token <MemoryOpVal> MALLOC ALLOCA FREE LOAD STORE GETELEMENTPTR

// Other Operators
%type  <OtherOpVal> ShiftOps
%token <OtherOpVal> PHI CALL INVOKE CAST SHL SHR

%start Module
%%

// Handle constant integer size restriction and conversion...
//

INTVAL : SINTVAL
INTVAL : UINTVAL {
  if ($1 > (uint32_t)INT32_MAX)     // Outside of my range!
    ThrowException("Value too large for type!");
  $$ = (int32_t)$1;
}


EINT64VAL : ESINT64VAL       // These have same type and can't cause problems...
EINT64VAL : EUINT64VAL {
  if ($1 > (uint64_t)INT64_MAX)     // Outside of my range!
    ThrowException("Value too large for type!");
  $$ = (int64_t)$1;
}

// Operations that are notably excluded from this list include: 
// RET, BR, & SWITCH because they end basic blocks and are treated specially.
//
UnaryOps  : NOT
BinaryOps : ADD | SUB | MUL | DIV | REM | AND | OR | XOR
BinaryOps : SETLE | SETGE | SETLT | SETGT | SETEQ | SETNE
ShiftOps  : SHL | SHR

// These are some types that allow classification if we only want a particular 
// thing... for example, only a signed, unsigned, or integral type.
SIntType :  LONG |  INT |  SHORT | SBYTE
UIntType : ULONG | UINT | USHORT | UBYTE
IntType  : SIntType | UIntType
FPType   : FLOAT | DOUBLE

// OptAssign - Value producing statements have an optional assignment component
OptAssign : VAR_ID '=' {
    $$ = $1;
  }
  | /*empty*/ { 
    $$ = 0; 
  }

OptInternal : INTERNAL { $$ = true; } | /*empty*/ { $$ = false; }

//===----------------------------------------------------------------------===//
// Types includes all predefined types... except void, because it can only be
// used in specific contexts (method returning void for example).  To have
// access to it, a user must explicitly use TypesV.
//

// TypesV includes all of 'Types', but it also includes the void type.
TypesV    : Types    | VOID { $$ = new PATypeHolder($1); }
UpRTypesV : UpRTypes | VOID { $$ = new PATypeHolder($1); }

Types     : UpRTypes {
    if (UpRefs.size())
      ThrowException("Invalid upreference in type: " + (*$1)->getDescription());
    $$ = $1;
  }


// Derived types are added later...
//
PrimType : BOOL | SBYTE | UBYTE | SHORT  | USHORT | INT   | UINT 
PrimType : LONG | ULONG | FLOAT | DOUBLE | TYPE   | LABEL
UpRTypes : OPAQUE {
    $$ = new PATypeHolder(OpaqueType::get());
  }
  | PrimType {
    $$ = new PATypeHolder($1);
  }
UpRTypes : ValueRef {                    // Named types are also simple types...
  $$ = new PATypeHolder(getTypeVal($1));
}

// Include derived types in the Types production.
//
UpRTypes : '\\' EUINT64VAL {                   // Type UpReference
    if ($2 > (uint64_t)INT64_MAX) ThrowException("Value out of range!");
    OpaqueType *OT = OpaqueType::get();        // Use temporary placeholder
    UpRefs.push_back(make_pair((unsigned)$2, OT));  // Add to vector...
    $$ = new PATypeHolder(OT);
    UR_OUT("New Upreference!\n");
  }
  | UpRTypesV '(' ArgTypeListI ')' {           // Function derived type?
    vector<const Type*> Params;
    mapto($3->begin(), $3->end(), std::back_inserter(Params), 
	  std::mem_fun_ref(&PATypeHandle<Type>::get));
    bool isVarArg = Params.size() && Params.back() == Type::VoidTy;
    if (isVarArg) Params.pop_back();

    $$ = new PATypeHolder(HandleUpRefs(FunctionType::get(*$1,Params,isVarArg)));
    delete $3;      // Delete the argument list
    delete $1;      // Delete the old type handle
  }
  | '[' EUINT64VAL 'x' UpRTypes ']' {          // Sized array type?
    $$ = new PATypeHolder(HandleUpRefs(ArrayType::get(*$4, (unsigned)$2)));
    delete $4;
  }
  | '{' TypeListI '}' {                        // Structure type?
    vector<const Type*> Elements;
    mapto($2->begin(), $2->end(), std::back_inserter(Elements), 
	std::mem_fun_ref(&PATypeHandle<Type>::get));

    $$ = new PATypeHolder(HandleUpRefs(StructType::get(Elements)));
    delete $2;
  }
  | '{' '}' {                                  // Empty structure type?
    $$ = new PATypeHolder(StructType::get(vector<const Type*>()));
  }
  | UpRTypes '*' {                             // Pointer type?
    $$ = new PATypeHolder(HandleUpRefs(PointerType::get(*$1)));
    delete $1;
  }

// TypeList - Used for struct declarations and as a basis for method type 
// declaration type lists
//
TypeListI : UpRTypes {
    $$ = new list<PATypeHolder>();
    $$->push_back(*$1); delete $1;
  }
  | TypeListI ',' UpRTypes {
    ($$=$1)->push_back(*$3); delete $3;
  }

// ArgTypeList - List of types for a method type declaration...
ArgTypeListI : TypeListI
  | TypeListI ',' DOTDOTDOT {
    ($$=$1)->push_back(Type::VoidTy);
  }
  | DOTDOTDOT {
    ($$ = new list<PATypeHolder>())->push_back(Type::VoidTy);
  }
  | /*empty*/ {
    $$ = new list<PATypeHolder>();
  }


// ConstVal - The various declarations that go into the constant pool.  This
// includes all forward declarations of types, constants, and functions.
//
ConstVal: Types '[' ConstVector ']' { // Nonempty unsized arr
    const ArrayType *ATy = dyn_cast<const ArrayType>($1->get());
    if (ATy == 0)
      ThrowException("Cannot make array constant with type: '" + 
                     (*$1)->getDescription() + "'!");
    const Type *ETy = ATy->getElementType();
    int NumElements = ATy->getNumElements();

    // Verify that we have the correct size...
    if (NumElements != -1 && NumElements != (int)$3->size())
      ThrowException("Type mismatch: constant sized array initialized with " +
		     utostr($3->size()) +  " arguments, but has size of " + 
		     itostr(NumElements) + "!");

    // Verify all elements are correct type!
    for (unsigned i = 0; i < $3->size(); i++) {
      if (ETy != (*$3)[i]->getType())
	ThrowException("Element #" + utostr(i) + " is not of type '" + 
		       ETy->getDescription() +"' as required!\nIt is of type '"+
		       (*$3)[i]->getType()->getDescription() + "'.");
    }

    $$ = ConstantArray::get(ATy, *$3);
    delete $1; delete $3;
  }
  | Types '[' ']' {
    const ArrayType *ATy = dyn_cast<const ArrayType>($1->get());
    if (ATy == 0)
      ThrowException("Cannot make array constant with type: '" + 
                     (*$1)->getDescription() + "'!");

    int NumElements = ATy->getNumElements();
    if (NumElements != -1 && NumElements != 0) 
      ThrowException("Type mismatch: constant sized array initialized with 0"
		     " arguments, but has size of " + itostr(NumElements) +"!");
    $$ = ConstantArray::get(ATy, vector<Constant*>());
    delete $1;
  }
  | Types 'c' STRINGCONSTANT {
    const ArrayType *ATy = dyn_cast<const ArrayType>($1->get());
    if (ATy == 0)
      ThrowException("Cannot make array constant with type: '" + 
                     (*$1)->getDescription() + "'!");

    int NumElements = ATy->getNumElements();
    const Type *ETy = ATy->getElementType();
    char *EndStr = UnEscapeLexed($3, true);
    if (NumElements != -1 && NumElements != (EndStr-$3))
      ThrowException("Can't build string constant of size " + 
		     itostr((int)(EndStr-$3)) +
		     " when array has size " + itostr(NumElements) + "!");
    vector<Constant*> Vals;
    if (ETy == Type::SByteTy) {
      for (char *C = $3; C != EndStr; ++C)
	Vals.push_back(ConstantSInt::get(ETy, *C));
    } else if (ETy == Type::UByteTy) {
      for (char *C = $3; C != EndStr; ++C)
	Vals.push_back(ConstantUInt::get(ETy, *C));
    } else {
      free($3);
      ThrowException("Cannot build string arrays of non byte sized elements!");
    }
    free($3);
    $$ = ConstantArray::get(ATy, Vals);
    delete $1;
  }
  | Types '{' ConstVector '}' {
    const StructType *STy = dyn_cast<const StructType>($1->get());
    if (STy == 0)
      ThrowException("Cannot make struct constant with type: '" + 
                     (*$1)->getDescription() + "'!");
    // FIXME: TODO: Check to see that the constants are compatible with the type
    // initializer!
    $$ = ConstantStruct::get(STy, *$3);
    delete $1; delete $3;
  }
  | Types NULL_TOK {
    const PointerType *PTy = dyn_cast<const PointerType>($1->get());
    if (PTy == 0)
      ThrowException("Cannot make null pointer constant with type: '" + 
                     (*$1)->getDescription() + "'!");

    $$ = ConstantPointerNull::get(PTy);
    delete $1;
  }
  | Types SymbolicValueRef {
    const PointerType *Ty = dyn_cast<const PointerType>($1->get());
    if (Ty == 0)
      ThrowException("Global const reference must be a pointer type!");

    Value *V = getValNonImprovising(Ty, $2);

    // If this is an initializer for a constant pointer, which is referencing a
    // (currently) undefined variable, create a stub now that shall be replaced
    // in the future with the right type of variable.
    //
    if (V == 0) {
      assert(isa<PointerType>(Ty) && "Globals may only be used as pointers!");
      const PointerType *PT = cast<PointerType>(Ty);

      // First check to see if the forward references value is already created!
      PerModuleInfo::GlobalRefsType::iterator I =
	CurModule.GlobalRefs.find(make_pair(PT, $2));
    
      if (I != CurModule.GlobalRefs.end()) {
	V = I->second;             // Placeholder already exists, use it...
      } else {
	// TODO: Include line number info by creating a subclass of
	// TODO: GlobalVariable here that includes the said information!
	
	// Create a placeholder for the global variable reference...
	GlobalVariable *GV = new GlobalVariable(PT->getElementType(),
                                                false, true);
	// Keep track of the fact that we have a forward ref to recycle it
	CurModule.GlobalRefs.insert(make_pair(make_pair(PT, $2), GV));

	// Must temporarily push this value into the module table...
	CurModule.CurrentModule->getGlobalList().push_back(GV);
	V = GV;
      }
    }

    GlobalValue *GV = cast<GlobalValue>(V);
    $$ = ConstantPointerRef::get(GV);
    delete $1;            // Free the type handle
  }


ConstVal : SIntType EINT64VAL {     // integral constants
    if (!ConstantSInt::isValueValidForType($1, $2))
      ThrowException("Constant value doesn't fit in type!");
    $$ = ConstantSInt::get($1, $2);
  } 
  | UIntType EUINT64VAL {           // integral constants
    if (!ConstantUInt::isValueValidForType($1, $2))
      ThrowException("Constant value doesn't fit in type!");
    $$ = ConstantUInt::get($1, $2);
  } 
  | BOOL TRUE {                     // Boolean constants
    $$ = ConstantBool::True;
  }
  | BOOL FALSE {                    // Boolean constants
    $$ = ConstantBool::False;
  }
  | FPType FPVAL {                   // Float & Double constants
    $$ = ConstantFP::get($1, $2);
  }

// ConstVector - A list of comma seperated constants.
ConstVector : ConstVector ',' ConstVal {
    ($$ = $1)->push_back($3);
  }
  | ConstVal {
    $$ = new vector<Constant*>();
    $$->push_back($1);
  }


// GlobalType - Match either GLOBAL or CONSTANT for global declarations...
GlobalType : GLOBAL { $$ = false; } | CONSTANT { $$ = true; }


// ConstPool - Constants with optional names assigned to them.
ConstPool : ConstPool OptAssign CONST ConstVal { 
    if (setValueName($4, $2)) { assert(0 && "No redefinitions allowed!"); }
    InsertValue($4);
  }
  | ConstPool OptAssign TYPE TypesV {  // Types can be defined in the const pool
    // Eagerly resolve types.  This is not an optimization, this is a
    // requirement that is due to the fact that we could have this:
    //
    // %list = type { %list * }
    // %list = type { %list * }    ; repeated type decl
    //
    // If types are not resolved eagerly, then the two types will not be
    // determined to be the same type!
    //
    ResolveTypeTo($2, $4->get());

    // TODO: FIXME when Type are not const
    if (!setValueName(const_cast<Type*>($4->get()), $2)) {
      // If this is not a redefinition of a type...
      if (!$2) {
        InsertType($4->get(),
                   inFunctionScope() ? CurMeth.Types : CurModule.Types);
      }
    }

    delete $4;
  }
  | ConstPool FunctionProto {       // Function prototypes can be in const pool
  }
  | ConstPool OptAssign OptInternal GlobalType ConstVal {
    const Type *Ty = $5->getType();
    // Global declarations appear in Constant Pool
    Constant *Initializer = $5;
    if (Initializer == 0)
      ThrowException("Global value initializer is not a constant!");
	 
    GlobalVariable *GV = new GlobalVariable(Ty, $4, $3, Initializer);
    if (!setValueName(GV, $2)) {   // If not redefining...
      CurModule.CurrentModule->getGlobalList().push_back(GV);
      int Slot = InsertValue(GV, CurModule.Values);

      if (Slot != -1) {
	CurModule.DeclareNewGlobalValue(GV, ValID::create(Slot));
      } else {
	CurModule.DeclareNewGlobalValue(GV, ValID::create(
				                (char*)GV->getName().c_str()));
      }
    }
  }
  | ConstPool OptAssign OptInternal UNINIT GlobalType Types {
    const Type *Ty = *$6;
    // Global declarations appear in Constant Pool
    GlobalVariable *GV = new GlobalVariable(Ty, $5, $3);
    if (!setValueName(GV, $2)) {   // If not redefining...
      CurModule.CurrentModule->getGlobalList().push_back(GV);
      int Slot = InsertValue(GV, CurModule.Values);

      if (Slot != -1) {
	CurModule.DeclareNewGlobalValue(GV, ValID::create(Slot));
      } else {
	assert(GV->hasName() && "Not named and not numbered!?");
	CurModule.DeclareNewGlobalValue(GV, ValID::create(
				                (char*)GV->getName().c_str()));
      }
    }
    delete $6;
  }
  | /* empty: end of list */ { 
  }


//===----------------------------------------------------------------------===//
//                             Rules to match Modules
//===----------------------------------------------------------------------===//

// Module rule: Capture the result of parsing the whole file into a result
// variable...
//
Module : FunctionList {
  $$ = ParserResult = $1;
  CurModule.ModuleDone();
}

// FunctionList - A list of methods, preceeded by a constant pool.
//
FunctionList : FunctionList Function {
    $$ = $1;
    assert($2->getParent() == 0 && "Function already in module!");
    $1->getFunctionList().push_back($2);
    CurMeth.FunctionDone();
  } 
  | FunctionList FunctionProto {
    $$ = $1;
  }
  | ConstPool IMPLEMENTATION {
    $$ = CurModule.CurrentModule;
    // Resolve circular types before we parse the body of the module
    ResolveTypes(CurModule.LateResolveTypes);
  }


//===----------------------------------------------------------------------===//
//                       Rules to match Function Headers
//===----------------------------------------------------------------------===//

OptVAR_ID : VAR_ID | /*empty*/ { $$ = 0; }

ArgVal : Types OptVAR_ID {
  $$ = new pair<Argument*, char*>(new Argument(*$1), $2);
  delete $1;  // Delete the type handle..
}

ArgListH : ArgVal ',' ArgListH {
    $$ = $3;
    $3->push_front(*$1);
    delete $1;
  }
  | ArgVal {
    $$ = new list<pair<Argument*,char*> >();
    $$->push_front(*$1);
    delete $1;
  }
  | DOTDOTDOT {
    $$ = new list<pair<Argument*, char*> >();
    $$->push_front(pair<Argument*,char*>(new Argument(Type::VoidTy), 0));
  }

ArgList : ArgListH {
    $$ = $1;
  }
  | /* empty */ {
    $$ = 0;
  }

FunctionHeaderH : OptInternal TypesV STRINGCONSTANT '(' ArgList ')' {
  UnEscapeLexed($3);
  string FunctionName($3);
  
  vector<const Type*> ParamTypeList;
  if ($5)
    for (list<pair<Argument*,char*> >::iterator I = $5->begin();
         I != $5->end(); ++I)
      ParamTypeList.push_back(I->first->getType());

  bool isVarArg = ParamTypeList.size() && ParamTypeList.back() == Type::VoidTy;
  if (isVarArg) ParamTypeList.pop_back();

  const FunctionType *MT = FunctionType::get(*$2, ParamTypeList, isVarArg);
  const PointerType *PMT = PointerType::get(MT);
  delete $2;

  Function *M = 0;
  if (SymbolTable *ST = CurModule.CurrentModule->getSymbolTable()) {
    // Is the function already in symtab?
    if (Value *V = ST->lookup(PMT, FunctionName)) {
      M = cast<Function>(V);

      // Yes it is.  If this is the case, either we need to be a forward decl,
      // or it needs to be.
      if (!CurMeth.isDeclare && !M->isExternal())
	ThrowException("Redefinition of method '" + FunctionName + "'!");      

      // If we found a preexisting method prototype, remove it from the module,
      // so that we don't get spurious conflicts with global & local variables.
      //
      CurModule.CurrentModule->getFunctionList().remove(M);
    }
  }

  if (M == 0) {  // Not already defined?
    M = new Function(MT, $1, FunctionName);
    InsertValue(M, CurModule.Values);
    CurModule.DeclareNewGlobalValue(M, ValID::create($3));
  }
  free($3);  // Free strdup'd memory!

  CurMeth.FunctionStart(M);

  // Add all of the arguments we parsed to the method...
  if ($5 && !CurMeth.isDeclare) {        // Is null if empty...
    Function::ArgumentListType &ArgList = M->getArgumentList();

    for (list<pair<Argument*, char*> >::iterator I = $5->begin();
         I != $5->end(); ++I) {
      if (setValueName(I->first, I->second)) {  // Insert into symtab...
        assert(0 && "No arg redef allowed!");
      }
      
      InsertValue(I->first);
      ArgList.push_back(I->first);
    }
    delete $5;                     // We're now done with the argument list
  } else if ($5) {
    // If we are a declaration, we should free the memory for the argument list!
    for (list<pair<Argument*, char*> >::iterator I = $5->begin(), E = $5->end();
         I != E; ++I) {
      if (I->second) free(I->second);   // Free the memory for the name...
      delete I->first;                  // Free the unused function argument
    }
    delete $5;                          // Free the memory for the list itself
  }
}

FunctionHeader : FunctionHeaderH ConstPool BEGINTOK {
  $$ = CurMeth.CurrentFunction;

  // Resolve circular types before we parse the body of the method.
  ResolveTypes(CurMeth.LateResolveTypes);
}

Function : BasicBlockList END {
  $$ = $1;
}

FunctionProto : DECLARE { CurMeth.isDeclare = true; } FunctionHeaderH {
  $$ = CurMeth.CurrentFunction;
  assert($$->getParent() == 0 && "Function already in module!");
  CurModule.CurrentModule->getFunctionList().push_back($$);
  CurMeth.FunctionDone();
}

//===----------------------------------------------------------------------===//
//                        Rules to match Basic Blocks
//===----------------------------------------------------------------------===//

ConstValueRef : ESINT64VAL {    // A reference to a direct constant
    $$ = ValID::create($1);
  }
  | EUINT64VAL {
    $$ = ValID::create($1);
  }
  | FPVAL {                     // Perhaps it's an FP constant?
    $$ = ValID::create($1);
  }
  | TRUE {
    $$ = ValID::create((int64_t)1);
  } 
  | FALSE {
    $$ = ValID::create((int64_t)0);
  }
  | NULL_TOK {
    $$ = ValID::createNull();
  }

/*
  | STRINGCONSTANT {        // Quoted strings work too... especially for methods
    $$ = ValID::create_conststr($1);
  }
*/

// SymbolicValueRef - Reference to one of two ways of symbolically refering to
// another value.
//
SymbolicValueRef : INTVAL {  // Is it an integer reference...?
    $$ = ValID::create($1);
  }
  | VAR_ID {                 // Is it a named reference...?
    $$ = ValID::create($1);
  }

// ValueRef - A reference to a definition... either constant or symbolic
ValueRef : SymbolicValueRef | ConstValueRef


// ResolvedVal - a <type> <value> pair.  This is used only in cases where the
// type immediately preceeds the value reference, and allows complex constant
// pool references (for things like: 'ret [2 x int] [ int 12, int 42]')
ResolvedVal : Types ValueRef {
    $$ = getVal(*$1, $2); delete $1;
  }


BasicBlockList : BasicBlockList BasicBlock {
    ($$ = $1)->getBasicBlocks().push_back($2);
  }
  | FunctionHeader BasicBlock { // Do not allow methods with 0 basic blocks   
    ($$ = $1)->getBasicBlocks().push_back($2);
  }


// Basic blocks are terminated by branching instructions: 
// br, br/cc, switch, ret
//
BasicBlock : InstructionList OptAssign BBTerminatorInst  {
    if (setValueName($3, $2)) { assert(0 && "No redefn allowed!"); }
    InsertValue($3);

    $1->getInstList().push_back($3);
    InsertValue($1);
    $$ = $1;
  }
  | LABELSTR InstructionList OptAssign BBTerminatorInst  {
    if (setValueName($4, $3)) { assert(0 && "No redefn allowed!"); }
    InsertValue($4);

    $2->getInstList().push_back($4);
    if (setValueName($2, $1)) { assert(0 && "No label redef allowed!"); }

    InsertValue($2);
    $$ = $2;
  }

InstructionList : InstructionList Inst {
    $1->getInstList().push_back($2);
    $$ = $1;
  }
  | /* empty */ {
    $$ = new BasicBlock();
  }

BBTerminatorInst : RET ResolvedVal {              // Return with a result...
    $$ = new ReturnInst($2);
  }
  | RET VOID {                                       // Return with no result...
    $$ = new ReturnInst();
  }
  | BR LABEL ValueRef {                         // Unconditional Branch...
    $$ = new BranchInst(cast<BasicBlock>(getVal(Type::LabelTy, $3)));
  }                                                  // Conditional Branch...
  | BR BOOL ValueRef ',' LABEL ValueRef ',' LABEL ValueRef {  
    $$ = new BranchInst(cast<BasicBlock>(getVal(Type::LabelTy, $6)), 
			cast<BasicBlock>(getVal(Type::LabelTy, $9)),
			getVal(Type::BoolTy, $3));
  }
  | SWITCH IntType ValueRef ',' LABEL ValueRef '[' JumpTable ']' {
    SwitchInst *S = new SwitchInst(getVal($2, $3), 
                                   cast<BasicBlock>(getVal(Type::LabelTy, $6)));
    $$ = S;

    vector<pair<Constant*,BasicBlock*> >::iterator I = $8->begin(),
      E = $8->end();
    for (; I != E; ++I)
      S->dest_push_back(I->first, I->second);
  }
  | INVOKE TypesV ValueRef '(' ValueRefListE ')' TO ResolvedVal 
    EXCEPT ResolvedVal {
    const PointerType *PMTy;
    const FunctionType *Ty;

    if (!(PMTy = dyn_cast<PointerType>($2->get())) ||
        !(Ty = dyn_cast<FunctionType>(PMTy->getElementType()))) {
      // Pull out the types of all of the arguments...
      vector<const Type*> ParamTypes;
      if ($5) {
        for (vector<Value*>::iterator I = $5->begin(), E = $5->end(); I!=E; ++I)
          ParamTypes.push_back((*I)->getType());
      }

      bool isVarArg = ParamTypes.size() && ParamTypes.back() == Type::VoidTy;
      if (isVarArg) ParamTypes.pop_back();

      Ty = FunctionType::get($2->get(), ParamTypes, isVarArg);
      PMTy = PointerType::get(Ty);
    }
    delete $2;

    Value *V = getVal(PMTy, $3);   // Get the method we're calling...

    BasicBlock *Normal = dyn_cast<BasicBlock>($8);
    BasicBlock *Except = dyn_cast<BasicBlock>($10);

    if (Normal == 0 || Except == 0)
      ThrowException("Invoke instruction without label destinations!");

    // Create the call node...
    if (!$5) {                                   // Has no arguments?
      $$ = new InvokeInst(V, Normal, Except, vector<Value*>());
    } else {                                     // Has arguments?
      // Loop through FunctionType's arguments and ensure they are specified
      // correctly!
      //
      FunctionType::ParamTypes::const_iterator I = Ty->getParamTypes().begin();
      FunctionType::ParamTypes::const_iterator E = Ty->getParamTypes().end();
      vector<Value*>::iterator ArgI = $5->begin(), ArgE = $5->end();

      for (; ArgI != ArgE && I != E; ++ArgI, ++I)
	if ((*ArgI)->getType() != *I)
	  ThrowException("Parameter " +(*ArgI)->getName()+ " is not of type '" +
			 (*I)->getDescription() + "'!");

      if (I != E || (ArgI != ArgE && !Ty->isVarArg()))
	ThrowException("Invalid number of parameters detected!");

      $$ = new InvokeInst(V, Normal, Except, *$5);
    }
    delete $5;
  }



JumpTable : JumpTable IntType ConstValueRef ',' LABEL ValueRef {
    $$ = $1;
    Constant *V = cast<Constant>(getValNonImprovising($2, $3));
    if (V == 0)
      ThrowException("May only switch on a constant pool value!");

    $$->push_back(make_pair(V, cast<BasicBlock>(getVal($5, $6))));
  }
  | IntType ConstValueRef ',' LABEL ValueRef {
    $$ = new vector<pair<Constant*, BasicBlock*> >();
    Constant *V = cast<Constant>(getValNonImprovising($1, $2));

    if (V == 0)
      ThrowException("May only switch on a constant pool value!");

    $$->push_back(make_pair(V, cast<BasicBlock>(getVal($4, $5))));
  }

Inst : OptAssign InstVal {
  // Is this definition named?? if so, assign the name...
  if (setValueName($2, $1)) { assert(0 && "No redefin allowed!"); }
  InsertValue($2);
  $$ = $2;
}

PHIList : Types '[' ValueRef ',' ValueRef ']' {    // Used for PHI nodes
    $$ = new list<pair<Value*, BasicBlock*> >();
    $$->push_back(make_pair(getVal(*$1, $3), 
                            cast<BasicBlock>(getVal(Type::LabelTy, $5))));
    delete $1;
  }
  | PHIList ',' '[' ValueRef ',' ValueRef ']' {
    $$ = $1;
    $1->push_back(make_pair(getVal($1->front().first->getType(), $4),
                            cast<BasicBlock>(getVal(Type::LabelTy, $6))));
  }


ValueRefList : ResolvedVal {    // Used for call statements, and memory insts...
    $$ = new vector<Value*>();
    $$->push_back($1);
  }
  | ValueRefList ',' ResolvedVal {
    $$ = $1;
    $1->push_back($3);
  }

// ValueRefListE - Just like ValueRefList, except that it may also be empty!
ValueRefListE : ValueRefList | /*empty*/ { $$ = 0; }

InstVal : BinaryOps Types ValueRef ',' ValueRef {
    $$ = BinaryOperator::create($1, getVal(*$2, $3), getVal(*$2, $5));
    if ($$ == 0)
      ThrowException("binary operator returned null!");
    delete $2;
  }
  | UnaryOps ResolvedVal {
    $$ = UnaryOperator::create($1, $2);
    if ($$ == 0)
      ThrowException("unary operator returned null!");
  }
  | ShiftOps ResolvedVal ',' ResolvedVal {
    if ($4->getType() != Type::UByteTy)
      ThrowException("Shift amount must be ubyte!");
    $$ = new ShiftInst($1, $2, $4);
  }
  | CAST ResolvedVal TO Types {
    $$ = new CastInst($2, *$4);
    delete $4;
  }
  | PHI PHIList {
    const Type *Ty = $2->front().first->getType();
    $$ = new PHINode(Ty);
    while ($2->begin() != $2->end()) {
      if ($2->front().first->getType() != Ty) 
	ThrowException("All elements of a PHI node must be of the same type!");
      cast<PHINode>($$)->addIncoming($2->front().first, $2->front().second);
      $2->pop_front();
    }
    delete $2;  // Free the list...
  } 
  | CALL TypesV ValueRef '(' ValueRefListE ')' {
    const PointerType *PMTy;
    const FunctionType *Ty;

    if (!(PMTy = dyn_cast<PointerType>($2->get())) ||
        !(Ty = dyn_cast<FunctionType>(PMTy->getElementType()))) {
      // Pull out the types of all of the arguments...
      vector<const Type*> ParamTypes;
      if ($5) {
        for (vector<Value*>::iterator I = $5->begin(), E = $5->end(); I!=E; ++I)
          ParamTypes.push_back((*I)->getType());
      }

      bool isVarArg = ParamTypes.size() && ParamTypes.back() == Type::VoidTy;
      if (isVarArg) ParamTypes.pop_back();

      Ty = FunctionType::get($2->get(), ParamTypes, isVarArg);
      PMTy = PointerType::get(Ty);
    }
    delete $2;

    Value *V = getVal(PMTy, $3);   // Get the method we're calling...

    // Create the call node...
    if (!$5) {                                   // Has no arguments?
      $$ = new CallInst(V, vector<Value*>());
    } else {                                     // Has arguments?
      // Loop through FunctionType's arguments and ensure they are specified
      // correctly!
      //
      FunctionType::ParamTypes::const_iterator I = Ty->getParamTypes().begin();
      FunctionType::ParamTypes::const_iterator E = Ty->getParamTypes().end();
      vector<Value*>::iterator ArgI = $5->begin(), ArgE = $5->end();

      for (; ArgI != ArgE && I != E; ++ArgI, ++I)
	if ((*ArgI)->getType() != *I)
	  ThrowException("Parameter " +(*ArgI)->getName()+ " is not of type '" +
			 (*I)->getDescription() + "'!");

      if (I != E || (ArgI != ArgE && !Ty->isVarArg()))
	ThrowException("Invalid number of parameters detected!");

      $$ = new CallInst(V, *$5);
    }
    delete $5;
  }
  | MemoryInst {
    $$ = $1;
  }


// IndexList - List of indices for GEP based instructions...
IndexList : ',' ValueRefList { 
  $$ = $2; 
} | /* empty */ { 
  $$ = new vector<Value*>(); 
}

MemoryInst : MALLOC Types {
    $$ = new MallocInst(PointerType::get(*$2));
    delete $2;
  }
  | MALLOC Types ',' UINT ValueRef {
    const Type *Ty = PointerType::get(*$2);
    $$ = new MallocInst(Ty, getVal($4, $5));
    delete $2;
  }
  | ALLOCA Types {
    $$ = new AllocaInst(PointerType::get(*$2));
    delete $2;
  }
  | ALLOCA Types ',' UINT ValueRef {
    const Type *Ty = PointerType::get(*$2);
    Value *ArrSize = getVal($4, $5);
    $$ = new AllocaInst(Ty, ArrSize);
    delete $2;
  }
  | FREE ResolvedVal {
    if (!$2->getType()->isPointerType())
      ThrowException("Trying to free nonpointer type " + 
                     $2->getType()->getDescription() + "!");
    $$ = new FreeInst($2);
  }

  | LOAD Types ValueRef IndexList {
    if (!(*$2)->isPointerType())
      ThrowException("Can't load from nonpointer type: " +
		     (*$2)->getDescription());
    if (LoadInst::getIndexedType(*$2, *$4) == 0)
      ThrowException("Invalid indices for load instruction!");

    $$ = new LoadInst(getVal(*$2, $3), *$4);
    delete $4;   // Free the vector...
    delete $2;
  }
  | STORE ResolvedVal ',' Types ValueRef IndexList {
    if (!(*$4)->isPointerType())
      ThrowException("Can't store to a nonpointer type: " +
                     (*$4)->getDescription());
    const Type *ElTy = StoreInst::getIndexedType(*$4, *$6);
    if (ElTy == 0)
      ThrowException("Can't store into that field list!");
    if (ElTy != $2->getType())
      ThrowException("Can't store '" + $2->getType()->getDescription() +
                     "' into space of type '" + ElTy->getDescription() + "'!");
    $$ = new StoreInst($2, getVal(*$4, $5), *$6);
    delete $4; delete $6;
  }
  | GETELEMENTPTR Types ValueRef IndexList {
    if (!(*$2)->isPointerType())
      ThrowException("getelementptr insn requires pointer operand!");
    if (!GetElementPtrInst::getIndexedType(*$2, *$4, true))
      ThrowException("Can't get element ptr '" + (*$2)->getDescription()+ "'!");
    $$ = new GetElementPtrInst(getVal(*$2, $3), *$4);
    delete $2; delete $4;
  }

%%
int yyerror(const char *ErrorMsg) {
  ThrowException(string("Parse error: ") + ErrorMsg);
  return 0;
}
