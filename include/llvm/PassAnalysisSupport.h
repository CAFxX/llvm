//===- llvm/PassAnalysisSupport.h - Analysis Pass Support code ---*- C++ -*-==//
//
// This file defines stuff that is used to define and "use" Analysis Passes.
// This file is automatically #included by Pass.h, so:
//
//           NO .CPP FILES SHOULD INCLUDE THIS FILE DIRECTLY
//
// Instead, #include Pass.h
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_PASS_ANALYSIS_SUPPORT_H
#define LLVM_PASS_ANALYSIS_SUPPORT_H

// No need to include Pass.h, we are being included by it!



//===----------------------------------------------------------------------===//
// AnalysisUsage - Represent the analysis usage information of a pass.  This
// tracks analyses that the pass REQUIRES (must available when the pass runs),
// and analyses that the pass PRESERVES (the pass does not invalidate the
// results of these analyses).  This information is provided by a pass to the
// Pass infrastructure through the getAnalysisUsage virtual function.
//
class AnalysisUsage {
  // Sets of analyses required and preserved by a pass
  std::vector<AnalysisID> Required, Preserved;
  bool PreservesAll;
public:
  AnalysisUsage() : PreservesAll(false) {}
  
  // addRequired - Add the specified ID to the required set of the usage info
  // for a pass.
  //
  AnalysisUsage &addRequiredID(AnalysisID ID) {
    Required.push_back(ID);
    return *this;
  }
  template<class PassClass>
  AnalysisUsage &addRequired() {
    Required.push_back(PassClass::ID);
    return *this;
  }

  // addPreserved - Add the specified ID to the set of analyses preserved by
  // this pass
  //
  AnalysisUsage &addPreservedID(AnalysisID ID) {
    Preserved.push_back(ID);
    return *this;
  }

  template<class PassClass>
  AnalysisUsage &addPreserved() {
    Preserved.push_back(PassClass::ID);
    return *this;
  }

  // setPreservesAll - Set by analyses that do not transform their input at all
  void setPreservesAll() { PreservesAll = true; }
  bool preservesAll() const { return PreservesAll; }

  // preservesCFG - This function should be called by the pass, iff they do not:
  //
  //  1. Add or remove basic blocks from the function
  //  2. Modify terminator instructions in any way.
  //
  // This function annotates the AnalysisUsage info object to say that analyses
  // that only depend on the CFG are preserved by this pass.
  //
  void preservesCFG();

  const std::vector<AnalysisID> &getRequiredSet() const { return Required; }
  const std::vector<AnalysisID> &getPreservedSet() const { return Preserved; }
};



//===----------------------------------------------------------------------===//
// AnalysisResolver - Simple interface implemented by PassManager objects that
// is used to pull analysis information out of them.
//
struct AnalysisResolver {
  virtual Pass *getAnalysisOrNullUp(AnalysisID ID) const = 0;
  virtual Pass *getAnalysisOrNullDown(AnalysisID ID) const = 0;
  Pass *getAnalysis(AnalysisID ID) {
    Pass *Result = getAnalysisOrNullUp(ID);
    assert(Result && "Pass has an incorrect analysis uses set!");
    return Result;
  }

  // getAnalysisToUpdate - Return an analysis result or null if it doesn't exist
  Pass *getAnalysisToUpdate(AnalysisID ID) {
    return getAnalysisOrNullUp(ID);
  }

  // Methods for introspecting into pass manager objects...
  virtual unsigned getDepth() const = 0;
  virtual unsigned getNumContainedPasses() const = 0;
  virtual const Pass *getContainedPass(unsigned N) const = 0;

  virtual void markPassUsed(AnalysisID P, Pass *User) = 0;

  void startPass(Pass *P) {}
  void endPass(Pass *P) {}
protected:
  void setAnalysisResolver(Pass *P, AnalysisResolver *AR);
};

#endif
