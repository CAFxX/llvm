//===- LowerAllocations.cpp - Reduce malloc & free insts to calls ---------===//
//
// The LowerAllocations transformation is a target dependant tranformation
// because it depends on the size of data types and alignment constraints.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/ChangeAllocations.h"
#include "llvm/Module.h"
#include "llvm/Function.h"
#include "llvm/BasicBlock.h"
#include "llvm/DerivedTypes.h"
#include "llvm/iMemory.h"
#include "llvm/iOther.h"
#include "llvm/Constants.h"
#include "llvm/Pass.h"
#include "llvm/Target/TargetData.h"
#include "Support/StatisticReporter.h"

static Statistic<> NumLowered("lowerallocs\t- Number of allocations lowered");
using std::vector;

namespace {

// LowerAllocations - Turn malloc and free instructions into %malloc and %free
// calls.
//
class LowerAllocations : public BasicBlockPass {
  Function *MallocFunc;   // Functions in the module we are processing
  Function *FreeFunc;     // Initialized by doInitialization

  const TargetData &DataLayout;
public:
  inline LowerAllocations(const TargetData &TD) : DataLayout(TD) {
    MallocFunc = FreeFunc = 0;
  }

  const char *getPassName() const { return "Lower Allocations"; }

  // doPassInitialization - For the lower allocations pass, this ensures that a
  // module contains a declaration for a malloc and a free function.
  //
  bool doInitialization(Module *M);

  // runOnBasicBlock - This method does the actual work of converting
  // instructions over, assuming that the pass has already been initialized.
  //
  bool runOnBasicBlock(BasicBlock *BB);
};

}

// createLowerAllocationsPass - Interface to this file...
Pass *createLowerAllocationsPass(const TargetData &TD) {
  return new LowerAllocations(TD);
}


// doInitialization - For the lower allocations pass, this ensures that a
// module contains a declaration for a malloc and a free function.
//
// This function is always successful.
//
bool LowerAllocations::doInitialization(Module *M) {
  const FunctionType *MallocType = 
    FunctionType::get(PointerType::get(Type::SByteTy),
                      vector<const Type*>(1, Type::UIntTy), false);
  const FunctionType *FreeType = 
    FunctionType::get(Type::VoidTy,
                      vector<const Type*>(1, PointerType::get(Type::SByteTy)),
                      false);

  MallocFunc = M->getOrInsertFunction("malloc", MallocType);
  FreeFunc   = M->getOrInsertFunction("free"  , FreeType);

  return true;
}

// runOnBasicBlock - This method does the actual work of converting
// instructions over, assuming that the pass has already been initialized.
//
bool LowerAllocations::runOnBasicBlock(BasicBlock *BB) {
  bool Changed = false;
  assert(MallocFunc && FreeFunc && BB && "Pass not initialized!");

  // Loop over all of the instructions, looking for malloc or free instructions
  for (unsigned i = 0; i != BB->size(); ++i) {
    BasicBlock::InstListType &BBIL = BB->getInstList();
    if (MallocInst *MI = dyn_cast<MallocInst>(*(BBIL.begin()+i))) {
      BBIL.remove(BBIL.begin()+i);   // remove the malloc instr...
        
      const Type *AllocTy = cast<PointerType>(MI->getType())->getElementType();
      
      // Get the number of bytes to be allocated for one element of the
      // requested type...
      unsigned Size = DataLayout.getTypeSize(AllocTy);
      
      // malloc(type) becomes sbyte *malloc(constint)
      Value *MallocArg = ConstantUInt::get(Type::UIntTy, Size);
      if (MI->getNumOperands() && Size == 1) {
        MallocArg = MI->getOperand(0);         // Operand * 1 = Operand
      } else if (MI->getNumOperands()) {
        // Multiply it by the array size if neccesary...
        MallocArg = BinaryOperator::create(Instruction::Mul,MI->getOperand(0),
                                           MallocArg);
        BBIL.insert(BBIL.begin()+i++, cast<Instruction>(MallocArg));
      }
      
      // Create the call to Malloc...
      CallInst *MCall = new CallInst(MallocFunc,
                                     vector<Value*>(1, MallocArg));
      BBIL.insert(BBIL.begin()+i, MCall);
      
      // Create a cast instruction to convert to the right type...
      CastInst *MCast = new CastInst(MCall, MI->getType());
      BBIL.insert(BBIL.begin()+i+1, MCast);
      
      // Replace all uses of the old malloc inst with the cast inst
      MI->replaceAllUsesWith(MCast);
      delete MI;                          // Delete the malloc inst
      Changed = true;
      ++NumLowered;
    } else if (FreeInst *FI = dyn_cast<FreeInst>(*(BBIL.begin()+i))) {
      BBIL.remove(BB->getInstList().begin()+i);
      
      // Cast the argument to free into a ubyte*...
      CastInst *MCast = new CastInst(FI->getOperand(0), 
                                     PointerType::get(Type::UByteTy));
      BBIL.insert(BBIL.begin()+i, MCast);
      
      // Insert a call to the free function...
      CallInst *FCall = new CallInst(FreeFunc,
                                     vector<Value*>(1, MCast));
      BBIL.insert(BBIL.begin()+i+1, FCall);
      
      // Delete the old free instruction
      delete FI;
      Changed = true;
      ++NumLowered;
    }
  }

  return Changed;
}
