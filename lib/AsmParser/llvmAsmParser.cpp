
/*  A Bison parser, made from llvmAsmParser.y
    by GNU Bison version 1.28  */

#define YYBISON 1  /* Identify Bison output.  */

#define yyparse llvmAsmparse
#define yylex llvmAsmlex
#define yyerror llvmAsmerror
#define yylval llvmAsmlval
#define yychar llvmAsmchar
#define yydebug llvmAsmdebug
#define yynerrs llvmAsmnerrs
#define	ESINT64VAL	257
#define	EUINT64VAL	258
#define	SINTVAL	259
#define	UINTVAL	260
#define	VOID	261
#define	BOOL	262
#define	SBYTE	263
#define	UBYTE	264
#define	SHORT	265
#define	USHORT	266
#define	INT	267
#define	UINT	268
#define	LONG	269
#define	ULONG	270
#define	FLOAT	271
#define	DOUBLE	272
#define	STRING	273
#define	TYPE	274
#define	LABEL	275
#define	VAR_ID	276
#define	LABELSTR	277
#define	STRINGCONSTANT	278
#define	IMPLEMENTATION	279
#define	TRUE	280
#define	FALSE	281
#define	BEGINTOK	282
#define	END	283
#define	DECLARE	284
#define	TO	285
#define	PHI	286
#define	CALL	287
#define	RET	288
#define	BR	289
#define	SWITCH	290
#define	NOT	291
#define	CAST	292
#define	ADD	293
#define	SUB	294
#define	MUL	295
#define	DIV	296
#define	REM	297
#define	SETLE	298
#define	SETGE	299
#define	SETLT	300
#define	SETGT	301
#define	SETEQ	302
#define	SETNE	303
#define	MALLOC	304
#define	ALLOCA	305
#define	FREE	306
#define	LOAD	307
#define	STORE	308
#define	GETFIELD	309
#define	PUTFIELD	310

#line 13 "llvmAsmParser.y"

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


#line 337 "llvmAsmParser.y"
typedef union {
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
} YYSTYPE;
#include <stdio.h>

#ifndef __cplusplus
#ifndef __STDC__
#define const
#endif
#endif



#define	YYFINAL		234
#define	YYFLAG		-32768
#define	YYNTBASE	67

#define YYTRANSLATE(x) ((unsigned)(x) <= 310 ? yytranslate[x] : 103)

static const char yytranslate[] = {     0,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,    64,
    65,    66,     2,    63,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
    57,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
    58,     2,    59,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,    60,
     2,     2,    61,     2,    62,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     2,     2,     2,     2,     2,
     2,     2,     2,     2,     2,     1,     3,     4,     5,     6,
     7,     8,     9,    10,    11,    12,    13,    14,    15,    16,
    17,    18,    19,    20,    21,    22,    23,    24,    25,    26,
    27,    28,    29,    30,    31,    32,    33,    34,    35,    36,
    37,    38,    39,    40,    41,    42,    43,    44,    45,    46,
    47,    48,    49,    50,    51,    52,    53,    54,    55,    56
};

#if YYDEBUG != 0
static const short yyprhs[] = {     0,
     0,     2,     4,     6,     8,    10,    12,    14,    16,    18,
    20,    22,    24,    26,    28,    30,    32,    34,    36,    38,
    40,    42,    44,    46,    48,    50,    52,    54,    56,    58,
    60,    62,    64,    66,    68,    70,    72,    74,    76,    78,
    80,    82,    84,    87,    88,    91,    94,    97,   100,   103,
   106,   113,   119,   128,   136,   143,   148,   152,   154,   158,
   159,   161,   164,   167,   169,   170,   173,   177,   179,   181,
   182,   188,   192,   195,   197,   199,   201,   203,   205,   207,
   209,   211,   213,   218,   222,   226,   232,   236,   239,   242,
   244,   248,   251,   254,   257,   261,   264,   265,   269,   272,
   276,   286,   296,   303,   309,   312,   319,   327,   330,   334,
   336,   337,   343,   347,   353,   356,   363,   365,   368,   374,
   377,   383
};

static const short yyrhs[] = {     5,
     0,     6,     0,     3,     0,     4,     0,     8,     0,     9,
     0,    10,     0,    11,     0,    12,     0,    13,     0,    14,
     0,    15,     0,    16,     0,    17,     0,    18,     0,    19,
     0,    20,     0,    21,     0,    69,     0,     7,     0,    37,
     0,    39,     0,    40,     0,    41,     0,    42,     0,    43,
     0,    44,     0,    45,     0,    46,     0,    47,     0,    48,
     0,    49,     0,    15,     0,    13,     0,    11,     0,     9,
     0,    16,     0,    14,     0,    12,     0,    10,     0,    73,
     0,    74,     0,    22,    57,     0,     0,    73,    68,     0,
    74,     4,     0,     8,    26,     0,     8,    27,     0,    19,
    24,     0,    20,    69,     0,    58,    69,    59,    58,    78,
    59,     0,    58,    69,    59,    58,    59,     0,    58,     4,
    60,    69,    59,    58,    78,    59,     0,    58,     4,    60,
    69,    59,    58,    59,     0,    61,    91,    62,    61,    78,
    62,     0,    61,    62,    61,    62,     0,    78,    63,    77,
     0,    77,     0,    79,    76,    77,     0,     0,    81,     0,
    81,    88,     0,    79,    25,     0,    22,     0,     0,    69,
    82,     0,    83,    63,    84,     0,    83,     0,    84,     0,
     0,    70,    24,    64,    85,    65,     0,    86,    79,    28,
     0,    92,    29,     0,     3,     0,     4,     0,    26,     0,
    27,     0,    24,     0,    67,     0,    22,     0,    89,     0,
    90,     0,    70,    64,    91,    65,     0,    70,    64,    65,
     0,    58,    69,    59,     0,    58,     4,    60,    69,    59,
     0,    61,    91,    62,     0,    61,    62,     0,    69,    66,
     0,    69,     0,    91,    63,    69,     0,    92,    93,     0,
    87,    93,     0,    94,    95,     0,    23,    94,    95,     0,
    94,    97,     0,     0,    34,    69,    90,     0,    34,     7,
     0,    35,    21,    90,     0,    35,     8,    90,    63,    21,
    90,    63,    21,    90,     0,    36,    75,    90,    63,    21,
    90,    58,    96,    59,     0,    96,    75,    89,    63,    21,
    90,     0,    75,    89,    63,    21,    90,     0,    76,   101,
     0,    69,    58,    90,    63,    90,    59,     0,    98,    63,
    58,    90,    63,    90,    59,     0,    69,    90,     0,    99,
    63,    90,     0,    99,     0,     0,    72,    69,    90,    63,
    90,     0,    71,    69,    90,     0,    38,    69,    90,    31,
    69,     0,    32,    98,     0,    33,    69,    90,    64,   100,
    65,     0,   102,     0,    50,    69,     0,    50,    69,    63,
    14,    90,     0,    51,    69,     0,    51,    69,    63,    14,
    90,     0,    52,    69,    90,     0
};

#endif

#if YYDEBUG != 0
static const short yyrline[] = { 0,
   432,   433,   440,   441,   452,   452,   452,   452,   452,   452,
   452,   453,   453,   453,   453,   453,   453,   453,   456,   456,
   461,   462,   462,   462,   462,   462,   463,   463,   463,   463,
   463,   463,   467,   467,   467,   467,   468,   468,   468,   468,
   469,   469,   471,   474,   478,   483,   488,   491,   494,   500,
   503,   516,   520,   538,   545,   553,   567,   570,   576,   584,
   595,   600,   605,   614,   614,   616,   624,   628,   633,   636,
   640,   667,   671,   680,   683,   686,   689,   692,   697,   700,
   703,   710,   718,   723,   727,   730,   733,   738,   741,   746,
   750,   755,   759,   768,   773,   782,   786,   790,   793,   796,
   799,   804,   815,   823,   833,   841,   846,   853,   857,   863,
   863,   865,   870,   875,   878,   889,   926,   930,   935,   944,
   949,   958
};
#endif


#if YYDEBUG != 0 || defined (YYERROR_VERBOSE)

static const char * const yytname[] = {   "$","error","$undefined.","ESINT64VAL",
"EUINT64VAL","SINTVAL","UINTVAL","VOID","BOOL","SBYTE","UBYTE","SHORT","USHORT",
"INT","UINT","LONG","ULONG","FLOAT","DOUBLE","STRING","TYPE","LABEL","VAR_ID",
"LABELSTR","STRINGCONSTANT","IMPLEMENTATION","TRUE","FALSE","BEGINTOK","END",
"DECLARE","TO","PHI","CALL","RET","BR","SWITCH","NOT","CAST","ADD","SUB","MUL",
"DIV","REM","SETLE","SETGE","SETLT","SETGT","SETEQ","SETNE","MALLOC","ALLOCA",
"FREE","LOAD","STORE","GETFIELD","PUTFIELD","'='","'['","']'","'x'","'{'","'}'",
"','","'('","')'","'*'","INTVAL","EINT64VAL","Types","TypesV","UnaryOps","BinaryOps",
"SIntType","UIntType","IntType","OptAssign","ConstVal","ConstVector","ConstPool",
"Module","MethodList","OptVAR_ID","ArgVal","ArgListH","ArgList","MethodHeaderH",
"MethodHeader","Method","ConstValueRef","ValueRef","TypeList","BasicBlockList",
"BasicBlock","InstructionList","BBTerminatorInst","JumpTable","Inst","PHIList",
"ValueRefList","ValueRefListE","InstVal","MemoryInst", NULL
};
#endif

