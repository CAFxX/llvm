//===-- Verifier.cpp - Implement the Module Verifier -------------*- C++ -*-==//
//
// This file defines the function verifier interface, that can be used for some
// sanity checking of input to the system.
//
// Note that this does not provide full 'java style' security and verifications,
// instead it just tries to ensure that code is well formed.
//
//  . There are no duplicated names in a symbol table... ie there !exist a val
//    with the same name as something in the symbol table, but with a different
//    address as what is in the symbol table...
//  * Both of a binary operator's parameters are the same type
//  . Verify that arithmetic and other things are only performed on first class
//    types.  No adding structures or arrays.
//  . All of the constants in a switch statement are of the correct type
//  . The code is in valid SSA form
//  . It should be illegal to put a label into any other type (like a structure)
//    or to return one. [except constant arrays!]
//  * Only phi nodes can be self referential: 'add int %0, %0 ; <int>:0' is bad
//  * PHI nodes must have an entry for each predecessor, with no extras.
//  . All basic blocks should only end with terminator insts, not contain them
//  * The entry node to a function must not have predecessors
//  * All Instructions must be embeded into a basic block
//  . Verify that none of the Value getType()'s are null.
//  . Function's cannot take a void typed parameter
//  * Verify that a function's argument list agrees with it's declared type.
//  . Verify that arrays and structures have fixed elements: No unsized arrays.
//  * It is illegal to specify a name for a void value.
//  * It is illegal to have a internal function that is just a declaration
//  * It is illegal to have a ret instruction that returns a value that does not
//    agree with the function return value type.
//  . All other things that are tested by asserts spread about the code...
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/Verifier.h"
#include "llvm/Pass.h"
#include "llvm/Function.h"
#include "llvm/Module.h"
#include "llvm/BasicBlock.h"
#include "llvm/DerivedTypes.h"
#include "llvm/iPHINode.h"
#include "llvm/iTerminators.h"
#include "llvm/iOther.h"
#include "llvm/Argument.h"
#include "llvm/SymbolTable.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/InstVisitor.h"
#include "Support/STLExtras.h"
#include <algorithm>

namespace {  // Anonymous namespace for class

  struct Verifier : public MethodPass, InstVisitor<Verifier> {
    bool Broken;

    Verifier() : Broken(false) {}

    bool doInitialization(Module *M) {
      verifySymbolTable(M->getSymbolTable());
      return false;
    }

    bool runOnMethod(Function *F) {
      visit(F);
      return false;
    }

    // Verification methods...
    void verifySymbolTable(SymbolTable *ST);
    void visitFunction(Function *F);
    void visitBasicBlock(BasicBlock *BB);
    void visitPHINode(PHINode *PN);
    void visitBinaryOperator(BinaryOperator *B);
    void visitCallInst(CallInst *CI);
    void visitInstruction(Instruction *I);

    // CheckFailed - A check failed, so print out the condition and the message
    // that failed.  This provides a nice place to put a breakpoint if you want
    // to see why something is not correct.
    //
    inline void CheckFailed(const char *Cond, const std::string &Message,
                            const Value *V1 = 0, const Value *V2 = 0) {
      std::cerr << Message << "\n";
      if (V1) { std::cerr << V1 << "\n"; }
      if (V2) { std::cerr << V2 << "\n"; }
      Broken = true;
    }
  };
}

