//===- llvm/Transforms/Pass.h - Base class for XForm Passes ------*- C++ -*--=//
//
// This file defines a marker class that indicates that a specified class is a
// transformation pass implementation.
//
// Pass's are designed this way so that it is possible to apply N passes to a
// module, by first doing N Pass specific initializations for the module, then
// looping over all of the methods in the module, doing method specific work
// N times for each method.  Like this:
//
// for_each(Passes.begin(), Passes.end(), doPassInitialization(Module));
// for_each(Method *M <- Module->begin(), Module->end())
//   for_each(Passes.begin(), Passes.end(), doPerMethodWork(M));
//
// The other way to do things is like this:
// for_each(Pass *P <- Passes.begin(), Passes.end()) {
//   Passes->doPassInitialization(Module)
//   for_each(Module->begin(), Module->end(), P->doPerMethodWork);
// }
//
// But this can cause thrashing and poor cache performance, so we don't do it
// that way.
//
// Because a transformation does not see all methods consecutively, it should
// be careful about the state that it maintains... another pass may modify a
// method between two invokacations of doPerMethodWork.
//
// Also, implementations of doMethodWork should not remove any methods from the
// module.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_PASS_H
#define LLVM_TRANSFORMS_PASS_H

#include "llvm/Module.h"
#include "llvm/Method.h"

//===----------------------------------------------------------------------===//
// Pass interface - Implemented by all 'passes'.
//
struct Pass {
  //===--------------------------------------------------------------------===//
  // The externally useful entry points
  //

  // runAllPasses - Run a bunch of passes on the specified module, efficiently.
  static bool runAllPasses(Module *M, vector<Pass*> &Passes) {
    for (unsigned i = 0; i < Passes.size(); ++i)
      if (Passes[i]->doPassInitializationVirt(M)) return true;
    
    // Loop over all of the methods, applying all of the passes to them
    for (Module::iterator I = M->begin(); I != M->end(); ++I)
      for (unsigned i = 0; i < Passes.size(); ++i)
        if (Passes[i]->doPerMethodWorkVirt(*I)) return true;
    return false;
  }

  // runAllPassesAndFree - Run a bunch of passes on the specified module,
  // efficiently.  When done, delete all of the passes.
  //
  static bool runAllPassesAndFree(Module *M, vector<Pass*> &Passes) {
    // First run all of the passes
    bool Result = runAllPasses(M, Passes);

    // Free all of the passes.
    for (unsigned i = 0; i < Passes.size(); ++i)
      delete Passes[i];
    return Result;
  }


  // run(Module*) - Run this pass on a module and all of the methods contained
  // within it.  Returns false on success.
  //
  bool run(Module *M) {
    if (doPassInitializationVirt(M)) return true;

    // Loop over methods in the module.  doPerMethodWork could add a method to
    // the Module, so we have to keep checking for end of method list condition.
    //
    for (Module::iterator I = M->begin(); I != M->end(); ++I)
      if (doPerMethodWorkVirt(*I)) return true;
    return false;
  }

  // run(Method*) - Run this pass on a module and one specific method.  Returns
  // false on success.
  //
  bool run(Method *M) {
    if (doPassInitializationVirt(M->getParent())) return true;
    return doPerMethodWorkVirt(M);
  }


  //===--------------------------------------------------------------------===//
  // Functions to be implemented by subclasses
  //

  // Destructor - Virtual so we can be subclassed
  inline virtual ~Pass() {}

  // doPassInitializationVirt - Virtual method overridden by subclasses to do
  // any neccesary per-module initialization.  Returns false on success.
  //
  virtual bool doPassInitializationVirt(Module *M) = 0;

  // doPerMethodWorkVirt - Virtual method overriden by subclasses to do the
  // per-method processing of the pass.  Returns false on success.
  //
  virtual bool doPerMethodWorkVirt(Method *M) = 0;
};


//===----------------------------------------------------------------------===//
// ConcretePass class - This is used by implementations of passes to fill in
// boiler plate code.
//
// Deriving from this class is good because if new methods are added in the 
// future, code for your pass won't have to change to stub out the unused
// functionality.
//
struct ConcretePass : public Pass {

  // doPassInitializationVirt - Default to success.
  virtual bool doPassInitializationVirt(Module *M) { return false; }

  // doPerMethodWorkVirt - Default to success.
  virtual bool doPerMethodWorkVirt(Method *M) { return false; }
};



//===----------------------------------------------------------------------===//
// StatelessPass<t> class - This is used by implementations of passes to fill in
// boiler plate code.  Subclassing this class indicates that a class has no
// state to keep around, so it's safe to invoke static versions of functions.
// This can be more efficient that using virtual function dispatch all of the
// time.
//
// SubClass should be a concrete class that is derived from StatelessPass.
//
template<class SubClass>
struct StatelessPass : public ConcretePass {

  //===--------------------------------------------------------------------===//
  // The externally useful entry points - These are specialized to avoid the
  // overhead of virtual method invokations if 
  //
  // run(Module*) - Run this pass on a module and all of the methods contained
  // within it.  Returns false on success.
  //
  static bool run(Module *M) {
    if (doPassInitialization(M->getParent())) return true;

    // Loop over methods in the module.  doPerMethodWork could add a method to
    // the Module, so we have to keep checking for end of method list condition.
    //
    for (Module::iterator I = M->begin(); I != M->end(); ++I)
      if (doPerMethodWork(*I)) return true;
    return false;
  }

  // run(Method*) - Run this pass on a module and one specific method.  Returns
  // false on success.
  //
  static bool run(Method *M) {
    if (doPassInitialization(M->getParent())) return true;
    return doPerMethodWork(M);
  }

  //===--------------------------------------------------------------------===//
  // Default static method implementations, these should be defined in SubClass

  static bool doPassInitialization(Module *M) { return false; }
  static bool doPerMethodWork(Method *M) { return false; }


  //===--------------------------------------------------------------------===//
  // Virtual method forwarders...

  // doPassInitializationVirt - For a StatelessPass, default to implementing in
  // terms of the static method.
  //
  virtual bool doPassInitializationVirt(Module *M) {
    return SubClass::doPassInitialization(M);
  }

  // doPerMethodWorkVirt - For a StatelessPass, default to implementing in
  // terms of the static method.
  //
  virtual bool doPerMethodWorkVirt(Method *M) {
    return SubClass::doPerMethodWork(M);
  }
};

#endif

