//===- RaiseAllocations.cpp - Convert %malloc & %free calls to insts ------===//
//
// This file defines the RaiseAllocations pass which convert malloc and free
// calls to malloc and free instructions.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Module.h"
#include "llvm/Function.h"
#include "llvm/DerivedTypes.h"
#include "llvm/iMemory.h"
#include "llvm/iOther.h"
#include "llvm/Pass.h"
#include "Support/StatisticReporter.h"

static Statistic<> NumRaised("raiseallocs\t- Number of allocations raised");

namespace {

// RaiseAllocations - Turn %malloc and %free calls into the appropriate
// instruction.
//
class RaiseAllocations : public BasicBlockPass {
  Function *MallocFunc;   // Functions in the module we are processing
  Function *FreeFunc;     // Initialized by doPassInitializationVirt
public:
  RaiseAllocations() : MallocFunc(0), FreeFunc(0) {}

  // doPassInitialization - For the raise allocations pass, this finds a
  // declaration for malloc and free if they exist.
  //
  bool doInitialization(Module &M);

  // runOnBasicBlock - This method does the actual work of converting
  // instructions over, assuming that the pass has already been initialized.
  //
  bool runOnBasicBlock(BasicBlock &BB);
};

  RegisterOpt<RaiseAllocations>
  X("raiseallocs", "Raise allocations from calls to instructions");
}  // end anonymous namespace


// createRaiseAllocationsPass - The interface to this file...
Pass *createRaiseAllocationsPass() {
  return new RaiseAllocations();
}


bool RaiseAllocations::doInitialization(Module &M) {
  // If the module has a symbol table, they might be referring to the malloc
  // and free functions.  If this is the case, grab the method pointers that 
  // the module is using.
  //
  // Lookup %malloc and %free in the symbol table, for later use.  If they
  // don't exist, or are not external, we do not worry about converting calls
  // to that function into the appropriate instruction.
  //
  const FunctionType *MallocType =   // Get the type for malloc
    FunctionType::get(PointerType::get(Type::SByteTy),
                    std::vector<const Type*>(1, Type::ULongTy), false);

  const FunctionType *FreeType =     // Get the type for free
    FunctionType::get(Type::VoidTy,
                   std::vector<const Type*>(1, PointerType::get(Type::SByteTy)),
                      false);

  // Get Malloc and free prototypes if they exist!
  MallocFunc = M.getFunction("malloc", MallocType);
  FreeFunc   = M.getFunction("free"  , FreeType);

  // Check to see if the prototype is wrong, giving us sbyte*(uint) * malloc
  // This handles the common declaration of: 'void *malloc(unsigned);'
  if (MallocFunc == 0) {
    MallocType = FunctionType::get(PointerType::get(Type::SByteTy),
                            std::vector<const Type*>(1, Type::UIntTy), false);
    MallocFunc = M.getFunction("malloc", MallocType);
  }

  // Check to see if the prototype is missing, giving us sbyte*(...) * malloc
  // This handles the common declaration of: 'void *malloc();'
  if (MallocFunc == 0) {
    MallocType = FunctionType::get(PointerType::get(Type::SByteTy),
                                   std::vector<const Type*>(), true);
    MallocFunc = M.getFunction("malloc", MallocType);
  }

  // Check to see if the prototype was forgotten, giving us void (...) * free
  // This handles the common forward declaration of: 'void free();'
  if (FreeFunc == 0) {
    FreeType = FunctionType::get(Type::VoidTy, std::vector<const Type*>(),true);
    FreeFunc = M.getFunction("free", FreeType);
  }


  // Don't mess with locally defined versions of these functions...
  if (MallocFunc && !MallocFunc->isExternal()) MallocFunc = 0;
  if (FreeFunc && !FreeFunc->isExternal())     FreeFunc = 0;
  return false;
}

// runOnBasicBlock - Process a basic block, fixing it up...
//
bool RaiseAllocations::runOnBasicBlock(BasicBlock &BB) {
  bool Changed = false;
  BasicBlock::InstListType &BIL = BB.getInstList();

  for (BasicBlock::iterator BI = BB.begin(); BI != BB.end(); ++BI) {
    Instruction *I = BI;

    if (CallInst *CI = dyn_cast<CallInst>(I)) {
      if (CI->getCalledValue() == MallocFunc) {      // Replace call to malloc?
        Value *Source = CI->getOperand(1);
        
        // If no prototype was provided for malloc, we may need to cast the
        // source size.
        if (Source->getType() != Type::UIntTy)
          Source = new CastInst(Source, Type::UIntTy, "MallocAmtCast", BI);

        std::string Name(CI->getName()); CI->setName("");
        BI = new MallocInst(Type::SByteTy, Source, Name, BI);
        CI->replaceAllUsesWith(BI);
        BIL.erase(I);
        Changed = true;
        ++NumRaised;
      } else if (CI->getCalledValue() == FreeFunc) { // Replace call to free?
        // If no prototype was provided for free, we may need to cast the
        // source pointer.  This should be really uncommon, but it's neccesary
        // just in case we are dealing with wierd code like this:
        //   free((long)ptr);
        //
        Value *Source = CI->getOperand(1);
        if (!isa<PointerType>(Source->getType()))
          Source = new CastInst(Source, PointerType::get(Type::SByteTy),
                                "FreePtrCast", BI);
        BI = new FreeInst(Source, BI);
        BIL.erase(I);
        Changed = true;
        ++NumRaised;
      }
    }
  }

  return Changed;
}