static const short yyr1[] = {     0,
    67,    67,    68,    68,    69,    69,    69,    69,    69,    69,
    69,    69,    69,    69,    69,    69,    69,    69,    70,    70,
    71,    72,    72,    72,    72,    72,    72,    72,    72,    72,
    72,    72,    73,    73,    73,    73,    74,    74,    74,    74,
    75,    75,    76,    76,    77,    77,    77,    77,    77,    77,
    77,    77,    77,    77,    77,    77,    78,    78,    79,    79,
    80,    81,    81,    82,    82,    83,    84,    84,    85,    85,
    86,    87,    88,    89,    89,    89,    89,    89,    90,    90,
    90,    69,    69,    69,    69,    69,    69,    69,    69,    91,
    91,    92,    92,    93,    93,    94,    94,    95,    95,    95,
    95,    95,    96,    96,    97,    98,    98,    99,    99,   100,
   100,   101,   101,   101,   101,   101,   101,   102,   102,   102,
   102,   102
};

static const short yyr2[] = {     0,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     1,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     2,     0,     2,     2,     2,     2,     2,     2,
     6,     5,     8,     7,     6,     4,     3,     1,     3,     0,
     1,     2,     2,     1,     0,     2,     3,     1,     1,     0,
     5,     3,     2,     1,     1,     1,     1,     1,     1,     1,
     1,     1,     4,     3,     3,     5,     3,     2,     2,     1,
     3,     2,     2,     2,     3,     2,     0,     3,     2,     3,
     9,     9,     6,     5,     2,     6,     7,     2,     3,     1,
     0,     5,     3,     5,     2,     6,     1,     2,     5,     2,
     5,     3
};

static const short yydefact[] = {    60,
    44,    61,     0,    63,     0,    74,    75,     1,     2,    20,
     5,     6,     7,     8,     9,    10,    11,    12,    13,    14,
    15,    16,    17,    18,    80,    78,    76,    77,     0,     0,
    79,    19,     0,    60,    97,    62,    81,    82,    97,    43,
     0,    36,    40,    35,    39,    34,    38,    33,    37,     0,
     0,     0,     0,     0,     0,    59,    75,    19,     0,    88,
    90,     0,    89,     0,     0,    44,    97,    93,    44,    73,
    92,    47,    48,    49,    50,    75,    19,     0,     0,     3,
     4,    45,    46,     0,    85,    87,     0,    70,    84,     0,
    72,    44,     0,     0,     0,     0,    94,    96,     0,     0,
     0,     0,    19,    91,    65,    68,    69,     0,    83,    95,
    99,    19,     0,     0,    41,    42,     0,     0,     0,    21,
     0,    22,    23,    24,    25,    26,    27,    28,    29,    30,
    31,    32,     0,     0,     0,     0,     0,   105,   117,    19,
     0,    56,     0,    86,    64,    66,     0,    71,    98,     0,
   100,     0,    19,   115,    19,    19,   118,   120,    19,    19,
    19,     0,    52,    58,     0,     0,    67,     0,     0,     0,
     0,     0,     0,     0,     0,   122,   113,     0,     0,    51,
     0,    55,     0,     0,     0,     0,   111,     0,     0,     0,
     0,    54,     0,    57,     0,     0,     0,     0,    19,   110,
     0,   114,   119,   121,   112,    53,     0,     0,     0,     0,
   108,     0,   116,     0,     0,     0,   106,     0,   109,   101,
     0,   102,     0,   107,     0,     0,     0,     0,   104,     0,
   103,     0,     0,     0
};

static const short yydefgoto[] = {    31,
    82,    61,    59,   136,   137,    54,    55,   117,     5,   164,
   165,     1,   232,     2,   146,   106,   107,   108,    34,    35,
    36,    37,    38,    62,    39,    68,    69,    97,   216,    98,
   154,   200,   201,   138,   139
};

static const short yypact[] = {-32768,
   131,   319,   -53,-32768,    26,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,   378,   234,
-32768,   -13,   -21,-32768,    46,-32768,-32768,-32768,    85,-32768,
    53,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,    65,
   319,   403,   294,   126,   108,-32768,    60,   -11,    83,-32768,
    93,    76,-32768,    84,   209,   100,-32768,-32768,    32,-32768,
-32768,-32768,-32768,-32768,    93,    98,    -8,   121,    92,-32768,
-32768,-32768,-32768,   319,-32768,-32768,   319,   319,-32768,   103,
-32768,    32,   462,    36,   189,   239,-32768,-32768,   319,   111,
   125,   127,    22,    93,    -1,   129,-32768,   124,-32768,-32768,
   142,     4,   157,   157,-32768,-32768,   157,   319,   319,-32768,
   319,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,-32768,
-32768,-32768,   319,   319,   319,   319,   319,-32768,-32768,    24,
     3,-32768,    26,-32768,-32768,-32768,   319,-32768,-32768,   144,
-32768,   146,   -34,   147,     4,     4,    61,    80,     4,     4,
     4,   132,-32768,-32768,    33,   113,-32768,   211,   213,   157,
   199,   195,   231,   249,   250,-32768,-32768,   202,    91,-32768,
    26,-32768,   157,   157,   203,   157,   319,   319,   157,   157,
   157,-32768,    50,-32768,   205,   215,   157,   206,     4,   212,
   228,    93,-32768,-32768,-32768,-32768,   273,   189,   258,   157,
-32768,   157,-32768,   157,   170,    62,-32768,   260,-32768,-32768,
   279,-32768,   170,-32768,   323,   284,   157,   327,-32768,   157,
-32768,   349,   350,-32768
};

static const short yypgoto[] = {-32768,
-32768,    -2,   351,-32768,-32768,   -93,   -90,  -183,   -63,    -4,
  -123,   317,-32768,-32768,-32768,-32768,   207,-32768,-32768,-32768,
-32768,  -163,   -19,    -6,-32768,   318,   291,   267,-32768,-32768,
-32768,-32768,-32768,-32768,-32768
};


#define	YYLAST		523


static const short yytable[] = {    32,
    56,   115,    64,    40,   116,    96,     6,     7,     8,     9,
    41,    42,    43,    44,    45,    46,    47,    48,    49,   166,
   145,    50,    51,   170,   215,    25,    58,    26,    96,    27,
    28,    63,   223,    41,    42,    43,    44,    45,    46,    47,
    48,    49,    65,   113,    50,    51,    79,    85,    75,    77,
   100,   221,    63,     3,    63,   193,   114,    63,    90,   226,
    52,   163,   -19,    53,    63,    93,    94,    95,    67,    63,
    42,    43,    44,    45,    46,    47,    48,    49,    72,    73,
   144,   103,   162,    52,   104,   105,    53,    63,    74,    63,
   112,   180,   149,   150,   151,   181,   140,   152,    41,    42,
    43,    44,    45,    46,    47,    48,    49,    67,   206,    50,
    51,    83,   181,    70,   115,   153,   155,   116,   156,    84,
   222,     3,   115,   174,   -19,   116,    63,    91,    80,    81,
   157,   158,   159,   160,   161,   172,   173,    86,    87,   176,
   177,   178,   175,   -19,   105,    63,    65,    88,    52,   192,
   185,    53,     3,   102,    87,     4,   -19,    99,    63,     6,
     7,     8,     9,   195,   196,    87,   198,   109,   141,   203,
   204,   205,     6,     7,   182,   181,   194,   209,    25,   211,
    26,   101,    27,    28,   199,   202,   142,   143,   148,   179,
   218,   147,   219,    26,   220,    27,    28,    42,    43,    44,
    45,    46,    47,    48,    49,   -20,   168,   229,   169,   171,
   231,     6,     7,     8,     9,    10,    11,    12,    13,    14,
    15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
    25,   183,    26,   184,    27,    28,     6,     7,     8,     9,
    10,    11,    12,    13,    14,    15,    16,    17,    18,    19,
    20,    21,    22,    23,    24,    25,   186,    26,   187,    27,
    28,   188,   189,   190,   191,   197,    29,   207,   210,    30,
   118,   119,   208,    89,   212,   120,   121,   122,   123,   124,
   125,   126,   127,   128,   129,   130,   131,   132,   133,   134,
   135,    29,   213,   214,    30,    60,     6,     7,     8,     9,
    10,    11,    12,    13,    14,    15,    16,    17,    18,    19,
    20,    21,    22,    23,    24,    25,   217,    26,   224,    27,
    28,     6,     7,     8,     9,    10,    11,    12,    13,    14,
    15,    16,    17,    18,    19,    20,    21,    22,    23,    24,
    25,   225,    26,   227,    27,    28,   228,   230,   233,   234,
    66,    29,    33,   167,    30,    78,    71,    92,   110,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,    29,     0,     0,    30,
     6,    57,     8,     9,    10,    11,    12,    13,    14,    15,
    16,    17,    18,    19,    20,    21,    22,    23,    24,    25,
     0,    26,     0,    27,    28,     6,    76,     8,     9,    10,
    11,    12,    13,    14,    15,    16,    17,    18,    19,    20,
    21,    22,    23,    24,    25,     0,    26,     0,    27,    28,
     0,     0,     0,     0,     0,    29,     0,     0,    30,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
    29,     0,     0,    30,     6,     7,     8,     9,   111,    11,
    12,    13,    14,    15,    16,    17,    18,    19,    20,    21,
    22,    23,    24,    25,     0,    26,     0,    27,    28,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,     0,
     0,     0,     0,     0,     0,     0,     0,     0,     0,    29,
     0,     0,    30
};

