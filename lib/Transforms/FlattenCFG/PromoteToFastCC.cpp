//===-- PromoteToFastCC.cpp - Promote functions to fast calling conv. -----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass clones all functions in the module called from within the module
// itself, changing the calling convention of the cloned function to fastcc.
// All calls to these functions are then replaced with calls to the fastcc
// version.
//
// Optionally, the bodies of the original functions are replaced with calls
// to the fastcc versions to reduce code size and improve cache locality. 
//
// TODO: If we detect that a call is unlikely we can either use coldcc for 
// the cloned function (if all callsites are unlikely) or a coldcc wrapper
// of the fastcc version.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "promotecc"
#include "FlattenCFG.h"

STATISTIC(numFastFunctions, "Number of functions changed to FastCC");
STATISTIC(numColdFunctions, "Number of ColdCC forwarding wrappers");
STATISTIC(numFastCallsites, "Number of callsites changed to FastCC");
STATISTIC(numColdCallsites, "Number of callsites changed to ColdCC");

namespace PassNS {
  class PromoteCC : public ModulePass {
    private:
      Module* M;
      FunctionMap FastFunc;
      FunctionMap ColdFunc;

      void ReplaceAllCallSites();
      void ReplaceCallSite(CallInst* ci);

    public :
      static char ID; // Pass identification, replacement for typeid
      PromoteCC() : ModulePass(ID), FastFunc(), ColdFunc() {
      }
      bool runOnModule(Module &_M);
  };

}

PassBoilerplate(PromoteCC, "promotecc", "Promote functions to fastcc or coldcc")

// Turn function Wrapper into a forwarding wrapper to function Target
static void Wrapperize(Function *Wrapper, Function *Target) {
  //errs() << "Wrapperize: " << to_string(Target) << " <- " << to_string(Wrapper) << "\n";
  assert(Target->getCallingConv() != Wrapper->getCallingConv() && "Target and Wrapper have the same calling convention. What's the point, then?");

  // empty the body of function Wrapper
  Wrapper->dropAllReferences();
  for (Function::iterator b = Wrapper->begin(), be = Wrapper->end(); b != be; ++b)
    b->eraseFromParent();
  // create a new entry bb for Wrapper
  BasicBlock *BB = BasicBlock::Create(Wrapper->getContext(), "entry", Wrapper, NULL);
  // create the forwarding call and insert it into BB
  std::vector<Value*> forwardedArgs;
  for (Function::arg_iterator a = Wrapper->arg_begin(), ae = Wrapper->arg_end(); a != ae; ++a)
    forwardedArgs.push_back((Value*)&*a);
  CallInst *CI = CallInst::Create(Target, forwardedArgs.begin(), forwardedArgs.end(), "", BB);
  CI->setCallingConv(Target->getCallingConv());
  CI->setTailCall(); // this call is the only instruction in the wrapper, so it's always in tail position
  // return the result of forwarding call CI
  ReturnInst::Create(Wrapper->getContext(), CI, BB);
}

static Function* CloneFunctionWithCC(Function *F, CallingConv::ID CC, bool MakeWrapper) {
  ValueToValueMapTy VMap;
  Function* NF = CloneFunction(F, VMap, false);
  NF->setCallingConv(CC);
  NF->setLinkage(GlobalValue::InternalLinkage);
  NF->setName(F->getName() + "_" + DEBUG_TYPE + to_string(CC));
  F->getParent()->getFunctionList().push_back(NF);
  if (MakeWrapper) {
    Wrapperize(F, NF);
  }
  return NF;
}

static Function* CreateWrapperWithCC(Function *F, CallingConv::ID CC) {
  Function *NF = Function::Create(
    F->getFunctionType(), 
    GlobalValue::InternalLinkage, 
    F->getName() + "_" + DEBUG_TYPE + to_string(CC)
  );
  NF->setCallingConv(CC);
  F->getParent()->getFunctionList().push_back(NF);
  Wrapperize(NF, F);
  return NF;
}

bool IsRarelyExecuted(CallSite& cs) {
  return false; // TODO return if this callsite is rarely executed
}

void PromoteCC::ReplaceAllCallSites() {
  for (Module::iterator f = M->begin(), fe = M->end(); f != fe; ++f)
    for (Function::iterator b = f->begin(), be = f->end(); b != be; ++b)
      for (BasicBlock::iterator i = b->begin(), ie = b->end(); i != ie; ++i)
        if (CallInst* ci = dyn_cast<CallInst>(&*i))
          ReplaceCallSite(ci);
}

void PromoteCC::ReplaceCallSite(CallInst* ci) {
  CallSite cs(ci);        
  Function *F = cs.getCalledFunction();
  if (ColdFunc.count(F) && IsRarelyExecuted(cs)) {
    cs.setCalledFunction(ColdFunc[F]);
    cs.setCallingConv(CallingConv::Cold);
    numColdCallsites++;           
  } else if (FastFunc.count(F)) {
    cs.setCalledFunction(FastFunc[F]);
    cs.setCallingConv(CallingConv::Fast);
    numFastCallsites++;
  }
}

bool PromoteCC::runOnModule(Module &m) {
  FastFunc.clear();
  ColdFunc.clear();
  M = &m;
  bool Changed = false;
  bool CreateColdCCWrapper = false; // TODO expose this
  bool ReplaceOriginalWithWrapper = true;
  std::set<Function*> newFunctions;
  for (Module::iterator F = M->begin(); F != M->end(); ++F) {
    if (F->isDeclaration())
      continue;
  	if (F->mayBeOverridden())
      continue;
    if (newFunctions.count(F)) 
      continue;
    if (F->getCallingConv() != CallingConv::Fast) {
      Function* FN = CloneFunctionWithCC(F, CallingConv::Fast, ReplaceOriginalWithWrapper); 
      FastFunc[F] = FN;
      newFunctions.insert(FN);
      Changed = true;
    }
    if (CreateColdCCWrapper && F->getCallingConv() != CallingConv::Cold) {
      Function* FN = CreateWrapperWithCC(FastFunc[F], CallingConv::Cold);
      FN->setName(F->getName() + "_" + DEBUG_TYPE + to_string(CallingConv::Cold));
      ColdFunc[F] = FN;
      newFunctions.insert(FN);
      Changed = true;
    }
  }
  ReplaceAllCallSites();
  numFastFunctions = FastFunc.size();
  numColdFunctions = ColdFunc.size();
  assert(numFastFunctions + numColdFunctions == newFunctions.size());
  return Changed;
}

