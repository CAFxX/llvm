//===- CloneFunction.cpp - Clone a function into another function ---------===//
//
// This file implements the CloneFunctionInto interface, which is used as the
// low-level function cloner.  This is used by the CloneFunction and function
// inliner to do the dirty work of copying the body of a function around.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/iTerminators.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"

// RemapInstruction - Convert the instruction operands from referencing the 
// current values into those specified by ValueMap.
//
static inline void RemapInstruction(Instruction *I, 
                                    std::map<const Value *, Value*> &ValueMap) {
  for (unsigned op = 0, E = I->getNumOperands(); op != E; ++op) {
    const Value *Op = I->getOperand(op);
    Value *V = ValueMap[Op];
    if (!V && (isa<GlobalValue>(Op) || isa<Constant>(Op)))
      continue;  // Globals and constants don't get relocated

#ifndef NDEBUG
    if (!V) {
      std::cerr << "Val = \n" << Op << "Addr = " << (void*)Op;
      std::cerr << "\nInst = " << I;
    }
#endif
    assert(V && "Referenced value not in value map!");
    I->setOperand(op, V);
  }
}

// Clone OldFunc into NewFunc, transforming the old arguments into references to
// ArgMap values.
//
void CloneFunctionInto(Function *NewFunc, const Function *OldFunc,
                       std::map<const Value*, Value*> &ValueMap,
                       std::vector<ReturnInst*> &Returns,
                       const char *NameSuffix) {
  assert(NameSuffix && "NameSuffix cannot be null!");
  
#ifndef NDEBUG
  for (Function::const_aiterator I = OldFunc->abegin(), E = OldFunc->aend();
       I != E; ++I)
    assert(ValueMap.count(I) && "No mapping from source argument specified!");
#endif

  // Loop over all of the basic blocks in the function, cloning them as
  // appropriate.  Note that we save BE this way in order to handle cloning of
  // recursive functions into themselves.
  //
  for (Function::const_iterator BI = OldFunc->begin(), BE = OldFunc->end();
       BI != BE; ++BI) {
    const BasicBlock &BB = *BI;
    
    // Create a new basic block to copy instructions into!
    BasicBlock *CBB = new BasicBlock("", NewFunc);
    if (BB.hasName()) CBB->setName(BB.getName()+NameSuffix);
    ValueMap[&BB] = CBB;                       // Add basic block mapping.

    // Loop over all instructions copying them over...
    for (BasicBlock::const_iterator II = BB.begin(), IE = BB.end();
         II != IE; ++II) {
      Instruction *NewInst = II->clone();
      if (II->hasName())
        NewInst->setName(II->getName()+NameSuffix);     // Name is not cloned...
      CBB->getInstList().push_back(NewInst);
      ValueMap[II] = NewInst;                // Add instruction map to value.
    }

    if (ReturnInst *RI = dyn_cast<ReturnInst>(CBB->getTerminator()))
      Returns.push_back(RI);
  }

  // Loop over all of the instructions in the function, fixing up operand 
  // references as we go.  This uses ValueMap to do all the hard work.
  //
  for (Function::const_iterator BB = OldFunc->begin(), BE = OldFunc->end();
       BB != BE; ++BB) {
    BasicBlock *NBB = cast<BasicBlock>(ValueMap[BB]);
    
    // Loop over all instructions, fixing each one as we find it...
    for (BasicBlock::iterator II = NBB->begin(); II != NBB->end(); ++II)
      RemapInstruction(II, ValueMap);
  }
}

/// CloneFunction - Return a copy of the specified function, but without
/// embedding the function into another module.  Also, any references specified
/// in the ValueMap are changed to refer to their mapped value instead of the
/// original one.  If any of the arguments to the function are in the ValueMap,
/// the arguments are deleted from the resultant function.  The ValueMap is
/// updated to include mappings from all of the instructions and basicblocks in
/// the function from their old to new values.
///
Function *CloneFunction(const Function *F,
                        std::map<const Value*, Value*> &ValueMap) {
  std::vector<const Type*> ArgTypes;

  // The user might be deleting arguments to the function by specifying them in
  // the ValueMap.  If so, we need to not add the arguments to the arg ty vector
  //
  for (Function::const_aiterator I = F->abegin(), E = F->aend(); I != E; ++I)
    if (ValueMap.count(I) == 0)  // Haven't mapped the argument to anything yet?
      ArgTypes.push_back(I->getType());

  // Create a new function type...
  FunctionType *FTy = FunctionType::get(F->getFunctionType()->getReturnType(),
                                    ArgTypes, F->getFunctionType()->isVarArg());

  // Create the new function...
  Function *NewF = new Function(FTy, F->hasInternalLinkage(), F->getName());
  
  // Loop over the arguments, copying the names of the mapped arguments over...
  Function::aiterator DestI = NewF->abegin();
  for (Function::const_aiterator I = F->abegin(), E = F->aend(); I != E; ++I)
    if (ValueMap.count(I)) {        // Is this argument preserved?
      DestI->setName(I->getName()); // Copy the name over...
      ValueMap[I] = DestI;          // Add mapping to ValueMap
    }

  std::vector<ReturnInst*> Returns;  // Ignore returns cloned...
  CloneFunctionInto(NewF, F, ValueMap, Returns);
  return NewF;                    
}
