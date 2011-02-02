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
// TODO: investigate if it is possible to actually fold the whole module in
// a single function, using phis and branches as appropriate, instead of
// relying on tail-call optimizations.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "cps"
#include "FlattenCFG.h"

STATISTIC(numFunctions, "Number of functions transformed to CPS");
STATISTIC(numCallsites, "Number of callsites transformed to CPS");
STATISTIC(numReturns, "Number of callsites transformed to CPS");

namespace PassNS {
  struct CPSFunction {
    Function* Original;
    Function* Outer;
    Function* Inner;
    StructType* ArgsFrame;
  };

  class TransformToCPS : public ModulePass {
    private:
      Module* M;
      FunctionMap FMap;
      FunctionMap FMapInv;
      FunctionSet NewF;

      StructType*   CPSContinuationTy;
      FunctionType* CPSFunctionTy;
      const PointerType*  CPSFunctionArgsTy;
      const PointerType*  CPSFunctionRetTy;

      bool TransformFunctions();
      CPSFunction TransformFunction(Function* F);
      void TransformInstructions();
      bool TransformCallsite(CallInst* ci);
      bool TransformReturn(ReturnInst* ri);
      void CreateTypes();

    public :
      static char ID; // Pass identification, replacement for typeid
      TransformToCPS() : ModulePass(ID), FMap() {
      }
      virtual void getAnalysisUsage(AnalysisUsage &AU) const {
        //AU.addRequired<PromoteCC>();
      }
      bool runOnModule(Module &M);
  };
}

PassBoilerplate(TransformToCPS, "cps", "Transform to Continuation-Passing Style")

CPSFunction TransformToCPS::TransformFunction(Function* F) {
  // TCO requires fastcc non-vararg functions
  assert(F->getCallingConv() == CallingConv::Fast && "Only FastCC functions can be transformed to CPS!");
  assert(F->isVarArg() == false && "Only non-vararg functions can be transformed to CPS!");

  CPSFunction cps;
  cps.Original = F;

  // create the type holding the arguments for the outer function
  std::vector<const Type*> Elms;
  for (Function::arg_iterator a = F->arg_begin(), ae = F->arg_end(); a != ae; ++a)
    Elms.push_back(a->getType());
  StructType *NIFArgTy = StructType::get(M->getContext(), Elms, false);
  M->addTypeName(
    ("cps_function_args_inner_" + F->getName()).str(), 
    NIFArgTy
  );
  cps.ArgsFrame = NIFArgTy;

  // create inner function
  std::vector<const Type*> Args;
  Args.push_back(CPSContinuationTy); // continuation
  for (Function::arg_iterator a = F->arg_begin(), ae = F->arg_end(); a != ae; ++a)
    Args.push_back(a->getType());
  FunctionType *NIFTy = FunctionType::get(CPSFunctionRetTy, Args, false);

  Function *NIF = Function::Create(
    NIFTy, 
    GlobalValue::InternalLinkage, 
    F->getName() + "_" + DEBUG_TYPE + "_inner"
  );
  NIF->arg_begin()->setName("C"); // continuation
  NIF->setCallingConv(CallingConv::Fast);
  NIF->addFnAttr(Attribute::AlwaysInline); // the inner function is used only once: force inlining
  F->getParent()->getFunctionList().push_back(NIF);
  cps.Inner = NIF;

  // clone the body of F in NIF
  ValueToValueMapTy VMap;
  SmallVectorImpl<ReturnInst*> Returns(6);
  for (Function::arg_iterator si = F->arg_begin(), di = ++(NIF->arg_begin()), se = F->arg_end(); si != se; si++, di++) {
    di->setName(si->getName() + "__cps");
    VMap[si] = di;
  }
  CloneFunctionInto(NIF, F, VMap, true, Returns, "__cps");
   
  // create outer, generic function
  Function *NF = Function::Create(
    CPSFunctionTy, 
    GlobalValue::InternalLinkage, 
    F->getName() + "_" + DEBUG_TYPE
  );
  Function::arg_iterator a = NF->arg_begin();
  (a++)->setName("C"); // continuation
  a->setName("args"); // arguments
  NF->setCallingConv(CallingConv::Fast);
  F->getParent()->getFunctionList().push_back(NF);
  cps.Outer = NF;

  // create the entry basic block
  BasicBlock *BB = BasicBlock::Create(M->getContext(), "entry", NF, NULL);
  // type cast the CPSFunctionArgTy to NIFArgTy
  Function::arg_iterator i = NF->arg_begin(); i++; 
  BitCastInst *InnerArgs = new BitCastInst(
    (Value*)&*i,
    PointerType::get(NIFArgTy, 0), 
    "cps_function_inner_args", 
    BB
  );
  // create the forwarding call and insert it into BB
  std::vector<Value*> forwardedArgs;
  forwardedArgs.push_back((Value*)&*NF->arg_begin());
  for (unsigned i=0; i<F->getArgumentList().size(); i++) {
    std::vector<Value*> Idx;
    Idx.push_back(ConstantInt::get(Type::getInt32Ty(M->getContext()),0,true));
    Idx.push_back(ConstantInt::get(Type::getInt32Ty(M->getContext()),i,true));
    GetElementPtrInst* ArgPtr = GetElementPtrInst::CreateInBounds(
      InnerArgs,
      Idx.begin(),
      Idx.end(),
      "argptr_" + to_string(i),
      BB
    );
    LoadInst* Arg = new LoadInst(
      ArgPtr,
      "arg_" + to_string(i),
      false,
      BB
    );
    forwardedArgs.push_back((Value*)Arg);
  }
  CallInst *CI = CallInst::Create(NIF, forwardedArgs.begin(), forwardedArgs.end(), "", BB);
  CI->setCallingConv(NIF->getCallingConv());
  CI->setTailCall(); // this call is the only instruction in the wrapper, so it's always in tail position
  // return the result of forwarding call CI
  ReturnInst::Create(M->getContext(), CI, BB);

  return cps;
}