static const short yycheck[] = {     2,
     5,    95,    24,    57,    95,    69,     3,     4,     5,     6,
     8,     9,    10,    11,    12,    13,    14,    15,    16,   143,
    22,    19,    20,    58,   208,    22,    29,    24,    92,    26,
    27,    66,   216,     8,     9,    10,    11,    12,    13,    14,
    15,    16,    64,     8,    19,    20,    53,    59,    51,    52,
    59,   215,    66,    22,    66,   179,    21,    66,    65,   223,
    58,    59,    64,    61,    66,    34,    35,    36,    23,    66,
     9,    10,    11,    12,    13,    14,    15,    16,    26,    27,
    59,    84,    59,    58,    87,    88,    61,    66,    24,    66,
    93,    59,   112,   113,   114,    63,    99,   117,     8,     9,
    10,    11,    12,    13,    14,    15,    16,    23,    59,    19,
    20,     4,    63,    29,   208,   118,   119,   208,   121,    60,
    59,    22,   216,    63,    64,   216,    66,    28,     3,     4,
   133,   134,   135,   136,   137,   155,   156,    62,    63,   159,
   160,   161,    63,    64,   147,    66,    64,    64,    58,    59,
   170,    61,    22,    62,    63,    25,    64,    60,    66,     3,
     4,     5,     6,   183,   184,    63,   186,    65,    58,   189,
   190,   191,     3,     4,    62,    63,   181,   197,    22,   199,
    24,    61,    26,    27,   187,   188,    62,    61,    65,    58,
   210,    63,   212,    24,   214,    26,    27,     9,    10,    11,
    12,    13,    14,    15,    16,    64,    63,   227,    63,    63,
   230,     3,     4,     5,     6,     7,     8,     9,    10,    11,
    12,    13,    14,    15,    16,    17,    18,    19,    20,    21,
    22,    21,    24,    21,    26,    27,     3,     4,     5,     6,
     7,     8,     9,    10,    11,    12,    13,    14,    15,    16,
    17,    18,    19,    20,    21,    22,    58,    24,    64,    26,
    27,    31,    14,    14,    63,    63,    58,    63,    63,    61,
    32,    33,    58,    65,    63,    37,    38,    39,    40,    41,
    42,    43,    44,    45,    46,    47,    48,    49,    50,    51,
    52,    58,    65,    21,    61,    62,     3,     4,     5,     6,
     7,     8,     9,    10,    11,    12,    13,    14,    15,    16,
    17,    18,    19,    20,    21,    22,    59,    24,    59,    26,
    27,     3,     4,     5,     6,     7,     8,     9,    10,    11,
    12,    13,    14,    15,    16,    17,    18,    19,    20,    21,
    22,    63,    24,    21,    26,    27,    63,    21,     0,     0,
    34,    58,     2,   147,    61,    62,    39,    67,    92,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    58,    -1,    -1,    61,
     3,     4,     5,     6,     7,     8,     9,    10,    11,    12,
    13,    14,    15,    16,    17,    18,    19,    20,    21,    22,
    -1,    24,    -1,    26,    27,     3,     4,     5,     6,     7,
     8,     9,    10,    11,    12,    13,    14,    15,    16,    17,
    18,    19,    20,    21,    22,    -1,    24,    -1,    26,    27,
    -1,    -1,    -1,    -1,    -1,    58,    -1,    -1,    61,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    58,    -1,    -1,    61,     3,     4,     5,     6,     7,     8,
     9,    10,    11,    12,    13,    14,    15,    16,    17,    18,
    19,    20,    21,    22,    -1,    24,    -1,    26,    27,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,
    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    -1,    58,
    -1,    -1,    61
};
/* -*-C-*-  Note some compilers choke on comments on `#line' lines.  */
#line 3 "/usr/dcs/software/supported/encap/bison-1.28/share/bison.simple"
/* This file comes from bison-1.28.  */

/* Skeleton output parser for bison,
   Copyright (C) 1984, 1989, 1990 Free Software Foundation, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2, or (at your option)
   any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place - Suite 330,
   Boston, MA 02111-1307, USA.  */

/* As a special exception, when this file is copied by Bison into a
   Bison output file, you may use that output file without restriction.
   This special exception was added by the Free Software Foundation
   in version 1.24 of Bison.  */

/* This is the parser code that is written into each bison parser
  when the %semantic_parser declaration is not specified in the grammar.
  It was written by Richard Stallman by simplifying the hairy parser
  used when %semantic_parser is specified.  */

#ifndef YYSTACK_USE_ALLOCA
#ifdef alloca
#define YYSTACK_USE_ALLOCA
#else /* alloca not defined */
#ifdef __GNUC__
#define YYSTACK_USE_ALLOCA
#define alloca __builtin_alloca
#else /* not GNU C.  */
#if (!defined (__STDC__) && defined (sparc)) || defined (__sparc__) || defined (__sparc) || defined (__sgi) || (defined (__sun) && defined (__i386))
#define YYSTACK_USE_ALLOCA
#include <alloca.h>
#else /* not sparc */
/* We think this test detects Watcom and Microsoft C.  */
/* This used to test MSDOS, but that is a bad idea
   since that symbol is in the user namespace.  */
#if (defined (_MSDOS) || defined (_MSDOS_)) && !defined (__TURBOC__)
#if 0 /* No need for malloc.h, which pollutes the namespace;
	 instead, just don't use alloca.  */
#include <malloc.h>
#endif
#else /* not MSDOS, or __TURBOC__ */
#if defined(_AIX)
/* I don't know what this was needed for, but it pollutes the namespace.
   So I turned it off.   rms, 2 May 1997.  */
/* #include <malloc.h>  */
 #pragma alloca
#define YYSTACK_USE_ALLOCA
#else /* not MSDOS, or __TURBOC__, or _AIX */
#if 0
#ifdef __hpux /* haible@ilog.fr says this works for HPUX 9.05 and up,
		 and on HPUX 10.  Eventually we can turn this on.  */
#define YYSTACK_USE_ALLOCA
#define alloca __builtin_alloca
#endif /* __hpux */
#endif
#endif /* not _AIX */
#endif /* not MSDOS, or __TURBOC__ */
#endif /* not sparc */
#endif /* not GNU C */
#endif /* alloca not defined */
#endif /* YYSTACK_USE_ALLOCA not defined */

#ifdef YYSTACK_USE_ALLOCA
#define YYSTACK_ALLOC alloca
#else
#define YYSTACK_ALLOC malloc
#endif

/* Note: there must be only one dollar sign in this file.
   It is replaced by the list of actions, each action
   as one case of the switch.  */

#define yyerrok		(yyerrstatus = 0)
#define yyclearin	(yychar = YYEMPTY)
#define YYEMPTY		-2
#define YYEOF		0
#define YYACCEPT	goto yyacceptlab
#define YYABORT 	goto yyabortlab
#define YYERROR		goto yyerrlab1
/* Like YYERROR except do call yyerror.
   This remains here temporarily to ease the
   transition to the new meaning of YYERROR, for GCC.
   Once GCC version 2 has supplanted version 1, this can go.  */
#define YYFAIL		goto yyerrlab
#define YYRECOVERING()  (!!yyerrstatus)
#define YYBACKUP(token, value) \
do								\
  if (yychar == YYEMPTY && yylen == 1)				\
    { yychar = (token), yylval = (value);			\
      yychar1 = YYTRANSLATE (yychar);				\
      YYPOPSTACK;						\
      goto yybackup;						\
    }								\
  else								\
    { yyerror ("syntax error: cannot back up"); YYERROR; }	\
while (0)

#define YYTERROR	1
#define YYERRCODE	256

#ifndef YYPURE
#define YYLEX		yylex()
#endif

#ifdef YYPURE
#ifdef YYLSP_NEEDED
#ifdef YYLEX_PARAM
#define YYLEX		yylex(&yylval, &yylloc, YYLEX_PARAM)
#else
#define YYLEX		yylex(&yylval, &yylloc)
#endif
#else /* not YYLSP_NEEDED */
#ifdef YYLEX_PARAM
#define YYLEX		yylex(&yylval, YYLEX_PARAM)
#else
#define YYLEX		yylex(&yylval)
#endif
#endif /* not YYLSP_NEEDED */
#endif

