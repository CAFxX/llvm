//===- SymbolStripping.cpp - Strip symbols for functions and modules ------===//
//
// This file implements stripping symbols out of symbol tables.
//
// Specifically, this allows you to strip all of the symbols out of:
//   * A function
//   * All functions in a module
//   * All symbols in a module (all function symbols + all module scope symbols)
//
// Notice that:
//   * This pass makes code much less readable, so it should only be used in
//     situations where the 'strip' utility would be used (such as reducing 
//     code size, and making it harder to reverse engineer code).
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar.h"
#include "llvm/Module.h"
#include "llvm/Function.h"
#include "llvm/SymbolTable.h"
#include "llvm/Pass.h"

static bool StripSymbolTable(SymbolTable *SymTab) {
  if (SymTab == 0) return false;    // No symbol table?  No problem.
  bool RemovedSymbol = false;

  for (SymbolTable::iterator I = SymTab->begin(); I != SymTab->end(); ++I) {
    std::map<const std::string, Value *> &Plane = I->second;
    
    SymbolTable::type_iterator B;
    while ((B = Plane.begin()) != Plane.end()) {   // Found nonempty type plane!
      Value *V = B->second;
      if (isa<Constant>(V) || isa<Type>(V))
	SymTab->type_remove(B);
      else 
	V->setName("", SymTab);   // Set name to "", removing from symbol table!
      RemovedSymbol = true;
      assert(Plane.begin() != B && "Symbol not removed from table!");
    }
  }
 
  return RemovedSymbol;
}


// DoSymbolStripping - Remove all symbolic information from a function
//
static bool doSymbolStripping(Function *F) {
  return StripSymbolTable(F->getSymbolTable());
}

// doStripGlobalSymbols - Remove all symbolic information from all functions 
// in a module, and all module level symbols. (function names, etc...)
//
static bool doStripGlobalSymbols(Module *M) {
  // Remove all symbols from functions in this module... and then strip all of
  // the symbols in this module...
  //  
  return StripSymbolTable(M->getSymbolTable());
}

namespace {
  struct SymbolStripping : public FunctionPass {
    const char *getPassName() const { return "Strip Symbols from Functions"; }

    virtual bool runOnFunction(Function *F) {
      return doSymbolStripping(F);
    }
    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.setPreservesAll();
    }
  };

  struct FullSymbolStripping : public SymbolStripping {
    const char *getPassName() const { return "Strip Symbols from Module"; }
    virtual bool doInitialization(Module *M) {
      return doStripGlobalSymbols(M);
    }
  };
}

Pass *createSymbolStrippingPass() {
  return new SymbolStripping();
}

Pass *createFullSymbolStrippingPass() {
  return new FullSymbolStripping();
}