bool TransformToCPS::TransformCallsite(CallInst* ci) {
  

  return false;
}

bool TransformToCPS::TransformReturn(ReturnInst* ri) {
  

  return false;
}

void TransformToCPS::TransformInstructions() {
  for (Module::iterator f = M->begin(), fe = M->end(); f != fe; ++f) {
    for (Function::iterator b = f->begin(), be = f->end(); b != be; ++b) {
      for (BasicBlock::iterator i = b->begin(), ie = b->end(); i != ie; ++i) {
        if (CallInst* ci = dyn_cast<CallInst>(&*i)) {
          if (TransformCallsite(ci))
            numCallsites++;
        } else if (ReturnInst* ri = dyn_cast<ReturnInst>(&*i)) {
          if (TransformReturn(ri))
            numReturns++;
        }
      }
    }
  }
}

bool TransformToCPS::TransformFunctions() {
  bool Changed = false;
  for (Module::iterator f = M->begin(), fe = M->end(); f != fe; ++f) {
    Function *F = &*f;
    if (F->getCallingConv() != CallingConv::Fast) 
      continue;
    if (F->isVarArg()) 
      continue;
    if (NewF.count(F))
      continue;
    CPSFunction cps = TransformFunction(F);
    Changed = true;
    FMap[cps.Original] = cps.Outer;
    FMapInv[cps.Outer] = cps.Original;
    NewF.insert(cps.Outer);
    NewF.insert(cps.Inner);
    numFunctions++;
  }
  return Changed;
}

// this function creates the types used to implement CPS
void TransformToCPS::CreateTypes() {
  CPSFunctionArgsTy = Type::getInt8PtrTy(M->getContext());
  M->addTypeName("cps_function_args", CPSFunctionArgsTy);

  CPSFunctionRetTy = Type::getInt8PtrTy(M->getContext());
  M->addTypeName("cps_function_ret", CPSFunctionRetTy);

  PATypeHolder AbstractCPSContinuationTy = OpaqueType::get(M->getContext());
  PATypeHolder AbstractCPSFunctionTy = OpaqueType::get(M->getContext());

  std::vector<const Type*> Elts;
  Elts.push_back(PointerType::getUnqual(AbstractCPSContinuationTy)); // next continuation
  Elts.push_back(PointerType::getUnqual(AbstractCPSFunctionTy)); // continuation function
  Elts.push_back(CPSFunctionArgsTy); // continuation function args
  CPSContinuationTy = StructType::get(M->getContext(), Elts, false);

  std::vector<const Type*> Args;
  Args.push_back(AbstractCPSContinuationTy); // continuation
  Args.push_back(CPSFunctionArgsTy); // function args
  CPSFunctionTy = FunctionType::get(CPSFunctionRetTy, Args, false);

  cast<OpaqueType>(AbstractCPSContinuationTy.get())->refineAbstractTypeTo(CPSContinuationTy);
  cast<OpaqueType>(AbstractCPSFunctionTy.get())->refineAbstractTypeTo(CPSFunctionTy);
  CPSContinuationTy = cast<StructType>(AbstractCPSContinuationTy.get());
  CPSFunctionTy = cast<FunctionType>(AbstractCPSFunctionTy.get());

  M->addTypeName("cps_continuation", CPSContinuationTy);
  M->addTypeName("cps_function", CPSFunctionTy);
}

bool TransformToCPS::runOnModule(Module &m) {
  M = &m;
  FMap.clear();
  FMapInv.clear();
  NewF.clear();
  CreateTypes();
  if (!TransformFunctions()) 
    return false;
  TransformInstructions();
  return true;
}

