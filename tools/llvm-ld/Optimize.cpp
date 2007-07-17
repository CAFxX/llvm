//===- Optimize.cpp - Optimize a complete program -------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by Reid Spencer and is distributed under the
// University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements all optimization of the linked module for llvm-ld.
//
//===----------------------------------------------------------------------===//

#include "llvm/Module.h"
#include "llvm/PassManager.h"
#include "llvm/Analysis/LoadValueNumbering.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/System/DynamicLibrary.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Support/PassNameParser.h"
#include "llvm/Support/PluginLoader.h"
#include <iostream>
using namespace llvm;

// Pass Name Options as generated by the PassNameParser
static cl::list<const PassInfo*, bool, PassNameParser>
  OptimizationList(cl::desc("Optimizations available:"));

// Optimization Enumeration
enum OptimizationLevels {
  OPT_FAST_COMPILE         = 1,
  OPT_SIMPLE               = 2,
  OPT_AGGRESSIVE           = 3,
  OPT_LINK_TIME            = 4,
  OPT_AGGRESSIVE_LINK_TIME = 5
};

// Optimization Options
static cl::opt<OptimizationLevels> OptLevel(
  cl::desc("Choose level of optimization to apply:"),
  cl::init(OPT_FAST_COMPILE), cl::values(
    clEnumValN(OPT_FAST_COMPILE,"O0",
      "An alias for the -O1 option."),
    clEnumValN(OPT_FAST_COMPILE,"O1",
      "Optimize for linking speed, not execution speed."),
    clEnumValN(OPT_SIMPLE,"O2",
      "Perform only required/minimal optimizations"),
    clEnumValN(OPT_AGGRESSIVE,"O3",
      "An alias for the -O2 option."),
    clEnumValN(OPT_LINK_TIME,"O4",
      "Perform standard link time optimizations"),
    clEnumValN(OPT_AGGRESSIVE_LINK_TIME,"O5",
      "Perform aggressive link time optimizations"),
    clEnumValEnd
  )
);

static cl::opt<bool> DisableInline("disable-inlining",
  cl::desc("Do not run the inliner pass"));

static cl::opt<bool>
DisableOptimizations("disable-opt",
  cl::desc("Do not run any optimization passes"));

static cl::opt<bool> DisableInternalize("disable-internalize",
  cl::desc("Do not mark all symbols as internal"));

static cl::opt<bool> VerifyEach("verify-each",
 cl::desc("Verify intermediate results of all passes"));

static cl::alias ExportDynamic("export-dynamic",
  cl::aliasopt(DisableInternalize),
  cl::desc("Alias for -disable-internalize"));

static cl::opt<bool> Strip("strip-all", 
  cl::desc("Strip all symbol info from executable"));

static cl::alias A0("s", cl::desc("Alias for --strip-all"), 
  cl::aliasopt(Strip));

static cl::opt<bool> StripDebug("strip-debug",
  cl::desc("Strip debugger symbol info from executable"));

static cl::alias A1("S", cl::desc("Alias for --strip-debug"),
  cl::aliasopt(StripDebug));

// A utility function that adds a pass to the pass manager but will also add
// a verifier pass after if we're supposed to verify.
static inline void addPass(PassManager &PM, Pass *P) {
  // Add the pass to the pass manager...
  PM.add(P);

  // If we are verifying all of the intermediate steps, add the verifier...
  if (VerifyEach)
    PM.add(createVerifierPass());
}

