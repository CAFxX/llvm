//===-- TransformToCPS.cpp - Transform to Continuation-Passing Style ------===//
//
// This pass transforms all functions and callsites so that 
// continuation-passing style is used instead of the normal stack-based 
// call-ret approach.
//
// The interesting side-effect of this is that all calls become tail-calls,
// therefore allowing tail-call elimination to run and do its thing. This in 
// turn allows avoiding all calling convention overhead (note: this increases 
// the load on the register allocator!)
//
// Since ATM tail-call elimination requires the callee to be fastcc, it is a
// good idea to run PromoteCC before this pass, so we set PromoteCC as 
// required in getAnalysisUsage.
//
// It should be also noted that this is better run at link-time (so that all
// functions are in the same module) on unoptimzed bitcode (so that other 
// optimizations had no chance to mess around).
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "cps"
#include "FlattenCFG.h"

STATISTIC(numFunctions, "Number of functions transformed to CPS");
STATISTIC(numCalls,     "Number of calls transformed to CPS");
STATISTIC(numReturns,   "Number of returns transformed to CPS");
STATISTIC(numInvokes,   "Number of invokes transformed to CPS");
STATISTIC(numUnwinds,   "Number of unwinds transformed to CPS");

namespace PassNS {
  struct CPSFunction {
    Function* Orig; // original function
    std::vector<BasicBlock*> BBs; // cloned basic blocks
    ValueToValueMapTy VMap; // argument mapping
    std::vector<ReturnInst*> Returns; // return
    BasicBlock* PhiBB; // basic block containing the phis
    PhiNode* Continuation; // value containing the continuation
  }
	
  class TransformToCPS : public ModulePass {
    private:
      Module* M; // the module we are working on
      FunctionType* FmTy; // the type of the module pseudo-function
      Function* Fm; // the module pseudo-function
      StructType* ContinuationTy; // the type of the structure used to hold the continuation data
      Type* ContinuationBBTy; // the type of the pointer to a continuation basic block
      PointerType* ContinuationBBArgsTy; // the type of the pointer to the arguments of a continuation
      
    public :
      static char ID; // Pass identification, replacement for typeid
      TransformToCPS() : ModulePass(ID) {
      }
      virtual void getAnalysisUsage(AnalysisUsage &AU) const {
        //AU.addRequired<PromoteCC>();
      }
      bool runOnModule(Module &M);
  };
}

PassBoilerplate(TransformToCPS, "cps", "Transform to Continuation-Passing Style")

void TransformToCPS::Transform(CallInst* ci) {
  // step 0: in the caller, prepare the continuation before the call
  
  // get the parent continuation
  Value* CP = ...

  // alloca the parent continuation so that we can get a pointer to it
  AllocaInst* CPa = AllocaInst::Create(ContinuationTy, "C_alloca_", ci);
  StoreInst::Create(CPa, CP, M->getContext());
  
  // create the new continuation
  Value* C = Value::get(ContinuationTy, "C_", M->getContext());
  
  // store the pointer to the parent continuation in the new one
  // TODO

  // put all variables live after the current callinst in a struct, insert a pointer to
  // the struct in the continuation
  
  // step 1: teach the callee about this callsite
  
  
  // step 2: split the caller at the callsite and create the BB we should return to
  
  // replace the callinst with a unconditional branch to the PhiBB of the callee 
  
  // split the basic block after the branch, creating a new BB

  // store a pointer to the new BB in the next field of the continuation

  // set the return block of the callee as the predecessor of the new BB
  
  // extract the live variables from the continuation
  
}

void TransformToCPS::Transform(InvokeInst* ii) {
  assert(!"Invokes and unwinds are not supported yet");
}

void TransformToCPS::Transform(ReturnInst* ri) {
  // step 0 extract the values from the continuation object
  std::vector<Value*> Idx;
  Idx.push_back(ConstantInt::get(Type::getInt32Ty(M->getContext()),0,true));
  Idx.push_back(ConstantInt::get(Type::getInt32Ty(M->getContext()),0,true));
  GetElementPtrInst* NextBB = GetElementPtrInst::CreateInBounds(
    cps->Continuation,
    Idx.begin(),
    Idx.end(),
    "nextbb_" + cps->Original->getName(),
    BB
  );

  // step 1 create an indirect branch to the BB pointed to by the continuation object
  //        note that this requires knowing all possible BBs we might jump to, so this can be done 
  //        only after all calls and invokes have been transformed
  BranchInst::Create();
}