/* If nonreentrant, generate the variables here */

#ifndef YYPURE

int	yychar;			/*  the lookahead symbol		*/
YYSTYPE	yylval;			/*  the semantic value of the		*/
				/*  lookahead symbol			*/

#ifdef YYLSP_NEEDED
YYLTYPE yylloc;			/*  location data for the lookahead	*/
				/*  symbol				*/
#endif

int yynerrs;			/*  number of parse errors so far       */
#endif  /* not YYPURE */

#if YYDEBUG != 0
int yydebug;			/*  nonzero means print parse trace	*/
/* Since this is uninitialized, it does not stop multiple parsers
   from coexisting.  */
#endif

/*  YYINITDEPTH indicates the initial size of the parser's stacks	*/

#ifndef	YYINITDEPTH
#define YYINITDEPTH 200
#endif

/*  YYMAXDEPTH is the maximum size the stacks can grow to
    (effective only if the built-in stack extension method is used).  */

#if YYMAXDEPTH == 0
#undef YYMAXDEPTH
#endif

#ifndef YYMAXDEPTH
#define YYMAXDEPTH 10000
#endif

/* Define __yy_memcpy.  Note that the size argument
   should be passed with type unsigned int, because that is what the non-GCC
   definitions require.  With GCC, __builtin_memcpy takes an arg
   of type size_t, but it can handle unsigned int.  */

#if __GNUC__ > 1		/* GNU C and GNU C++ define this.  */
#define __yy_memcpy(TO,FROM,COUNT)	__builtin_memcpy(TO,FROM,COUNT)
#else				/* not GNU C or C++ */
#ifndef __cplusplus

/* This is the most reliable way to avoid incompatibilities
   in available built-in functions on various systems.  */
static void
__yy_memcpy (to, from, count)
     char *to;
     char *from;
     unsigned int count;
{
  register char *f = from;
  register char *t = to;
  register int i = count;

  while (i-- > 0)
    *t++ = *f++;
}

#else /* __cplusplus */

/* This is the most reliable way to avoid incompatibilities
   in available built-in functions on various systems.  */
static void
__yy_memcpy (char *to, char *from, unsigned int count)
{
  register char *t = to;
  register char *f = from;
  register int i = count;

  while (i-- > 0)
    *t++ = *f++;
}

#endif
#endif

#line 217 "/usr/dcs/software/supported/encap/bison-1.28/share/bison.simple"

/* The user can define YYPARSE_PARAM as the name of an argument to be passed
   into yyparse.  The argument should have type void *.
   It should actually point to an object.
   Grammar actions can access the variable by casting it
   to the proper pointer type.  */

#ifdef YYPARSE_PARAM
#ifdef __cplusplus
#define YYPARSE_PARAM_ARG void *YYPARSE_PARAM
#define YYPARSE_PARAM_DECL
#else /* not __cplusplus */
#define YYPARSE_PARAM_ARG YYPARSE_PARAM
#define YYPARSE_PARAM_DECL void *YYPARSE_PARAM;
#endif /* not __cplusplus */
#else /* not YYPARSE_PARAM */
#define YYPARSE_PARAM_ARG
#define YYPARSE_PARAM_DECL
#endif /* not YYPARSE_PARAM */

/* Prevent warning if -Wstrict-prototypes.  */
#ifdef __GNUC__
#ifdef YYPARSE_PARAM
int yyparse (void *);
#else
int yyparse (void);
#endif
#endif