// Assert - We know that cond should be true, if not print an error message.
#define Assert(C, M) \
  do { if (!(C)) { CheckFailed(#C, M); return; } } while (0)
#define Assert1(C, M, V1) \
  do { if (!(C)) { CheckFailed(#C, M, V1); return; } } while (0)
#define Assert2(C, M, V1, V2) \
  do { if (!(C)) { CheckFailed(#C, M, V1, V2); return; } } while (0)


// verifySymbolTable - Verify that a function or module symbol table is ok
//
void Verifier::verifySymbolTable(SymbolTable *ST) {
  if (ST == 0) return;   // No symbol table to process

  // Loop over all of the types in the symbol table...
  for (SymbolTable::iterator TI = ST->begin(), TE = ST->end(); TI != TE; ++TI)
    for (SymbolTable::type_iterator I = TI->second.begin(),
           E = TI->second.end(); I != E; ++I) {
      Value *V = I->second;

      // Check that there are no void typed values in the symbol table.  Values
      // with a void type cannot be put into symbol tables because they cannot
      // have names!
      Assert1(V->getType() != Type::VoidTy,
              "Values with void type are not allowed to have names!\n", V);
    }
}


// visitFunction - Verify that a function is ok.
//
void Verifier::visitFunction(Function *F) {
  if (F->isExternal()) return;
  verifySymbolTable(F->getSymbolTable());

  // Check linkage of function...
  Assert1(!F->isExternal() || F->hasExternalLinkage(),
          "Function cannot be an 'internal' 'declare'ation!", F);

  // Check function arguments...
  const FunctionType *FT = F->getFunctionType();
  const Function::ArgumentListType &ArgList = F->getArgumentList();

  Assert2(!FT->isVarArg(), "Cannot define varargs functions in LLVM!", F, FT);
  Assert2(FT->getParamTypes().size() == ArgList.size(),
          "# formal arguments must match # of arguments for function type!",
          F, FT);

  // Check that the argument values match the function type for this function...
  if (FT->getParamTypes().size() == ArgList.size()) {
    for (unsigned i = 0, e = ArgList.size(); i != e; ++i)
      Assert2(ArgList[i]->getType() == FT->getParamType(i),
              "Argument value does not match function argument type!",
              ArgList[i], FT->getParamType(i));
  }

  // Check the entry node
  BasicBlock *Entry = F->getEntryNode();
  Assert1(pred_begin(Entry) == pred_end(Entry),
          "Entry block to function must not have predecessors!", Entry);
}


// verifyBasicBlock - Verify that a basic block is well formed...
//
void Verifier::visitBasicBlock(BasicBlock *BB) {
  Assert1(BB->getTerminator(), "Basic Block does not have terminator!\n", BB);

  // Check that the terminator is ok as well...
  if (isa<ReturnInst>(BB->getTerminator())) {
    Instruction *I = BB->getTerminator();
    Function *F = I->getParent()->getParent();
    if (I->getNumOperands() == 0)
      Assert1(F->getReturnType() == Type::VoidTy,
              "Function returns no value, but ret instruction found that does!",
              I);
    else
      Assert2(F->getReturnType() == I->getOperand(0)->getType(),
              "Function return type does not match operand "
              "type of return inst!", I, F->getReturnType());
  }
}


// visitPHINode - Ensure that a PHI node is well formed.
void Verifier::visitPHINode(PHINode *PN) {
  std::vector<BasicBlock*> Preds(pred_begin(PN->getParent()),
                                 pred_end(PN->getParent()));
  // Loop over all of the incoming values, make sure that there are
  // predecessors for each one...
  //
  for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i) {
    // Make sure all of the incoming values are the right types...
    Assert2(PN->getType() == PN->getIncomingValue(i)->getType(),
            "PHI node argument type does not agree with PHI node type!",
            PN, PN->getIncomingValue(i));

    BasicBlock *BB = PN->getIncomingBlock(i);
    std::vector<BasicBlock*>::iterator PI =
      find(Preds.begin(), Preds.end(), BB);
    Assert2(PI != Preds.end(), "PHI node has entry for basic block that"
            " is not a predecessor!", PN, BB);
    Preds.erase(PI);
  }
  
  // There should be no entries left in the predecessor list...
  for (std::vector<BasicBlock*>::iterator I = Preds.begin(),
         E = Preds.end(); I != E; ++I)
    Assert2(0, "PHI node does not have entry for a predecessor basic block!",
            PN, *I);

  visitInstruction(PN);
}

void Verifier::visitCallInst(CallInst *CI) {
  Assert1(isa<PointerType>(CI->getOperand(0)->getType()),
          "Called function must be a pointer!", CI);
  PointerType *FPTy = cast<PointerType>(CI->getOperand(0)->getType());
  Assert1(isa<FunctionType>(FPTy->getElementType()),
          "Called function is not pointer to function type!", CI);
}

// visitBinaryOperator - Check that both arguments to the binary operator are
// of the same type!
//
void Verifier::visitBinaryOperator(BinaryOperator *B) {
  Assert2(B->getOperand(0)->getType() == B->getOperand(1)->getType(),
          "Both operands to a binary operator are not of the same type!",
          B->getOperand(0), B->getOperand(1));

  visitInstruction(B);
}


// verifyInstruction - Verify that a non-terminator instruction is well formed.
//
void Verifier::visitInstruction(Instruction *I) {
  assert(I->getParent() && "Instruction not embedded in basic block!");

  // Check that all uses of the instruction, if they are instructions
  // themselves, actually have parent basic blocks.  If the use is not an
  // instruction, it is an error!
  //
  for (User::use_iterator UI = I->use_begin(), UE = I->use_end();
       UI != UE; ++UI) {
    Assert1(isa<Instruction>(*UI), "Use of instruction is not an instruction!",
            *UI);
    Instruction *Used = cast<Instruction>(*UI);
    Assert2(Used->getParent() != 0, "Instruction referencing instruction not"
            " embeded in a basic block!", I, Used);
  }

  if (!isa<PHINode>(I)) {   // Check that non-phi nodes are not self referential
    for (Value::use_iterator UI = I->use_begin(), UE = I->use_end();
         UI != UE; ++UI)
      Assert1(*UI != (User*)I,
              "Only PHI nodes may reference their own value!", I);
  }

  Assert1(I->getType() != Type::VoidTy || !I->hasName(),
          "Instruction has a name, but provides a void value!", I);
}


//===----------------------------------------------------------------------===//
//  Implement the public interfaces to this file...
//===----------------------------------------------------------------------===//

Pass *createVerifierPass() {
  return new Verifier();
}

bool verifyFunction(const Function *F) {
  Verifier V;
  V.visit((Function*)F);
  return V.Broken;
}

// verifyModule - Check a module for errors, printing messages on stderr.
// Return true if the module is corrupt.
//
bool verifyModule(const Module *M) {
  Verifier V;
  V.run((Module*)M);
  return V.Broken;
}
