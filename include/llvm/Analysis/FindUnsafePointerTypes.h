//===- llvm/Analysis/FindUnsafePointerTypes.h - Unsafe pointers ---*- C++ -*-=//
//
// This file defines a pass that can be used to determine, interprocedurally, 
// which pointer types are accessed unsafely in a program.  If there is an
// "unsafe" access to a specific pointer type, transformations that depend on
// type safety cannot be permitted.
//
// The result of running this analysis over a program is a set of unsafe pointer
// types that cannot be transformed.  Safe pointer types are not tracked.
//
// Additionally, this analysis exports a hidden command line argument that (when
// enabled) prints out the reasons a type was determined to be unsafe.  Just add
// -printunsafeptrinst to the command line of the tool you want to get it.
// 
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_UNSAFEPOINTERTYPES_H
#define LLVM_ANALYSIS_UNSAFEPOINTERTYPES_H

#include "llvm/Pass.h"
#include <set>

class PointerType;

struct FindUnsafePointerTypes : public Pass {
  // UnsafeTypes - Set of types that are not safe to transform.
  std::set<PointerType*> UnsafeTypes;
public:
  static AnalysisID ID;    // We are an analysis, we must have an ID

  FindUnsafePointerTypes(AnalysisID id) { assert(ID == id); }

  virtual const char *getPassName() const { return "Find Unsafe Pointer Types";}

  // Accessor for underlying type set...
  inline const std::set<PointerType*> &getUnsafeTypes() const {
    return UnsafeTypes;
  }

  // run - Inspect the operations that the specified module does on
  // values of various types.  If they are deemed to be 'unsafe' note that the
  // type is not safe to transform.
  //
  virtual bool run(Module &M);

  // printResults - Loop over the results of the analysis, printing out unsafe
  // types.
  //
  void printResults(const Module *Mod, std::ostream &o) const;

  // getAnalysisUsage - Of course, we provide ourself...
  //
  virtual void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.setPreservesAll();
    AU.addProvided(ID);
  }
};

#endif
