//===- ConstantProp.cpp - Code to perform Simple Constant Propogation -----===//
//
// This file implements constant propogation and merging:
//
// Specifically, this:
//   * Converts instructions like "add int 1, 2" into 3
//
// Notice that:
//   * This pass has a habit of making definitions be dead.  It is a good idea
//     to to run a DIE pass sometime after running this pass.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/ConstantHandling.h"
#include "llvm/Instruction.h"
#include "llvm/Pass.h"
#include "llvm/Support/InstIterator.h"
#include <set>

#include "Support/StatisticReporter.h"
static Statistic<> NumInstKilled("constprop - Number of instructions killed");

namespace {
  struct ConstantPropogation : public FunctionPass {
    const char *getPassName() const { return "Simple Constant Propogation"; }

    inline bool runOnFunction(Function *F);

    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.preservesCFG();
    }
  };
}

Pass *createConstantPropogationPass() {
  return new ConstantPropogation();
}


bool ConstantPropogation::runOnFunction(Function *F) {
  // Initialize the worklist to all of the instructions ready to process...
  std::set<Instruction*> WorkList(inst_begin(F), inst_end(F));
  bool Changed = false;

  while (!WorkList.empty()) {
    Instruction *I = *WorkList.begin();
    WorkList.erase(WorkList.begin());    // Get an element from the worklist...

    if (!I->use_empty())                 // Don't muck with dead instructions...
      if (Constant *C = ConstantFoldInstruction(I)) {
        // Add all of the users of this instruction to the worklist, they might
        // be constant propogatable now...
        for (Value::use_iterator UI = I->use_begin(), UE = I->use_end();
             UI != UE; ++UI)
          WorkList.insert(cast<Instruction>(*UI));
        
        // Replace all of the uses of a variable with uses of the constant.
        I->replaceAllUsesWith(C);

        // We made a change to the function...
        Changed = true;
        ++NumInstKilled;
      }
  }
  return Changed;
}
