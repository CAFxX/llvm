//===-- llvmAsmParser.y - Parser for llvm assembly files ---------*- C++ -*--=//
//
//  This file implements the bison parser for LLVM assembly languages files.
//
//===------------------------------------------------------------------------=//

//
// TODO: Parse comments and add them to an internal node... so that they may
// be saved in the bytecode format as well as everything else.  Very important
// for a general IR format.
//

%{
#include "ParserInternals.h"
#include "llvm/BasicBlock.h"
#include "llvm/Method.h"
#include "llvm/SymbolTable.h"
#include "llvm/Module.h"
#include "llvm/Type.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Assembly/Parser.h"
#include "llvm/ConstantPool.h"
#include "llvm/iTerminators.h"
#include "llvm/iMemory.h"
#include <list>
#include <utility>            // Get definition of pair class
#include <stdio.h>            // This embarasment is due to our flex lexer...

int yyerror(const char *ErrorMsg); // Forward declarations to prevent "implicit 
int yylex();                       // declaration" of xxx warnings.
int yyparse();

static Module *ParserResult;
const ToolCommandLine *CurOptions = 0;

// This contains info used when building the body of a method.  It is destroyed
// when the method is completed.
//
typedef vector<Value *> ValueList;           // Numbered defs
static void ResolveDefinitions(vector<ValueList> &LateResolvers);

static struct PerModuleInfo {
  Module *CurrentModule;
  vector<ValueList> Values;     // Module level numbered definitions
  vector<ValueList> LateResolveValues;

  void ModuleDone() {
    // If we could not resolve some blocks at parsing time (forward branches)
    // resolve the branches now...
    ResolveDefinitions(LateResolveValues);

    Values.clear();         // Clear out method local definitions
    CurrentModule = 0;
  }
} CurModule;

static struct PerMethodInfo {
  Method *CurrentMethod;         // Pointer to current method being created

  vector<ValueList> Values;          // Keep track of numbered definitions
  vector<ValueList> LateResolveValues;

  inline PerMethodInfo() {
    CurrentMethod = 0;
  }

  inline ~PerMethodInfo() {}

  inline void MethodStart(Method *M) {
    CurrentMethod = M;
  }

  void MethodDone() {
    // If we could not resolve some blocks at parsing time (forward branches)
    // resolve the branches now...
    ResolveDefinitions(LateResolveValues);

    Values.clear();         // Clear out method local definitions
    CurrentMethod = 0;
  }
} CurMeth;  // Info for the current method...


//===----------------------------------------------------------------------===//
//               Code to handle definitions of all the types
//===----------------------------------------------------------------------===//

static void InsertValue(Value *D, vector<ValueList> &ValueTab = CurMeth.Values) {
  if (!D->hasName()) {             // Is this a numbered definition?
    unsigned type = D->getType()->getUniqueID();
    if (ValueTab.size() <= type)
      ValueTab.resize(type+1, ValueList());
    //printf("Values[%d][%d] = %d\n", type, ValueTab[type].size(), D);
    ValueTab[type].push_back(D);
  }
}

static Value *getVal(const Type *Type, ValID &D, 
                     bool DoNotImprovise = false) {
  switch (D.Type) {
  case 0: {                 // Is it a numbered definition?
    unsigned type = Type->getUniqueID();
    unsigned Num = (unsigned)D.Num;

    // Module constants occupy the lowest numbered slots...
    if (type < CurModule.Values.size()) {
      if (Num < CurModule.Values[type].size()) 
        return CurModule.Values[type][Num];

      Num -= CurModule.Values[type].size();
    }

    // Make sure that our type is within bounds
    if (CurMeth.Values.size() <= type)
      break;

    // Check that the number is within bounds...
    if (CurMeth.Values[type].size() <= Num)
      break;
  
    return CurMeth.Values[type][Num];
  }
  case 1: {                // Is it a named definition?
    string Name(D.Name);
    SymbolTable *SymTab = 0;
    if (CurMeth.CurrentMethod) 
      SymTab = CurMeth.CurrentMethod->getSymbolTable();
    Value *N = SymTab ? SymTab->lookup(Type, Name) : 0;

    if (N == 0) {
      SymTab = CurModule.CurrentModule->getSymbolTable();
      if (SymTab)
        N = SymTab->lookup(Type, Name);
      if (N == 0) break;
    }

    D.destroy();  // Free old strdup'd memory...
    return N;
  }

  case 2:                 // Is it a constant pool reference??
  case 3:                 // Is it an unsigned const pool reference?
  case 4:{                // Is it a string const pool reference?
    ConstPoolVal *CPV = 0;

    // Check to make sure that "Type" is an integral type, and that our 
    // value will fit into the specified type...
    switch (D.Type) {
    case 2:
      if (Type == Type::BoolTy) {  // Special handling for boolean data
        CPV = new ConstPoolBool(D.ConstPool64 != 0);
      } else {
        if (!ConstPoolSInt::isValueValidForType(Type, D.ConstPool64))
          ThrowException("Symbolic constant pool reference is invalid!");
        CPV = new ConstPoolSInt(Type, D.ConstPool64);
      }
      break;
    case 3:
      if (!ConstPoolUInt::isValueValidForType(Type, D.UConstPool64)) {
        if (!ConstPoolSInt::isValueValidForType(Type, D.ConstPool64)) {
          ThrowException("Symbolic constant pool reference is invalid!");
        } else {     // This is really a signed reference.  Transmogrify.
          CPV = new ConstPoolSInt(Type, D.ConstPool64);
        }
      } else {
        CPV = new ConstPoolUInt(Type, D.UConstPool64);
      }
      break;
    case 4:
      cerr << "FIXME: TODO: String constants [sbyte] not implemented yet!\n";
      abort();
      //CPV = new ConstPoolString(D.Name);
      D.destroy();   // Free the string memory
      break;
    }
    assert(CPV && "How did we escape creating a constant??");

    // Scan through the constant table and see if we already have loaded this
    // constant.
    //
    ConstantPool &CP = CurMeth.CurrentMethod ? 
                         CurMeth.CurrentMethod->getConstantPool() :
                           CurModule.CurrentModule->getConstantPool();
    ConstPoolVal *C = CP.find(CPV);      // Already have this constant?
    if (C) {
      delete CPV;  // Didn't need this after all, oh well.
      return C;    // Yup, we already have one, recycle it!
    }
    CP.insert(CPV);
      
    // Success, everything is kosher. Lets go!
    return CPV;
  }   // End of case 2,3,4
  }   // End of switch


  // If we reached here, we referenced either a symbol that we don't know about
  // or an id number that hasn't been read yet.  We may be referencing something
  // forward, so just create an entry to be resolved later and get to it...
  //
  if (DoNotImprovise) return 0;  // Do we just want a null to be returned?

  // TODO: Attempt to coallecse nodes that are the same with previous ones.
  Value *d = 0;
  switch (Type->getPrimitiveID()) {
  case Type::LabelTyID: d = new    BBPlaceHolder(Type, D); break;
  case Type::MethodTyID:
    d = new MethPlaceHolder(Type, D); 
    InsertValue(d, CurModule.LateResolveValues);
    return d;
//case Type::ClassTyID:      d = new ClassPlaceHolder(Type, D); break;
  default:                   d = new   DefPlaceHolder(Type, D); break;
  }

  assert(d != 0 && "How did we not make something?");
  InsertValue(d, CurMeth.LateResolveValues);
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
static void ResolveDefinitions(vector<ValueList> &LateResolvers) {
  // Loop over LateResolveDefs fixing up stuff that couldn't be resolved
  for (unsigned ty = 0; ty < LateResolvers.size(); ty++) {
    while (!LateResolvers[ty].empty()) {
      Value *V = LateResolvers[ty].back();
      LateResolvers[ty].pop_back();
      ValID &DID = getValIDFromPlaceHolder(V);

      Value *TheRealValue = getVal(Type::getUniqueIDType(ty), DID, true);

      if (TheRealValue == 0 && DID.Type == 1)
        ThrowException("Reference to an invalid definition: '" +DID.getName() +
                       "' of type '" + V->getType()->getName() + "'");
      else if (TheRealValue == 0)
        ThrowException("Reference to an invalid definition: #" +itostr(DID.Num)+
                       " of type '" + V->getType()->getName() + "'");

      V->replaceAllUsesWith(TheRealValue);
      assert(V->use_empty());
      delete V;
    }
  }

  LateResolvers.clear();
}

// addConstValToConstantPool - This code is used to insert a constant into the
// current constant pool.  This is designed to make maximal (but not more than
// possible) reuse (merging) of constants in the constant pool.  This means that
// multiple references to %4, for example will all get merged.
//
static ConstPoolVal *addConstValToConstantPool(ConstPoolVal *C) {
  vector<ValueList> &ValTab = CurMeth.CurrentMethod ? 
                                  CurMeth.Values : CurModule.Values;
  ConstantPool &CP = CurMeth.CurrentMethod ? 
                          CurMeth.CurrentMethod->getConstantPool() : 
                          CurModule.CurrentModule->getConstantPool();

  if (ConstPoolVal *CPV = CP.find(C)) {
    // Constant already in constant pool. Try to merge the two constants
    if (CPV->hasName() && !C->hasName()) {
      // Merge the two values, we inherit the existing CPV's name.  
      // InsertValue requires that the value have no name to insert correctly
      // (because we want to fill the slot this constant would have filled)
      //
      string Name = CPV->getName();
      CPV->setName("");
      InsertValue(CPV, ValTab);
      CPV->setName(Name);
      delete C;
      return CPV;
    } else if (!CPV->hasName() && C->hasName()) {
      // If we have a name on this value and there isn't one in the const 
      // pool val already, propogate it.
      //
      CPV->setName(C->getName());
      delete C;   // Sorry, you're toast
      return CPV;
    } else if (CPV->hasName() && C->hasName()) {
      // Both values have distinct names.  We cannot merge them.
      CP.insert(C);
      InsertValue(C, ValTab);
      return C;
    } else if (!CPV->hasName() && !C->hasName()) {
      // Neither value has a name, trivially merge them.
      InsertValue(CPV, ValTab);
      delete C;
      return CPV;
    }

    assert(0 && "Not reached!");
    return 0;
  } else {           // No duplication of value.
    CP.insert(C);
    InsertValue(C, ValTab);
    return C;
  } 
}

//===----------------------------------------------------------------------===//
//            RunVMAsmParser - Define an interface to this parser
//===----------------------------------------------------------------------===//
//
Module *RunVMAsmParser(const ToolCommandLine &Opts, FILE *F) {
  llvmAsmin = F;
  CurOptions = &Opts;
  llvmAsmlineno = 1;      // Reset the current line number...

  CurModule.CurrentModule = new Module();  // Allocate a new module to read
  yyparse();       // Parse the file.
  Module *Result = ParserResult;
  CurOptions = 0;
  llvmAsmin = stdin;    // F is about to go away, don't use it anymore...
  ParserResult = 0;

  return Result;
}

%}

%union {
  Module                  *ModuleVal;
  Method                  *MethodVal;
  MethodArgument          *MethArgVal;
  BasicBlock              *BasicBlockVal;
  TerminatorInst          *TermInstVal;
  Instruction             *InstVal;
  ConstPoolVal            *ConstVal;
  const Type              *TypeVal;

  list<MethodArgument*>   *MethodArgList;
  list<Value*>            *ValueList;
  list<const Type*>       *TypeList;
  list<pair<Value*, BasicBlock*> > *PHIList;   // Represent the RHS of PHI node
  list<pair<ConstPoolVal*, BasicBlock*> > *JumpTable;
  vector<ConstPoolVal*>   *ConstVector;

  int64_t                  SInt64Val;
  uint64_t                 UInt64Val;
  int                      SIntVal;
  unsigned                 UIntVal;

  char                    *StrVal;   // This memory is allocated by strdup!
  ValID                    ValIDVal; // May contain memory allocated by strdup

  Instruction::UnaryOps    UnaryOpVal;
  Instruction::BinaryOps   BinaryOpVal;
  Instruction::TermOps     TermOpVal;
  Instruction::MemoryOps   MemOpVal;
}

%type <ModuleVal>     Module MethodList
%type <MethodVal>     Method MethodHeader BasicBlockList
%type <BasicBlockVal> BasicBlock InstructionList
%type <TermInstVal>   BBTerminatorInst
%type <InstVal>       Inst InstVal MemoryInst
%type <ConstVal>      ConstVal
%type <ConstVector>   ConstVector
%type <MethodArgList> ArgList ArgListH
%type <MethArgVal>    ArgVal
%type <PHIList>       PHIList
%type <ValueList>     ValueRefList ValueRefListE
%type <TypeList>      TypeList
%type <JumpTable>     JumpTable

%type <ValIDVal>      ValueRef ConstValueRef // Reference to a definition or BB

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

// Built in types...
%type  <TypeVal> Types TypesV SIntType UIntType IntType
%token <TypeVal> VOID BOOL SBYTE UBYTE SHORT USHORT INT UINT LONG ULONG
%token <TypeVal> FLOAT DOUBLE STRING TYPE LABEL

%token <StrVal>     VAR_ID LABELSTR STRINGCONSTANT
%type  <StrVal>  OptVAR_ID OptAssign


%token IMPLEMENTATION TRUE FALSE BEGINTOK END DECLARE TO
%token PHI CALL

// Basic Block Terminating Operators 
%token <TermOpVal> RET BR SWITCH

// Unary Operators 
%type  <UnaryOpVal> UnaryOps  // all the unary operators
%token <UnaryOpVal> NOT CAST

// Binary Operators 
%type  <BinaryOpVal> BinaryOps  // all the binary operators
%token <BinaryOpVal> ADD SUB MUL DIV REM

// Binary Comarators
%token <BinaryOpVal> SETLE SETGE SETLT SETGT SETEQ SETNE 

// Memory Instructions
%token <MemoryOpVal> MALLOC ALLOCA FREE LOAD STORE GETFIELD PUTFIELD

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

// Types includes all predefined types... except void, because you can't do 
// anything with it except for certain specific things...
//
// User defined types are added latter...
//
Types     : BOOL | SBYTE | UBYTE | SHORT | USHORT | INT | UINT 
Types     : LONG | ULONG | FLOAT | DOUBLE | STRING | TYPE | LABEL

// TypesV includes all of 'Types', but it also includes the void type.
TypesV    : Types | VOID

// Operations that are notably excluded from this list include: 
// RET, BR, & SWITCH because they end basic blocks and are treated specially.
//
UnaryOps  : NOT
BinaryOps : ADD | SUB | MUL | DIV | REM
BinaryOps : SETLE | SETGE | SETLT | SETGT | SETEQ | SETNE

// Valueine some types that allow classification if we only want a particular 
// thing...
SIntType :  LONG |  INT |  SHORT | SBYTE
UIntType : ULONG | UINT | USHORT | UBYTE
IntType : SIntType | UIntType

OptAssign : VAR_ID '=' {
    $$ = $1;
  }
  | /*empty*/ { 
    $$ = 0; 
  }

ConstVal : SIntType EINT64VAL {     // integral constants
    if (!ConstPoolSInt::isValueValidForType($1, $2))
      ThrowException("Constant value doesn't fit in type!");
    $$ = new ConstPoolSInt($1, $2);
  } 
  | UIntType EUINT64VAL {           // integral constants
    if (!ConstPoolUInt::isValueValidForType($1, $2))
      ThrowException("Constant value doesn't fit in type!");
    $$ = new ConstPoolUInt($1, $2);
  } 
  | BOOL TRUE {                     // Boolean constants
    $$ = new ConstPoolBool(true);
  }
  | BOOL FALSE {                    // Boolean constants
    $$ = new ConstPoolBool(false);
  }
  | STRING STRINGCONSTANT {         // String constants
    cerr << "FIXME: TODO: String constants [sbyte] not implemented yet!\n";
    abort();
    //$$ = new ConstPoolString($2);
    free($2);
  } 
  | TYPE Types {                    // Type constants
    $$ = new ConstPoolType($2);
  }
  | '[' Types ']' '[' ConstVector ']' {      // Nonempty array constant
    // Verify all elements are correct type!
    const ArrayType *AT = ArrayType::getArrayType($2);
    for (unsigned i = 0; i < $5->size(); i++) {
      if ($2 != (*$5)[i]->getType())
	ThrowException("Element #" + utostr(i) + " is not of type '" + 
		       $2->getName() + "' as required!\nIt is of type '" +
		       (*$5)[i]->getType()->getName() + "'.");
    }

    $$ = new ConstPoolArray(AT, *$5);
    delete $5;
  }
  | '[' Types ']' '[' ']' {                  // Empty array constant
    vector<ConstPoolVal*> Empty;
    $$ = new ConstPoolArray(ArrayType::getArrayType($2), Empty);
  }
  | '[' EUINT64VAL 'x' Types ']' '[' ConstVector ']' {
    // Verify all elements are correct type!
    const ArrayType *AT = ArrayType::getArrayType($4, (int)$2);
    if ($2 != $7->size())
      ThrowException("Type mismatch: constant sized array initialized with " +
		     utostr($7->size()) +  " arguments, but has size of " + 
		     itostr((int)$2) + "!");

    for (unsigned i = 0; i < $7->size(); i++) {
      if ($4 != (*$7)[i]->getType())
	ThrowException("Element #" + utostr(i) + " is not of type '" + 
		       $4->getName() + "' as required!\nIt is of type '" +
		       (*$7)[i]->getType()->getName() + "'.");
    }

    $$ = new ConstPoolArray(AT, *$7);
    delete $7;
  }
  | '[' EUINT64VAL 'x' Types ']' '[' ']' {
    if ($2 != 0) 
      ThrowException("Type mismatch: constant sized array initialized with 0"
		     " arguments, but has size of " + itostr((int)$2) + "!");
    vector<ConstPoolVal*> Empty;
    $$ = new ConstPoolArray(ArrayType::getArrayType($4, 0), Empty);
  }
  | '{' TypeList '}' '{' ConstVector '}' {
    StructType::ElementTypes Types($2->begin(), $2->end());
    delete $2;

    const StructType *St = StructType::getStructType(Types);
    $$ = new ConstPoolStruct(St, *$5);
    delete $5;
  }
  | '{' '}' '{' '}' {
    const StructType *St = 
      StructType::getStructType(StructType::ElementTypes());
    vector<ConstPoolVal*> Empty;
    $$ = new ConstPoolStruct(St, Empty);
  }
/*
  | Types '*' ConstVal {
    assert(0);
    $$ = 0;
  }
*/


ConstVector : ConstVector ',' ConstVal {
    ($$ = $1)->push_back(addConstValToConstantPool($3));
  }
  | ConstVal {
    $$ = new vector<ConstPoolVal*>();
    $$->push_back(addConstValToConstantPool($1));
  }


ConstPool : ConstPool OptAssign ConstVal { 
    if ($2) {
      $3->setName($2);
      free($2);
    }

    addConstValToConstantPool($3);
  }
  | /* empty: end of list */ { 
  }


//===----------------------------------------------------------------------===//
//                             Rules to match Modules
//===----------------------------------------------------------------------===//

// Module rule: Capture the result of parsing the whole file into a result
// variable...
//
Module : MethodList {
  $$ = ParserResult = $1;
  CurModule.ModuleDone();
}

MethodList : MethodList Method {
    $1->getMethodList().push_back($2);
    CurMeth.MethodDone();
    $$ = $1;
  } 
  | ConstPool IMPLEMENTATION {
    $$ = CurModule.CurrentModule;
  }


//===----------------------------------------------------------------------===//
//                       Rules to match Method Headers
//===----------------------------------------------------------------------===//

OptVAR_ID : VAR_ID | /*empty*/ { $$ = 0; }

ArgVal : Types OptVAR_ID {
  $$ = new MethodArgument($1);
  if ($2) {      // Was the argument named?
    $$->setName($2); 
    free($2);    // The string was strdup'd, so free it now.
  }
}

ArgListH : ArgVal ',' ArgListH {
    $$ = $3;
    $3->push_front($1);
  }
  | ArgVal {
    $$ = new list<MethodArgument*>();
    $$->push_front($1);
  }

ArgList : ArgListH {
    $$ = $1;
  }
  | /* empty */ {
    $$ = 0;
  }

MethodHeaderH : TypesV STRINGCONSTANT '(' ArgList ')' {
  MethodType::ParamTypes ParamTypeList;
  if ($4)
    for (list<MethodArgument*>::iterator I = $4->begin(); I != $4->end(); ++I)
      ParamTypeList.push_back((*I)->getType());

  const MethodType *MT = MethodType::getMethodType($1, ParamTypeList);

  Method *M = new Method(MT, $2);
  free($2);  // Free strdup'd memory!

  InsertValue(M, CurModule.Values);

  CurMeth.MethodStart(M);

  // Add all of the arguments we parsed to the method...
  if ($4) {        // Is null if empty...
    Method::ArgumentListType &ArgList = M->getArgumentList();

    for (list<MethodArgument*>::iterator I = $4->begin(); I != $4->end(); ++I) {
      InsertValue(*I);
      ArgList.push_back(*I);
    }
    delete $4;                     // We're now done with the argument list
  }
}

MethodHeader : MethodHeaderH ConstPool BEGINTOK {
  $$ = CurMeth.CurrentMethod;
}

Method : BasicBlockList END {
  $$ = $1;
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
  | TRUE {
    $$ = ValID::create((int64_t)1);
  } 
  | FALSE {
    $$ = ValID::create((int64_t)0);
  }
  | STRINGCONSTANT {        // Quoted strings work too... especially for methods
    $$ = ValID::create_conststr($1);
  }

// ValueRef - A reference to a definition... 
ValueRef : INTVAL {           // Is it an integer reference...?
    $$ = ValID::create($1);
  }
  | VAR_ID {                // It must be a named reference then...
    $$ = ValID::create($1);
  }
  | ConstValueRef {
    $$ = $1;
  }

// The user may refer to a user defined type by its typeplane... check for this
// now...
//
Types : ValueRef {
    Value *D = getVal(Type::TypeTy, $1, true);
    if (D == 0) ThrowException("Invalid user defined type: " + $1.getName());

    // User defined type not in const pool!
    ConstPoolType *CPT = (ConstPoolType*)D->castConstantAsserting();
    $$ = CPT->getValue();
  }
  | TypesV '(' TypeList ')' {               // Method derived type?
    MethodType::ParamTypes Params($3->begin(), $3->end());
    delete $3;
    $$ = MethodType::getMethodType($1, Params);
  }
  | TypesV '(' ')' {               // Method derived type?
    MethodType::ParamTypes Params;     // Empty list
    $$ = MethodType::getMethodType($1, Params);
  }
  | '[' Types ']' {
    $$ = ArrayType::getArrayType($2);
  }
  | '[' EUINT64VAL 'x' Types ']' {
    $$ = ArrayType::getArrayType($4, (int)$2);
  }
  | '{' TypeList '}' {
    StructType::ElementTypes Elements($2->begin(), $2->end());
    delete $2;
    $$ = StructType::getStructType(Elements);
  }
  | '{' '}' {
    $$ = StructType::getStructType(StructType::ElementTypes());
  }
  | Types '*' {
    $$ = PointerType::getPointerType($1);
  }


TypeList : Types {
    $$ = new list<const Type*>();
    $$->push_back($1);
  }
  | TypeList ',' Types {
    ($$=$1)->push_back($3);
  }


BasicBlockList : BasicBlockList BasicBlock {
    $1->getBasicBlocks().push_back($2);
    $$ = $1;
  }
  | MethodHeader BasicBlock { // Do not allow methods with 0 basic blocks   
    $$ = $1;                  // in them...
    $1->getBasicBlocks().push_back($2);
  }


// Basic blocks are terminated by branching instructions: 
// br, br/cc, switch, ret
//
BasicBlock : InstructionList BBTerminatorInst  {
    $1->getInstList().push_back($2);
    InsertValue($1);
    $$ = $1;
  }
  | LABELSTR InstructionList BBTerminatorInst  {
    $2->getInstList().push_back($3);
    $2->setName($1);
    free($1);         // Free the strdup'd memory...

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

BBTerminatorInst : RET Types ValueRef {              // Return with a result...
    $$ = new ReturnInst(getVal($2, $3));
  }
  | RET VOID {                                       // Return with no result...
    $$ = new ReturnInst();
  }
  | BR LABEL ValueRef {                         // Unconditional Branch...
    $$ = new BranchInst((BasicBlock*)getVal(Type::LabelTy, $3));
  }                                                  // Conditional Branch...
  | BR BOOL ValueRef ',' LABEL ValueRef ',' LABEL ValueRef {  
    $$ = new BranchInst((BasicBlock*)getVal(Type::LabelTy, $6), 
			(BasicBlock*)getVal(Type::LabelTy, $9),
			getVal(Type::BoolTy, $3));
  }
  | SWITCH IntType ValueRef ',' LABEL ValueRef '[' JumpTable ']' {
    SwitchInst *S = new SwitchInst(getVal($2, $3), 
                                   (BasicBlock*)getVal(Type::LabelTy, $6));
    $$ = S;

    list<pair<ConstPoolVal*, BasicBlock*> >::iterator I = $8->begin(), 
                                                      end = $8->end();
    for (; I != end; ++I)
      S->dest_push_back(I->first, I->second);
  }

JumpTable : JumpTable IntType ConstValueRef ',' LABEL ValueRef {
    $$ = $1;
    ConstPoolVal *V = (ConstPoolVal*)getVal($2, $3, true);
    if (V == 0)
      ThrowException("May only switch on a constant pool value!");

    $$->push_back(make_pair(V, (BasicBlock*)getVal($5, $6)));
  }
  | IntType ConstValueRef ',' LABEL ValueRef {
    $$ = new list<pair<ConstPoolVal*, BasicBlock*> >();
    ConstPoolVal *V = (ConstPoolVal*)getVal($1, $2, true);

    if (V == 0)
      ThrowException("May only switch on a constant pool value!");

    $$->push_back(make_pair(V, (BasicBlock*)getVal($4, $5)));
  }

Inst : OptAssign InstVal {
  if ($1)              // Is this definition named??
    $2->setName($1);   // if so, assign the name...

  InsertValue($2);
  $$ = $2;
}

PHIList : Types '[' ValueRef ',' ValueRef ']' {    // Used for PHI nodes
    $$ = new list<pair<Value*, BasicBlock*> >();
    $$->push_back(make_pair(getVal($1, $3), 
			    (BasicBlock*)getVal(Type::LabelTy, $5)));
  }
  | PHIList ',' '[' ValueRef ',' ValueRef ']' {
    $$ = $1;
    $1->push_back(make_pair(getVal($1->front().first->getType(), $4),
			    (BasicBlock*)getVal(Type::LabelTy, $6)));
  }


ValueRefList : Types ValueRef {    // Used for call statements...
    $$ = new list<Value*>();
    $$->push_back(getVal($1, $2));
  }
  | ValueRefList ',' ValueRef {
    $$ = $1;
    $1->push_back(getVal($1->front()->getType(), $3));
  }

// ValueRefListE - Just like ValueRefList, except that it may also be empty!
ValueRefListE : ValueRefList | /*empty*/ { $$ = 0; }

InstVal : BinaryOps Types ValueRef ',' ValueRef {
    $$ = BinaryOperator::create($1, getVal($2, $3), getVal($2, $5));
    if ($$ == 0)
      ThrowException("binary operator returned null!");
  }
  | UnaryOps Types ValueRef {
    $$ = UnaryOperator::create($1, getVal($2, $3));
    if ($$ == 0)
      ThrowException("unary operator returned null!");
  }
  | CAST Types ValueRef TO Types {
    $$ = UnaryOperator::create($1, getVal($2, $3), $5);
  }
  | PHI PHIList {
    const Type *Ty = $2->front().first->getType();
    $$ = new PHINode(Ty);
    while ($2->begin() != $2->end()) {
      if ($2->front().first->getType() != Ty) 
	ThrowException("All elements of a PHI node must be of the same type!");
      ((PHINode*)$$)->addIncoming($2->front().first, $2->front().second);
      $2->pop_front();
    }
    delete $2;  // Free the list...
  } 
  | CALL Types ValueRef '(' ValueRefListE ')' {
    if (!$2->isMethodType())
      ThrowException("Can only call methods: invalid type '" + 
		     $2->getName() + "'!");

    const MethodType *Ty = (const MethodType*)$2;

    Value *V = getVal(Ty, $3);
    if (!V->isMethod() || V->getType() != Ty)
      ThrowException("Cannot call: " + $3.getName() + "!");

    // Create or access a new type that corresponds to the function call...
    vector<Value *> Params;

    if ($5) {
      // Pull out just the arguments...
      Params.insert(Params.begin(), $5->begin(), $5->end());
      delete $5;

      // Loop through MethodType's arguments and ensure they are specified
      // correctly!
      //
      MethodType::ParamTypes::const_iterator I = Ty->getParamTypes().begin();
      unsigned i;
      for (i = 0; i < Params.size() && I != Ty->getParamTypes().end(); ++i,++I){
	if (Params[i]->getType() != *I)
	  ThrowException("Parameter " + utostr(i) + " is not of type '" + 
			 (*I)->getName() + "'!");
      }

      if (i != Params.size() || I != Ty->getParamTypes().end())
	ThrowException("Invalid number of parameters detected!");
    }

    // Create the call node...
    $$ = new CallInst((Method*)V, Params);
  }
  | MemoryInst {
    $$ = $1;
  }

MemoryInst : MALLOC Types {
    const Type *Ty = PointerType::getPointerType($2);
    addConstValToConstantPool(new ConstPoolType(Ty));
    $$ = new MallocInst(Ty);
  }
  | MALLOC Types ',' UINT ValueRef {
    if (!$2->isArrayType() || ((const ArrayType*)$2)->isSized())
      ThrowException("Trying to allocate " + $2->getName() + 
		     " as unsized array!");
    const Type *Ty = PointerType::getPointerType($2);
    addConstValToConstantPool(new ConstPoolType(Ty));
    Value *ArrSize = getVal($4, $5);
    $$ = new MallocInst(Ty, ArrSize);
  }
  | ALLOCA Types {
    const Type *Ty = PointerType::getPointerType($2);
    addConstValToConstantPool(new ConstPoolType(Ty));
    $$ = new AllocaInst(Ty);
  }
  | ALLOCA Types ',' UINT ValueRef {
    if (!$2->isArrayType() || ((const ArrayType*)$2)->isSized())
      ThrowException("Trying to allocate " + $2->getName() + 
		     " as unsized array!");
    const Type *Ty = PointerType::getPointerType($2);
    addConstValToConstantPool(new ConstPoolType(Ty));
    Value *ArrSize = getVal($4, $5);
    $$ = new AllocaInst(Ty, ArrSize);
  }
  | FREE Types ValueRef {
    if (!$2->isPointerType())
      ThrowException("Trying to free nonpointer type " + $2->getName() + "!");
    $$ = new FreeInst(getVal($2, $3));
  }

%%
int yyerror(const char *ErrorMsg) {
  ThrowException(string("Parse error: ") + ErrorMsg);
  return 0;
}