void TransformToCPS::Transform(UnwindInst* ui) {
  assert(!"Invokes and unwinds are not supported yet");
}
  
void TransformToCPS::CreateModuleFunction() {
  // create the function type
  std::vector<const Type*> Args;
  Args.push_back(Type::getInt32Ty(M->getContext())); // function index
  Args.push_back(Type::getInt8PtrTy(M->getContext())); // arguments
  FmTy = FunctionType::get(
    Type::getInt8PtrTy(M->getContext()),
    Args,
    false
  );
  // create the function itself
  Fm = Function::Create(
    FmTy,
    GlobalValue::InternalLinkage,
    "CPS_ModuleFunction"
  );
  Function::arg_iterator arg = Fm->arg_begin();
  (arg++)->setName("func"); // function index
  (arg++)->setName("args"); // arguments
  Fm->setCallingConv(CallingConv::Fast);
  // create the continuation type, a struct with 3 members: a pointer to the next continuation,
  // the address of the basic block we should jump to when we're done in the current function
  // and a pointer to the values we have to pass to the next basic block
  PATypeHolder OpaqueContinuationTy = OpaqueType::get(M->getContext());
  ContinuationBBTy = Type::get(Value::BasicBlockVal, M->getContext());
  ContinuationBBArgsTy = Type::getInt8PtrTy(M->getContext());
  std::vector<const Type*> Elts;
  Elts.push_back(PointerType::getUnqual(OpaqueContinuationTy)); // next continuation
  Elts.push_back(ContinuationBBTy); // continuation function
  Elts.push_back(ContinuationBBArgsTy); // continuation function args
  ContinuationTy = StructType::get(M->getContext(), Elts, false);
  cast<OpaqueType>(OpaqueContinuationTy.get())->refineAbstractTypeTo(ContinuationTy);
  ContinuationTy = cast<StructType>(OpaqueContinuationTy.get());
}

void TransformToCPS::FoldFunctions() {
  // iterate over all functions in the module
  for (Module::iterator f=M->begin(), fe=M->end(); f!=fe; f++) {
    CPSFunction cps;
    cps.Orig = f;
    // create a pre-entry BB containing all the phis
    cps.PhiBB = BasicBlock::Create(M->getContext(), "phibb_"+f->getName(), Fm, NULL);
    for (Function::arg_iterator a=f->arg_begin(), ae=f->arg_end(); a!=ae; a++) {
      cps.VMap[a] = PhiNode::Create(a->getType(), a->getName()+"_"+f->getName(), cps.PhiBB);
    }
    cps.Continuation = PhiNode::Create(ContinuationTy, "C_"+f->getName(), cps.PhiBB);
    // clone the function body
    CloneFunctionInto(Fm, f, cps.VMap, true, cps.Returns, "_"+f->getName());
    // terminate PhiBB with an unconditional branch to the cloned entry BB
    BranchInst::Create(VMap[f->getEntryBlock()], cps.PhiBB);
    // at this point the cloned code still contains CIRUs: they will be taken care of later
  }
}

void TransformToCPS::TransformCode() {
  for (Function::iterator b = Fm->begin(), be = Fm->end(); b != be; b++) {
    for (BasicBlock::iterator i = b->begin(), ie = b->end(); i != ie; i++) {
      if (CallInst* ci = dyn_cast<CallInst>(i)) {
        Transform(ci);
        numCalls++;
      } else if (InvokeInst* ii = dyn_cast<InvokeInst>(i)) {
        Transform(ii);
        numInvokes++;
      }
    }
  }
  for (Function::iterator b = Fm->begin(), be = Fm->end(); b != be; b++) {
    for (BasicBlock::iterator i = b->begin(), ie = b->end(); i != ie; i++) {
      if (ReturnInst* ri = dyn_cast<ReturnInst>(i)) {
        Transform(ri);
        numReturns++;
      } else if (UnwindInst* ui = dyn_cast<UnwindInst>(i)) {
        Transform(ui);
        numUnwinds++;
      }
    }
  }
}

bool TransformToCPS::runOnModule(Module &m) {
  M = &m;
  // step 0	create the pseudo-function Fm that will hold all the code of the module
  CreateModuleFunction();
  // step 1	clone the bodies of all functions in Fm, keeping track of which BBs 
  //        belong to which function
  FoldFunctions();
  // step 2	iterate on all the code in Fm, fixing CIRUs
  TransformCode();
  // step 3	transform all functions so that they forward to Fm
  ForwardFunctions();
  // step 4 we are done: insert Fm in the module
  M->getFunctionList().push_back(Fm);
  return true;
}


