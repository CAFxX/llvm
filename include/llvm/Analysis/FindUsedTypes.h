//===- llvm/Analysis/FindUsedTypes.h - Find all Types in use -----*- C++ -*--=//
//
// This pass is used to seek out all of the types in use by the program.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_FINDUSEDTYPES_H
#define LLVM_ANALYSIS_FINDUSEDTYPES_H

#include "llvm/Pass.h"
#include <set>
class SymbolTable;
class Type;

class FindUsedTypes : public Pass {
  std::set<const Type *> UsedTypes;

  bool IncludeSymbolTables;
public:
  // FindUsedTypes ctor - This pass can optionally include types that are
  // referenced only in symbol tables, but the default is not to.
  //
  static AnalysisID ID;
  static AnalysisID IncludeSymbolTableID;

  FindUsedTypes(AnalysisID id) : IncludeSymbolTables(id != ID) {}
  virtual const char *getPassName() const { return "Find Used Types"; }

  // getTypes - After the pass has been run, return the set containing all of
  // the types used in the module.
  //
  inline const std::set<const Type *> &getTypes() const { return UsedTypes; }

  // Print the types found in the module.  If the optional Module parameter is
  // passed in, then the types are printed symbolically if possible, using the
  // symbol table from the module.
  //
  void printTypes(std::ostream &o, const Module *M = 0) const;

private:
  // IncorporateType - Incorporate one type and all of its subtypes into the
  // collection of used types.
  //
  void IncorporateType(const Type *Ty);

  // IncorporateSymbolTable - Add all types referenced by the specified symtab
  // into the collection of used types.
  //
  void IncorporateSymbolTable(const SymbolTable *ST);

public:
  // run - This incorporates all types used by the specified module
  //
  bool run(Module &M);

  // getAnalysisUsage - Of course, we provide ourself...
  //
  virtual void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.setPreservesAll();
    AU.addProvided(ID);
  }
};

#endif
