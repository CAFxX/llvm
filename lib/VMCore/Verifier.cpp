//===-- Verifier.cpp - Implement the Module Verifier -------------*- C++ -*-==//
//
// This file defines the method verifier interface, that can be used for some
// sanity checking of input to the system.
//
// Note that this does not provide full 'java style' security and verifications,
// instead it just tries to ensure that code is well formed.
//
//  . There are no duplicated names in a symbol table... ie there !exist a val
//    with the same name as something in the symbol table, but with a different
//    address as what is in the symbol table...
//  . Both of a binary operator's parameters are the same type
//  . Only PHI nodes can refer to themselves
//  . All of the constants in a switch statement are of the correct type
//  . The code is in valid SSA form
//  . It should be illegal to put a label into any other type (like a structure)
//    or to return one. [except constant arrays!]
//  . Right now 'add bool 0, 0' is valid.  This isn't particularly good.
//  . Only phi nodes can be self referential: 'add int 0, 0 ; <int>:0' is bad
//  . All other things that are tested by asserts spread about the code...
//  . All basic blocks should only end with terminator insts, not contain them
//  . All methods must have >= 1 basic block
//  . Verify that none of the Def getType()'s are null.
//  . Method's cannot take a void typed parameter
//  . Verify that a method's argument list agrees with it's declared type.
//  . Verify that arrays and structures have fixed elements: No unsized arrays.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/Verifier.h"
#include "llvm/ConstantPool.h"
#include "llvm/Method.h"
#include "llvm/Module.h"
#include "llvm/BasicBlock.h"
#include "llvm/Type.h"

// Error - Define a macro to do the common task of pushing a message onto the
// end of the error list and setting Bad to true.
//
#define Error(msg) do { ErrorMsgs.push_back(msg); Bad = true; } while (0)

#define t(x) (1 << (unsigned)Type::x)

#define SignedIntegralTypes (t(SByteTyID) | t(ShortTyID) |  \
                             t(IntTyID)   | t(LongTyID))
static long UnsignedIntegralTypes = t(UByteTyID) | t(UShortTyID) | 
                                          t(UIntTyID)  | t(ULongTyID);
static const long FloatingPointTypes    = t(FloatTyID) | t(DoubleTyID);

static const long IntegralTypes = SignedIntegralTypes | UnsignedIntegralTypes;

#if 0
static long ValidTypes[Type::FirstDerivedTyID] = {
  [(unsigned)Instruction::UnaryOps::Not] t(BoolTyID),
  //[Instruction::UnaryOps::Add] = IntegralTypes,
  //  [Instruction::Sub] = IntegralTypes,
};
#endif

#undef t

static bool verify(const BasicBlock *BB, vector<string> &ErrorMsgs) {
  bool Bad = false;
  if (BB->getTerminator() == 0) Error("Basic Block does not have terminator!");

  
  return Bad;
}


bool verify(const Method *M, vector<string> &ErrorMsgs) {
  bool Bad = false;
  
  for (Method::BasicBlocksType::const_iterator BBIt = M->getBasicBlocks().begin();
       BBIt != M->getBasicBlocks().end(); BBIt++) {
    Bad |= verify(*BBIt, ErrorMsgs);
  }

  return Bad;
}

bool verify(const Module *C, vector<string> &ErrorMsgs) {
  bool Bad = false;
  assert(Type::FirstDerivedTyID-1 < sizeof(long)*8 && 
	 "Resize ValidTypes table to handle more than 32 primitive types!");

  for (Module::MethodListType::const_iterator MI = C->getMethodList().begin();
       MI != C->getMethodList().end(); MI++) {
    const Method *M = *MI;
    Bad |= verify(M, ErrorMsgs);
  }
  
  return Bad;
}