namespace llvm {

/// Optimize - Perform link time optimizations. This will run the scalar
/// optimizations, any loaded plugin-optimization modules, and then the
/// inter-procedural optimizations if applicable.
void Optimize(Module* M) {

  // Instantiate the pass manager to organize the passes.
  PassManager Passes;

  // If we're verifying, start off with a verification pass.
  if (VerifyEach)
    Passes.add(createVerifierPass());

  // Add an appropriate TargetData instance for this module...
  addPass(Passes, new TargetData(M));

  if (!DisableOptimizations) {
    // Now that composite has been compiled, scan through the module, looking
    // for a main function.  If main is defined, mark all other functions
    // internal.
    if (!DisableInternalize)
      addPass(Passes, createInternalizePass(true));

    // Propagate constants at call sites into the functions they call.  This
    // opens opportunities for globalopt (and inlining) by substituting function
    // pointers passed as arguments to direct uses of functions.  
    addPass(Passes, createIPSCCPPass());

    // Now that we internalized some globals, see if we can hack on them!
    addPass(Passes, createGlobalOptimizerPass());

    // Linking modules together can lead to duplicated global constants, only
    // keep one copy of each constant...
    addPass(Passes, createConstantMergePass());

    // Remove unused arguments from functions...
    addPass(Passes, createDeadArgEliminationPass());

    // Reduce the code after globalopt and ipsccp.  Both can open up significant
    // simplification opportunities, and both can propagate functions through
    // function pointers.  When this happens, we often have to resolve varargs
    // calls, etc, so let instcombine do this.
    addPass(Passes, createInstructionCombiningPass());

    if (!DisableInline)
      addPass(Passes, createFunctionInliningPass()); // Inline small functions

    addPass(Passes, createPruneEHPass());            // Remove dead EH info
    addPass(Passes, createGlobalOptimizerPass());    // Optimize globals again.
    addPass(Passes, createGlobalDCEPass());          // Remove dead functions

    // If we didn't decide to inline a function, check to see if we can
    // transform it to pass arguments by value instead of by reference.
    addPass(Passes, createArgumentPromotionPass());

    // The IPO passes may leave cruft around.  Clean up after them.
    addPass(Passes, createInstructionCombiningPass());

    addPass(Passes, createScalarReplAggregatesPass()); // Break up allocas

    // Run a few AA driven optimizations here and now, to cleanup the code.
    addPass(Passes, createGlobalsModRefPass());      // IP alias analysis

    addPass(Passes, createLICMPass());               // Hoist loop invariants
    addPass(Passes, createLoadValueNumberingPass()); // GVN for load instrs
    addPass(Passes, createGCSEPass());               // Remove common subexprs
    addPass(Passes, createFastDeadStoreEliminationPass()); // Nuke dead stores

    // Cleanup and simplify the code after the scalar optimizations.
    addPass(Passes, createInstructionCombiningPass());

    // Delete basic blocks, which optimization passes may have killed...
    addPass(Passes, createCFGSimplificationPass());

    // Now that we have optimized the program, discard unreachable functions...
    addPass(Passes, createGlobalDCEPass());
  }

  // If the -s or -S command line options were specified, strip the symbols out
  // of the resulting program to make it smaller.  -s and -S are GNU ld options
  // that we are supporting; they alias -strip-all and -strip-debug.
  if (Strip || StripDebug)
    addPass(Passes, createStripSymbolsPass(StripDebug && !Strip));

  // Create a new optimization pass for each one specified on the command line
  std::auto_ptr<TargetMachine> target;
  for (unsigned i = 0; i < OptimizationList.size(); ++i) {
    const PassInfo *Opt = OptimizationList[i];
    if (Opt->getNormalCtor())
      addPass(Passes, Opt->getNormalCtor()());
    else
      std::cerr << "llvm-ld: cannot create pass: " << Opt->getPassName() 
                << "\n";
  }

  // The user's passes may leave cruft around. Clean up after them them but
  // only if we haven't got DisableOptimizations set
  if (!DisableOptimizations) {
    addPass(Passes, createInstructionCombiningPass());
    addPass(Passes, createCFGSimplificationPass());
    addPass(Passes, createDeadCodeEliminationPass());
    addPass(Passes, createGlobalDCEPass());
  }

  // Make sure everything is still good.
  Passes.add(createVerifierPass());

  // Run our queue of passes all at once now, efficiently.
  Passes.run(*M);
}

}
