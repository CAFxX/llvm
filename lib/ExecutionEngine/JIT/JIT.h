//===-- VM.h - Definitions for Virtual Machine ------------------*- C++ -*-===//
//
// This file defines the top-level Virtual Machine data structure.
//
//===----------------------------------------------------------------------===//

#ifndef VM_H
#define VM_H

#include "../ExecutionEngine.h"
#include "llvm/PassManager.h"
#include <map>

class Function;
class GlobalValue;
class Constant;
class TargetMachine;
class MachineCodeEmitter;

class VM : public ExecutionEngine {
  TargetMachine &TM;       // The current target we are compiling to
  PassManager PM;          // Passes to compile a function
  MachineCodeEmitter *MCE; // MCE object

  // FunctionRefs - A mapping between addresses that refer to unresolved
  // functions and the LLVM function object itself.  This is used by the fault
  // handler to lazily patch up references...
  //
  std::map<void*, Function*> FunctionRefs;

public:
  VM(Module *M, TargetMachine *tm);
  ~VM();

  /// run - Start execution with the specified function and arguments.
  ///
  virtual int run(const std::string &FnName,
		  const std::vector<std::string> &Args);

  void addFunctionRef(void *Ref, Function *F) {
    FunctionRefs[Ref] = F;
  }

  const std::string &getFunctionReferencedName(void *RefAddr);

  void *resolveFunctionReference(void *RefAddr);

  /// getPointerToNamedFunction - This method returns the address of the
  /// specified function by using the dlsym function call.  As such it is only
  /// useful for resolving library symbols, not code generated symbols.
  ///
  void *getPointerToNamedFunction(const std::string &Name);

  // CompilationCallback - Invoked the first time that a call site is found,
  // which causes lazy compilation of the target function.
  // 
  static void CompilationCallback();

  /// runAtExitHandlers - Before exiting the program, at_exit functions must be
  /// called.  This method calls them.
  ///
  static void runAtExitHandlers();

private:
  static MachineCodeEmitter *createX86Emitter(VM &V);
  static MachineCodeEmitter *createSparcEmitter(VM &V);
  void setupPassManager();
  void *getPointerToFunction(const Function *F);

  void registerCallback();

  /// emitStubForFunction - This method is used by the JIT when it needs to emit
  /// the address of a function for a function whose code has not yet been
  /// generated.  In order to do this, it generates a stub which jumps to the
  /// lazy function compiler, which will eventually get fixed to call the
  /// function directly.
  ///
  void *emitStubForFunction(const Function &F);
};

#endif