int
yyparse(YYPARSE_PARAM_ARG)
     YYPARSE_PARAM_DECL
{
  register int yystate;
  register int yyn;
  register short *yyssp;
  register YYSTYPE *yyvsp;
  int yyerrstatus;	/*  number of tokens to shift before error messages enabled */
  int yychar1 = 0;		/*  lookahead token as an internal (translated) token number */

  short	yyssa[YYINITDEPTH];	/*  the state stack			*/
  YYSTYPE yyvsa[YYINITDEPTH];	/*  the semantic value stack		*/

  short *yyss = yyssa;		/*  refer to the stacks thru separate pointers */
  YYSTYPE *yyvs = yyvsa;	/*  to allow yyoverflow to reallocate them elsewhere */

#ifdef YYLSP_NEEDED
  YYLTYPE yylsa[YYINITDEPTH];	/*  the location stack			*/
  YYLTYPE *yyls = yylsa;
  YYLTYPE *yylsp;

#define YYPOPSTACK   (yyvsp--, yyssp--, yylsp--)
#else
#define YYPOPSTACK   (yyvsp--, yyssp--)
#endif

  int yystacksize = YYINITDEPTH;
  int yyfree_stacks = 0;

#ifdef YYPURE
  int yychar;
  YYSTYPE yylval;
  int yynerrs;
#ifdef YYLSP_NEEDED
  YYLTYPE yylloc;
#endif
#endif

  YYSTYPE yyval;		/*  the variable used to return		*/
				/*  semantic values from the action	*/
				/*  routines				*/

  int yylen;

#if YYDEBUG != 0
  if (yydebug)
    fprintf(stderr, "Starting parse\n");
#endif

  yystate = 0;
  yyerrstatus = 0;
  yynerrs = 0;
  yychar = YYEMPTY;		/* Cause a token to be read.  */

  /* Initialize stack pointers.
     Waste one element of value and location stack
     so that they stay on the same level as the state stack.
     The wasted elements are never initialized.  */

  yyssp = yyss - 1;
  yyvsp = yyvs;
#ifdef YYLSP_NEEDED
  yylsp = yyls;
#endif

/* Push a new state, which is found in  yystate  .  */
/* In all cases, when you get here, the value and location stacks
   have just been pushed. so pushing a state here evens the stacks.  */
yynewstate:

  *++yyssp = yystate;

  if (yyssp >= yyss + yystacksize - 1)
    {
      /* Give user a chance to reallocate the stack */
      /* Use copies of these so that the &'s don't force the real ones into memory. */
      YYSTYPE *yyvs1 = yyvs;
      short *yyss1 = yyss;
#ifdef YYLSP_NEEDED
      YYLTYPE *yyls1 = yyls;
#endif

      /* Get the current used size of the three stacks, in elements.  */
      int size = yyssp - yyss + 1;

#ifdef yyoverflow
      /* Each stack pointer address is followed by the size of
	 the data in use in that stack, in bytes.  */
#ifdef YYLSP_NEEDED
      /* This used to be a conditional around just the two extra args,
	 but that might be undefined if yyoverflow is a macro.  */
      yyoverflow("parser stack overflow",
		 &yyss1, size * sizeof (*yyssp),
		 &yyvs1, size * sizeof (*yyvsp),
		 &yyls1, size * sizeof (*yylsp),
		 &yystacksize);
#else
      yyoverflow("parser stack overflow",
		 &yyss1, size * sizeof (*yyssp),
		 &yyvs1, size * sizeof (*yyvsp),
		 &yystacksize);
#endif

      yyss = yyss1; yyvs = yyvs1;
#ifdef YYLSP_NEEDED
      yyls = yyls1;
#endif
#else /* no yyoverflow */
      /* Extend the stack our own way.  */
      if (yystacksize >= YYMAXDEPTH)
	{
	  yyerror("parser stack overflow");
	  if (yyfree_stacks)
	    {
	      free (yyss);
	      free (yyvs);
#ifdef YYLSP_NEEDED
	      free (yyls);
#endif
	    }
	  return 2;
	}
      yystacksize *= 2;
      if (yystacksize > YYMAXDEPTH)
	yystacksize = YYMAXDEPTH;
#ifndef YYSTACK_USE_ALLOCA
      yyfree_stacks = 1;
#endif
      yyss = (short *) YYSTACK_ALLOC (yystacksize * sizeof (*yyssp));
      __yy_memcpy ((char *)yyss, (char *)yyss1,
		   size * (unsigned int) sizeof (*yyssp));
      yyvs = (YYSTYPE *) YYSTACK_ALLOC (yystacksize * sizeof (*yyvsp));
      __yy_memcpy ((char *)yyvs, (char *)yyvs1,
		   size * (unsigned int) sizeof (*yyvsp));
#ifdef YYLSP_NEEDED
      yyls = (YYLTYPE *) YYSTACK_ALLOC (yystacksize * sizeof (*yylsp));
      __yy_memcpy ((char *)yyls, (char *)yyls1,
		   size * (unsigned int) sizeof (*yylsp));
#endif
#endif /* no yyoverflow */

      yyssp = yyss + size - 1;
      yyvsp = yyvs + size - 1;
#ifdef YYLSP_NEEDED
      yylsp = yyls + size - 1;
#endif

#if YYDEBUG != 0
      if (yydebug)
	fprintf(stderr, "Stack size increased to %d\n", yystacksize);
#endif

      if (yyssp >= yyss + yystacksize - 1)
	YYABORT;
    }

#if YYDEBUG != 0
  if (yydebug)
    fprintf(stderr, "Entering state %d\n", yystate);
#endif

  goto yybackup;
 yybackup:

/* Do appropriate processing given the current state.  */
/* Read a lookahead token if we need one and don't already have one.  */
/* yyresume: */

  /* First try to decide what to do without reference to lookahead token.  */

  yyn = yypact[yystate];
  if (yyn == YYFLAG)
    goto yydefault;

  /* Not known => get a lookahead token if don't already have one.  */

  /* yychar is either YYEMPTY or YYEOF
     or a valid token in external form.  */

  if (yychar == YYEMPTY)
    {
#if YYDEBUG != 0
      if (yydebug)
	fprintf(stderr, "Reading a token: ");
#endif
      yychar = YYLEX;
    }

  /* Convert token to internal form (in yychar1) for indexing tables with */

  if (yychar <= 0)		/* This means end of input. */
    {
      yychar1 = 0;
      yychar = YYEOF;		/* Don't call YYLEX any more */

#if YYDEBUG != 0
      if (yydebug)
	fprintf(stderr, "Now at end of input.\n");
#endif
    }
  else
    {
      yychar1 = YYTRANSLATE(yychar);

#if YYDEBUG != 0
      if (yydebug)
	{
	  fprintf (stderr, "Next token is %d (%s", yychar, yytname[yychar1]);
	  /* Give the individual parser a way to print the precise meaning
	     of a token, for further debugging info.  */
#ifdef YYPRINT
	  YYPRINT (stderr, yychar, yylval);
#endif
	  fprintf (stderr, ")\n");
	}
#endif
    }

  yyn += yychar1;
  if (yyn < 0 || yyn > YYLAST || yycheck[yyn] != yychar1)
    goto yydefault;

  yyn = yytable[yyn];

  /* yyn is what to do for this token type in this state.
     Negative => reduce, -yyn is rule number.
     Positive => shift, yyn is new state.
       New state is final state => don't bother to shift,
       just return success.
     0, or most negative number => error.  */

  if (yyn < 0)
    {
      if (yyn == YYFLAG)
	goto yyerrlab;
      yyn = -yyn;
      goto yyreduce;
    }
  else if (yyn == 0)
    goto yyerrlab;

  if (yyn == YYFINAL)
    YYACCEPT;

  /* Shift the lookahead token.  */

#if YYDEBUG != 0
  if (yydebug)
    fprintf(stderr, "Shifting token %d (%s), ", yychar, yytname[yychar1]);
#endif

  /* Discard the token being shifted unless it is eof.  */
  if (yychar != YYEOF)
    yychar = YYEMPTY;

  *++yyvsp = yylval;
#ifdef YYLSP_NEEDED
  *++yylsp = yylloc;
#endif

  /* count tokens shifted since error; after three, turn off error status.  */
  if (yyerrstatus) yyerrstatus--;

  yystate = yyn;
  goto yynewstate;

/* Do the default action for the current state.  */
yydefault:

  yyn = yydefact[yystate];
  if (yyn == 0)
    goto yyerrlab;

/* Do a reduction.  yyn is the number of a rule to reduce with.  */
yyreduce:
  yylen = yyr2[yyn];
  if (yylen > 0)
    yyval = yyvsp[1-yylen]; /* implement default value of the action */

#if YYDEBUG != 0
  if (yydebug)
    {
      int i;

      fprintf (stderr, "Reducing via rule %d (line %d), ",
	       yyn, yyrline[yyn]);

      /* Print the symbols being reduced, and their result.  */
      for (i = yyprhs[yyn]; yyrhs[i] > 0; i++)
	fprintf (stderr, "%s ", yytname[yyrhs[i]]);
      fprintf (stderr, " -> %s\n", yytname[yyr1[yyn]]);
    }
#endif


  switch (yyn) {

case 2:
#line 433 "llvmAsmParser.y"
{
  if (yyvsp[0].UIntVal > (uint32_t)INT32_MAX)     // Outside of my range!
    ThrowException("Value too large for type!");
  yyval.SIntVal = (int32_t)yyvsp[0].UIntVal;
;
    break;}
case 4:
#line 441 "llvmAsmParser.y"
{
  if (yyvsp[0].UInt64Val > (uint64_t)INT64_MAX)     // Outside of my range!
    ThrowException("Value too large for type!");
  yyval.SInt64Val = (int64_t)yyvsp[0].UInt64Val;
;
    break;}
case 43:
#line 471 "llvmAsmParser.y"
{
    yyval.StrVal = yyvsp[-1].StrVal;
  ;
    break;}
case 44:
#line 474 "llvmAsmParser.y"
{ 
    yyval.StrVal = 0; 
  ;
    break;}
case 45:
#line 478 "llvmAsmParser.y"
{     // integral constants
    if (!ConstPoolSInt::isValueValidForType(yyvsp[-1].TypeVal, yyvsp[0].SInt64Val))
      ThrowException("Constant value doesn't fit in type!");
    yyval.ConstVal = new ConstPoolSInt(yyvsp[-1].TypeVal, yyvsp[0].SInt64Val);
  ;
    break;}
case 46:
#line 483 "llvmAsmParser.y"
{           // integral constants
    if (!ConstPoolUInt::isValueValidForType(yyvsp[-1].TypeVal, yyvsp[0].UInt64Val))
      ThrowException("Constant value doesn't fit in type!");
    yyval.ConstVal = new ConstPoolUInt(yyvsp[-1].TypeVal, yyvsp[0].UInt64Val);
  ;
    break;}
case 47:
#line 488 "llvmAsmParser.y"
{                     // Boolean constants
    yyval.ConstVal = new ConstPoolBool(true);
  ;
    break;}
case 48:
#line 491 "llvmAsmParser.y"
{                    // Boolean constants
    yyval.ConstVal = new ConstPoolBool(false);
  ;
    break;}
case 49:
#line 494 "llvmAsmParser.y"
{         // String constants
    cerr << "FIXME: TODO: String constants [sbyte] not implemented yet!\n";
    abort();
    //$$ = new ConstPoolString($2);
    free(yyvsp[0].StrVal);
  ;
    break;}
case 50:
#line 500 "llvmAsmParser.y"
{                    // Type constants
    yyval.ConstVal = new ConstPoolType(yyvsp[0].TypeVal);
  ;
    break;}
case 51:
#line 503 "llvmAsmParser.y"
{      // Nonempty array constant
    // Verify all elements are correct type!
    const ArrayType *AT = ArrayType::getArrayType(yyvsp[-4].TypeVal);
    for (unsigned i = 0; i < yyvsp[-1].ConstVector->size(); i++) {
      if (yyvsp[-4].TypeVal != (*yyvsp[-1].ConstVector)[i]->getType())
	ThrowException("Element #" + utostr(i) + " is not of type '" + 
		       yyvsp[-4].TypeVal->getName() + "' as required!\nIt is of type '" +
		       (*yyvsp[-1].ConstVector)[i]->getType()->getName() + "'.");
    }

    yyval.ConstVal = new ConstPoolArray(AT, *yyvsp[-1].ConstVector);
    delete yyvsp[-1].ConstVector;
  ;
    break;}
case 52:
#line 516 "llvmAsmParser.y"
{                  // Empty array constant
    vector<ConstPoolVal*> Empty;
    yyval.ConstVal = new ConstPoolArray(ArrayType::getArrayType(yyvsp[-3].TypeVal), Empty);
  ;
    break;}
case 53:
#line 520 "llvmAsmParser.y"
{
    // Verify all elements are correct type!
    const ArrayType *AT = ArrayType::getArrayType(yyvsp[-4].TypeVal, (int)yyvsp[-6].UInt64Val);
    if (yyvsp[-6].UInt64Val != yyvsp[-1].ConstVector->size())
      ThrowException("Type mismatch: constant sized array initialized with " +
		     utostr(yyvsp[-1].ConstVector->size()) +  " arguments, but has size of " + 
		     itostr((int)yyvsp[-6].UInt64Val) + "!");

    for (unsigned i = 0; i < yyvsp[-1].ConstVector->size(); i++) {
      if (yyvsp[-4].TypeVal != (*yyvsp[-1].ConstVector)[i]->getType())
	ThrowException("Element #" + utostr(i) + " is not of type '" + 
		       yyvsp[-4].TypeVal->getName() + "' as required!\nIt is of type '" +
		       (*yyvsp[-1].ConstVector)[i]->getType()->getName() + "'.");
    }

    yyval.ConstVal = new ConstPoolArray(AT, *yyvsp[-1].ConstVector);
    delete yyvsp[-1].ConstVector;
  ;
    break;}
case 54:
#line 538 "llvmAsmParser.y"
{
    if (yyvsp[-5].UInt64Val != 0) 
      ThrowException("Type mismatch: constant sized array initialized with 0"
		     " arguments, but has size of " + itostr((int)yyvsp[-5].UInt64Val) + "!");
    vector<ConstPoolVal*> Empty;
    yyval.ConstVal = new ConstPoolArray(ArrayType::getArrayType(yyvsp[-3].TypeVal, 0), Empty);
  ;
    break;}
case 55:
#line 545 "llvmAsmParser.y"
{
    StructType::ElementTypes Types(yyvsp[-4].TypeList->begin(), yyvsp[-4].TypeList->end());
    delete yyvsp[-4].TypeList;

    const StructType *St = StructType::getStructType(Types);
    yyval.ConstVal = new ConstPoolStruct(St, *yyvsp[-1].ConstVector);
    delete yyvsp[-1].ConstVector;
  ;
    break;}
case 56:
#line 553 "llvmAsmParser.y"
{
    const StructType *St = 
      StructType::getStructType(StructType::ElementTypes());
    vector<ConstPoolVal*> Empty;
    yyval.ConstVal = new ConstPoolStruct(St, Empty);
  ;
    break;}
case 57:
#line 567 "llvmAsmParser.y"
{
    (yyval.ConstVector = yyvsp[-2].ConstVector)->push_back(addConstValToConstantPool(yyvsp[0].ConstVal));
  ;
    break;}
case 58:
#line 570 "llvmAsmParser.y"
{
    yyval.ConstVector = new vector<ConstPoolVal*>();
    yyval.ConstVector->push_back(addConstValToConstantPool(yyvsp[0].ConstVal));
  ;
    break;}
case 59:
#line 576 "llvmAsmParser.y"
{ 
    if (yyvsp[-1].StrVal) {
      yyvsp[0].ConstVal->setName(yyvsp[-1].StrVal);
      free(yyvsp[-1].StrVal);
    }

    addConstValToConstantPool(yyvsp[0].ConstVal);
  ;
    break;}
case 60:
#line 584 "llvmAsmParser.y"
{ 
  ;
    break;}
case 61:
#line 595 "llvmAsmParser.y"
{
  yyval.ModuleVal = ParserResult = yyvsp[0].ModuleVal;
  CurModule.ModuleDone();
;
    break;}
case 62:
#line 600 "llvmAsmParser.y"
{
    yyvsp[-1].ModuleVal->getMethodList().push_back(yyvsp[0].MethodVal);
    CurMeth.MethodDone();
    yyval.ModuleVal = yyvsp[-1].ModuleVal;
  ;
    break;}
case 63:
#line 605 "llvmAsmParser.y"
{
    yyval.ModuleVal = CurModule.CurrentModule;
  ;
    break;}
case 65:
#line 614 "llvmAsmParser.y"
{ yyval.StrVal = 0; ;
    break;}
case 66:
#line 616 "llvmAsmParser.y"
{
  yyval.MethArgVal = new MethodArgument(yyvsp[-1].TypeVal);
  if (yyvsp[0].StrVal) {      // Was the argument named?
    yyval.MethArgVal->setName(yyvsp[0].StrVal); 
    free(yyvsp[0].StrVal);    // The string was strdup'd, so free it now.
  }
;
    break;}
case 67:
#line 624 "llvmAsmParser.y"
{
    yyval.MethodArgList = yyvsp[0].MethodArgList;
    yyvsp[0].MethodArgList->push_front(yyvsp[-2].MethArgVal);
  ;
    break;}
case 68:
#line 628 "llvmAsmParser.y"
{
    yyval.MethodArgList = new list<MethodArgument*>();
    yyval.MethodArgList->push_front(yyvsp[0].MethArgVal);
  ;
    break;}
case 69:
#line 633 "llvmAsmParser.y"
{
    yyval.MethodArgList = yyvsp[0].MethodArgList;
  ;
    break;}
case 70:
#line 636 "llvmAsmParser.y"
{
    yyval.MethodArgList = 0;
  ;
    break;}
case 71:
#line 640 "llvmAsmParser.y"
{
  MethodType::ParamTypes ParamTypeList;
  if (yyvsp[-1].MethodArgList)
    for (list<MethodArgument*>::iterator I = yyvsp[-1].MethodArgList->begin(); I != yyvsp[-1].MethodArgList->end(); ++I)
      ParamTypeList.push_back((*I)->getType());

  const MethodType *MT = MethodType::getMethodType(yyvsp[-4].TypeVal, ParamTypeList);

  Method *M = new Method(MT, yyvsp[-3].StrVal);
  free(yyvsp[-3].StrVal);  // Free strdup'd memory!

  InsertValue(M, CurModule.Values);

  CurMeth.MethodStart(M);

  // Add all of the arguments we parsed to the method...
  if (yyvsp[-1].MethodArgList) {        // Is null if empty...
    Method::ArgumentListType &ArgList = M->getArgumentList();

    for (list<MethodArgument*>::iterator I = yyvsp[-1].MethodArgList->begin(); I != yyvsp[-1].MethodArgList->end(); ++I) {
      InsertValue(*I);
      ArgList.push_back(*I);
    }
    delete yyvsp[-1].MethodArgList;                     // We're now done with the argument list
  }
;
    break;}
case 72:
#line 667 "llvmAsmParser.y"
{
  yyval.MethodVal = CurMeth.CurrentMethod;
;
    break;}
case 73:
#line 671 "llvmAsmParser.y"
{
  yyval.MethodVal = yyvsp[-1].MethodVal;
;
    break;}
case 74:
#line 680 "llvmAsmParser.y"
{    // A reference to a direct constant
    yyval.ValIDVal = ValID::create(yyvsp[0].SInt64Val);
  ;
    break;}
case 75:
#line 683 "llvmAsmParser.y"
{
    yyval.ValIDVal = ValID::create(yyvsp[0].UInt64Val);
  ;
    break;}
case 76:
#line 686 "llvmAsmParser.y"
{
    yyval.ValIDVal = ValID::create((int64_t)1);
  ;
    break;}
case 77:
#line 689 "llvmAsmParser.y"
{
    yyval.ValIDVal = ValID::create((int64_t)0);
  ;
    break;}
case 78:
#line 692 "llvmAsmParser.y"
{        // Quoted strings work too... especially for methods
    yyval.ValIDVal = ValID::create_conststr(yyvsp[0].StrVal);
  ;
    break;}
case 79:
#line 697 "llvmAsmParser.y"
{           // Is it an integer reference...?
    yyval.ValIDVal = ValID::create(yyvsp[0].SIntVal);
  ;
    break;}
case 80:
#line 700 "llvmAsmParser.y"
{                // It must be a named reference then...
    yyval.ValIDVal = ValID::create(yyvsp[0].StrVal);
  ;
    break;}
case 81:
#line 703 "llvmAsmParser.y"
{
    yyval.ValIDVal = yyvsp[0].ValIDVal;
  ;
    break;}
case 82:
#line 710 "llvmAsmParser.y"
{
    Value *D = getVal(Type::TypeTy, yyvsp[0].ValIDVal, true);
    if (D == 0) ThrowException("Invalid user defined type: " + yyvsp[0].ValIDVal.getName());

    // User defined type not in const pool!
    ConstPoolType *CPT = (ConstPoolType*)D->castConstantAsserting();
    yyval.TypeVal = CPT->getValue();
  ;
    break;}
case 83:
#line 718 "llvmAsmParser.y"
{               // Method derived type?
    MethodType::ParamTypes Params(yyvsp[-1].TypeList->begin(), yyvsp[-1].TypeList->end());
    delete yyvsp[-1].TypeList;
    yyval.TypeVal = MethodType::getMethodType(yyvsp[-3].TypeVal, Params);
  ;
    break;}
case 84:
#line 723 "llvmAsmParser.y"
{               // Method derived type?
    MethodType::ParamTypes Params;     // Empty list
    yyval.TypeVal = MethodType::getMethodType(yyvsp[-2].TypeVal, Params);
  ;
    break;}
case 85:
#line 727 "llvmAsmParser.y"
{
    yyval.TypeVal = ArrayType::getArrayType(yyvsp[-1].TypeVal);
  ;
    break;}
case 86:
#line 730 "llvmAsmParser.y"
{
    yyval.TypeVal = ArrayType::getArrayType(yyvsp[-1].TypeVal, (int)yyvsp[-3].UInt64Val);
  ;
    break;}
case 87:
#line 733 "llvmAsmParser.y"
{
    StructType::ElementTypes Elements(yyvsp[-1].TypeList->begin(), yyvsp[-1].TypeList->end());
    delete yyvsp[-1].TypeList;
    yyval.TypeVal = StructType::getStructType(Elements);
  ;
    break;}
case 88:
#line 738 "llvmAsmParser.y"
{
    yyval.TypeVal = StructType::getStructType(StructType::ElementTypes());
  ;
    break;}
case 89:
#line 741 "llvmAsmParser.y"
{
    yyval.TypeVal = PointerType::getPointerType(yyvsp[-1].TypeVal);
  ;
    break;}
case 90:
#line 746 "llvmAsmParser.y"
{
    yyval.TypeList = new list<const Type*>();
    yyval.TypeList->push_back(yyvsp[0].TypeVal);
  ;
    break;}
case 91:
#line 750 "llvmAsmParser.y"
{
    (yyval.TypeList=yyvsp[-2].TypeList)->push_back(yyvsp[0].TypeVal);
  ;
    break;}
case 92:
#line 755 "llvmAsmParser.y"
{
    yyvsp[-1].MethodVal->getBasicBlocks().push_back(yyvsp[0].BasicBlockVal);
    yyval.MethodVal = yyvsp[-1].MethodVal;
  ;
    break;}
case 93:
#line 759 "llvmAsmParser.y"
{ // Do not allow methods with 0 basic blocks   
    yyval.MethodVal = yyvsp[-1].MethodVal;                  // in them...
    yyvsp[-1].MethodVal->getBasicBlocks().push_back(yyvsp[0].BasicBlockVal);
  ;
    break;}
case 94:
#line 768 "llvmAsmParser.y"
{
    yyvsp[-1].BasicBlockVal->getInstList().push_back(yyvsp[0].TermInstVal);
    InsertValue(yyvsp[-1].BasicBlockVal);
    yyval.BasicBlockVal = yyvsp[-1].BasicBlockVal;
  ;
    break;}
case 95:
#line 773 "llvmAsmParser.y"
{
    yyvsp[-1].BasicBlockVal->getInstList().push_back(yyvsp[0].TermInstVal);
    yyvsp[-1].BasicBlockVal->setName(yyvsp[-2].StrVal);
    free(yyvsp[-2].StrVal);         // Free the strdup'd memory...

    InsertValue(yyvsp[-1].BasicBlockVal);
    yyval.BasicBlockVal = yyvsp[-1].BasicBlockVal;
  ;
    break;}
case 96:
#line 782 "llvmAsmParser.y"
{
    yyvsp[-1].BasicBlockVal->getInstList().push_back(yyvsp[0].InstVal);
    yyval.BasicBlockVal = yyvsp[-1].BasicBlockVal;
  ;
    break;}
case 97:
#line 786 "llvmAsmParser.y"
{
    yyval.BasicBlockVal = new BasicBlock();
  ;
    break;}
case 98:
#line 790 "llvmAsmParser.y"
{              // Return with a result...
    yyval.TermInstVal = new ReturnInst(getVal(yyvsp[-1].TypeVal, yyvsp[0].ValIDVal));
  ;
    break;}
case 99:
#line 793 "llvmAsmParser.y"
{                                       // Return with no result...
    yyval.TermInstVal = new ReturnInst();
  ;
    break;}
case 100:
#line 796 "llvmAsmParser.y"
{                         // Unconditional Branch...
    yyval.TermInstVal = new BranchInst((BasicBlock*)getVal(Type::LabelTy, yyvsp[0].ValIDVal));
  ;
    break;}
case 101:
#line 799 "llvmAsmParser.y"
{  
    yyval.TermInstVal = new BranchInst((BasicBlock*)getVal(Type::LabelTy, yyvsp[-3].ValIDVal), 
			(BasicBlock*)getVal(Type::LabelTy, yyvsp[0].ValIDVal),
			getVal(Type::BoolTy, yyvsp[-6].ValIDVal));
  ;
    break;}
case 102:
#line 804 "llvmAsmParser.y"
{
    SwitchInst *S = new SwitchInst(getVal(yyvsp[-7].TypeVal, yyvsp[-6].ValIDVal), 
                                   (BasicBlock*)getVal(Type::LabelTy, yyvsp[-3].ValIDVal));
    yyval.TermInstVal = S;

    list<pair<ConstPoolVal*, BasicBlock*> >::iterator I = yyvsp[-1].JumpTable->begin(), 
                                                      end = yyvsp[-1].JumpTable->end();
    for (; I != end; ++I)
      S->dest_push_back(I->first, I->second);
  ;
    break;}
case 103:
#line 815 "llvmAsmParser.y"
{
    yyval.JumpTable = yyvsp[-5].JumpTable;
    ConstPoolVal *V = (ConstPoolVal*)getVal(yyvsp[-4].TypeVal, yyvsp[-3].ValIDVal, true);
    if (V == 0)
      ThrowException("May only switch on a constant pool value!");

    yyval.JumpTable->push_back(make_pair(V, (BasicBlock*)getVal(yyvsp[-1].TypeVal, yyvsp[0].ValIDVal)));
  ;
    break;}
case 104:
#line 823 "llvmAsmParser.y"
{
    yyval.JumpTable = new list<pair<ConstPoolVal*, BasicBlock*> >();
    ConstPoolVal *V = (ConstPoolVal*)getVal(yyvsp[-4].TypeVal, yyvsp[-3].ValIDVal, true);

    if (V == 0)
      ThrowException("May only switch on a constant pool value!");

    yyval.JumpTable->push_back(make_pair(V, (BasicBlock*)getVal(yyvsp[-1].TypeVal, yyvsp[0].ValIDVal)));
  ;
    break;}
case 105:
#line 833 "llvmAsmParser.y"
{
  if (yyvsp[-1].StrVal)              // Is this definition named??
    yyvsp[0].InstVal->setName(yyvsp[-1].StrVal);   // if so, assign the name...

  InsertValue(yyvsp[0].InstVal);
  yyval.InstVal = yyvsp[0].InstVal;
;
    break;}
case 106:
#line 841 "llvmAsmParser.y"
{    // Used for PHI nodes
    yyval.PHIList = new list<pair<Value*, BasicBlock*> >();
    yyval.PHIList->push_back(make_pair(getVal(yyvsp[-5].TypeVal, yyvsp[-3].ValIDVal), 
			    (BasicBlock*)getVal(Type::LabelTy, yyvsp[-1].ValIDVal)));
  ;
    break;}
case 107:
#line 846 "llvmAsmParser.y"
{
    yyval.PHIList = yyvsp[-6].PHIList;
    yyvsp[-6].PHIList->push_back(make_pair(getVal(yyvsp[-6].PHIList->front().first->getType(), yyvsp[-3].ValIDVal),
			    (BasicBlock*)getVal(Type::LabelTy, yyvsp[-1].ValIDVal)));
  ;
    break;}
case 108:
#line 853 "llvmAsmParser.y"
{    // Used for call statements...
    yyval.ValueList = new list<Value*>();
    yyval.ValueList->push_back(getVal(yyvsp[-1].TypeVal, yyvsp[0].ValIDVal));
  ;
    break;}
case 109:
#line 857 "llvmAsmParser.y"
{
    yyval.ValueList = yyvsp[-2].ValueList;
    yyvsp[-2].ValueList->push_back(getVal(yyvsp[-2].ValueList->front()->getType(), yyvsp[0].ValIDVal));
  ;
    break;}
case 111:
#line 863 "llvmAsmParser.y"
{ yyval.ValueList = 0; ;
    break;}
case 112:
#line 865 "llvmAsmParser.y"
{
    yyval.InstVal = BinaryOperator::create(yyvsp[-4].BinaryOpVal, getVal(yyvsp[-3].TypeVal, yyvsp[-2].ValIDVal), getVal(yyvsp[-3].TypeVal, yyvsp[0].ValIDVal));
    if (yyval.InstVal == 0)
      ThrowException("binary operator returned null!");
  ;
    break;}
case 113:
#line 870 "llvmAsmParser.y"
{
    yyval.InstVal = UnaryOperator::create(yyvsp[-2].UnaryOpVal, getVal(yyvsp[-1].TypeVal, yyvsp[0].ValIDVal));
    if (yyval.InstVal == 0)
      ThrowException("unary operator returned null!");
  ;
    break;}
case 114:
#line 875 "llvmAsmParser.y"
{
    yyval.InstVal = UnaryOperator::create(yyvsp[-4].UnaryOpVal, getVal(yyvsp[-3].TypeVal, yyvsp[-2].ValIDVal), yyvsp[0].TypeVal);
  ;
    break;}
case 115:
#line 878 "llvmAsmParser.y"
{
    const Type *Ty = yyvsp[0].PHIList->front().first->getType();
    yyval.InstVal = new PHINode(Ty);
    while (yyvsp[0].PHIList->begin() != yyvsp[0].PHIList->end()) {
      if (yyvsp[0].PHIList->front().first->getType() != Ty) 
	ThrowException("All elements of a PHI node must be of the same type!");
      ((PHINode*)yyval.InstVal)->addIncoming(yyvsp[0].PHIList->front().first, yyvsp[0].PHIList->front().second);
      yyvsp[0].PHIList->pop_front();
    }
    delete yyvsp[0].PHIList;  // Free the list...
  ;
    break;}
case 116:
#line 889 "llvmAsmParser.y"
{
    if (!yyvsp[-4].TypeVal->isMethodType())
      ThrowException("Can only call methods: invalid type '" + 
		     yyvsp[-4].TypeVal->getName() + "'!");

    const MethodType *Ty = (const MethodType*)yyvsp[-4].TypeVal;

    Value *V = getVal(Ty, yyvsp[-3].ValIDVal);
    if (!V->isMethod() || V->getType() != Ty)
      ThrowException("Cannot call: " + yyvsp[-3].ValIDVal.getName() + "!");

    // Create or access a new type that corresponds to the function call...
    vector<Value *> Params;

    if (yyvsp[-1].ValueList) {
      // Pull out just the arguments...
      Params.insert(Params.begin(), yyvsp[-1].ValueList->begin(), yyvsp[-1].ValueList->end());
      delete yyvsp[-1].ValueList;

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
    yyval.InstVal = new CallInst((Method*)V, Params);
  ;
    break;}
case 117:
#line 926 "llvmAsmParser.y"
{
    yyval.InstVal = yyvsp[0].InstVal;
  ;
    break;}
case 118:
#line 930 "llvmAsmParser.y"
{
    const Type *Ty = PointerType::getPointerType(yyvsp[0].TypeVal);
    addConstValToConstantPool(new ConstPoolType(Ty));
    yyval.InstVal = new MallocInst(Ty);
  ;
    break;}
case 119:
#line 935 "llvmAsmParser.y"
{
    if (!yyvsp[-3].TypeVal->isArrayType() || ((const ArrayType*)yyvsp[-3].TypeVal)->isSized())
      ThrowException("Trying to allocate " + yyvsp[-3].TypeVal->getName() + 
		     " as unsized array!");
    const Type *Ty = PointerType::getPointerType(yyvsp[-3].TypeVal);
    addConstValToConstantPool(new ConstPoolType(Ty));
    Value *ArrSize = getVal(yyvsp[-1].TypeVal, yyvsp[0].ValIDVal);
    yyval.InstVal = new MallocInst(Ty, ArrSize);
  ;
    break;}
case 120:
#line 944 "llvmAsmParser.y"
{
    const Type *Ty = PointerType::getPointerType(yyvsp[0].TypeVal);
    addConstValToConstantPool(new ConstPoolType(Ty));
    yyval.InstVal = new AllocaInst(Ty);
  ;
    break;}
case 121:
#line 949 "llvmAsmParser.y"
{
    if (!yyvsp[-3].TypeVal->isArrayType() || ((const ArrayType*)yyvsp[-3].TypeVal)->isSized())
      ThrowException("Trying to allocate " + yyvsp[-3].TypeVal->getName() + 
		     " as unsized array!");
    const Type *Ty = PointerType::getPointerType(yyvsp[-3].TypeVal);
    addConstValToConstantPool(new ConstPoolType(Ty));
    Value *ArrSize = getVal(yyvsp[-1].TypeVal, yyvsp[0].ValIDVal);
    yyval.InstVal = new AllocaInst(Ty, ArrSize);
  ;
    break;}
case 122:
#line 958 "llvmAsmParser.y"
{
    if (!yyvsp[-1].TypeVal->isPointerType())
      ThrowException("Trying to free nonpointer type " + yyvsp[-1].TypeVal->getName() + "!");
    yyval.InstVal = new FreeInst(getVal(yyvsp[-1].TypeVal, yyvsp[0].ValIDVal));
  ;
    break;}
}
   /* the action file gets copied in in place of this dollarsign */
#line 543 "/usr/dcs/software/supported/encap/bison-1.28/share/bison.simple"

  yyvsp -= yylen;
  yyssp -= yylen;
#ifdef YYLSP_NEEDED
  yylsp -= yylen;
#endif

#if YYDEBUG != 0
  if (yydebug)
    {
      short *ssp1 = yyss - 1;
      fprintf (stderr, "state stack now");
      while (ssp1 != yyssp)
	fprintf (stderr, " %d", *++ssp1);
      fprintf (stderr, "\n");
    }
#endif

  *++yyvsp = yyval;

#ifdef YYLSP_NEEDED
  yylsp++;
  if (yylen == 0)
    {
      yylsp->first_line = yylloc.first_line;
      yylsp->first_column = yylloc.first_column;
      yylsp->last_line = (yylsp-1)->last_line;
      yylsp->last_column = (yylsp-1)->last_column;
      yylsp->text = 0;
    }
  else
    {
      yylsp->last_line = (yylsp+yylen-1)->last_line;
      yylsp->last_column = (yylsp+yylen-1)->last_column;
    }
#endif

  /* Now "shift" the result of the reduction.
     Determine what state that goes to,
     based on the state we popped back to
     and the rule number reduced by.  */

  yyn = yyr1[yyn];

  yystate = yypgoto[yyn - YYNTBASE] + *yyssp;
  if (yystate >= 0 && yystate <= YYLAST && yycheck[yystate] == *yyssp)
    yystate = yytable[yystate];
  else
    yystate = yydefgoto[yyn - YYNTBASE];

  goto yynewstate;

yyerrlab:   /* here on detecting error */

  if (! yyerrstatus)
    /* If not already recovering from an error, report this error.  */
    {
      ++yynerrs;

#ifdef YYERROR_VERBOSE
      yyn = yypact[yystate];

      if (yyn > YYFLAG && yyn < YYLAST)
	{
	  int size = 0;
	  char *msg;
	  int x, count;

	  count = 0;
	  /* Start X at -yyn if nec to avoid negative indexes in yycheck.  */
	  for (x = (yyn < 0 ? -yyn : 0);
	       x < (sizeof(yytname) / sizeof(char *)); x++)
	    if (yycheck[x + yyn] == x)
	      size += strlen(yytname[x]) + 15, count++;
	  msg = (char *) malloc(size + 15);
	  if (msg != 0)
	    {
	      strcpy(msg, "parse error");

	      if (count < 5)
		{
		  count = 0;
		  for (x = (yyn < 0 ? -yyn : 0);
		       x < (sizeof(yytname) / sizeof(char *)); x++)
		    if (yycheck[x + yyn] == x)
		      {
			strcat(msg, count == 0 ? ", expecting `" : " or `");
			strcat(msg, yytname[x]);
			strcat(msg, "'");
			count++;
		      }
		}
	      yyerror(msg);
	      free(msg);
	    }
	  else
	    yyerror ("parse error; also virtual memory exceeded");
	}
      else
#endif /* YYERROR_VERBOSE */
	yyerror("parse error");
    }

  goto yyerrlab1;
yyerrlab1:   /* here on error raised explicitly by an action */

  if (yyerrstatus == 3)
    {
      /* if just tried and failed to reuse lookahead token after an error, discard it.  */

      /* return failure if at end of input */
      if (yychar == YYEOF)
	YYABORT;

#if YYDEBUG != 0
      if (yydebug)
	fprintf(stderr, "Discarding token %d (%s).\n", yychar, yytname[yychar1]);
#endif

      yychar = YYEMPTY;
    }

  /* Else will try to reuse lookahead token
     after shifting the error token.  */

  yyerrstatus = 3;		/* Each real token shifted decrements this */

  goto yyerrhandle;

yyerrdefault:  /* current state does not do anything special for the error token. */

#if 0
  /* This is wrong; only states that explicitly want error tokens
     should shift them.  */
  yyn = yydefact[yystate];  /* If its default is to accept any token, ok.  Otherwise pop it.*/
  if (yyn) goto yydefault;
#endif

yyerrpop:   /* pop the current state because it cannot handle the error token */

  if (yyssp == yyss) YYABORT;
  yyvsp--;
  yystate = *--yyssp;
#ifdef YYLSP_NEEDED
  yylsp--;
#endif

#if YYDEBUG != 0
  if (yydebug)
    {
      short *ssp1 = yyss - 1;
      fprintf (stderr, "Error: state stack now");
      while (ssp1 != yyssp)
	fprintf (stderr, " %d", *++ssp1);
      fprintf (stderr, "\n");
    }
#endif

yyerrhandle:

  yyn = yypact[yystate];
  if (yyn == YYFLAG)
    goto yyerrdefault;

  yyn += YYTERROR;
  if (yyn < 0 || yyn > YYLAST || yycheck[yyn] != YYTERROR)
    goto yyerrdefault;

  yyn = yytable[yyn];
  if (yyn < 0)
    {
      if (yyn == YYFLAG)
	goto yyerrpop;
      yyn = -yyn;
      goto yyreduce;
    }
  else if (yyn == 0)
    goto yyerrpop;

  if (yyn == YYFINAL)
    YYACCEPT;

#if YYDEBUG != 0
  if (yydebug)
    fprintf(stderr, "Shifting error token, ");
#endif

  *++yyvsp = yylval;
#ifdef YYLSP_NEEDED
  *++yylsp = yylloc;
#endif

  yystate = yyn;
  goto yynewstate;

 yyacceptlab:
  /* YYACCEPT comes here.  */
  if (yyfree_stacks)
    {
      free (yyss);
      free (yyvs);
#ifdef YYLSP_NEEDED
      free (yyls);
#endif
    }
  return 0;

 yyabortlab:
  /* YYABORT comes here.  */
  if (yyfree_stacks)
    {
      free (yyss);
      free (yyvs);
#ifdef YYLSP_NEEDED
      free (yyls);
#endif
    }
  return 1;
}
#line 964 "llvmAsmParser.y"

int yyerror(const char *ErrorMsg) {
  ThrowException(string("Parse error: ") + ErrorMsg);
  return 0;
}
