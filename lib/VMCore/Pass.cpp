//===- Pass.cpp - LLVM Pass Infrastructure Impementation ------------------===//
//
// This file implements the LLVM Pass infrastructure.  It is primarily
// responsible with ensuring that passes are executed and batched together
// optimally.
//
//===----------------------------------------------------------------------===//

#include "llvm/PassManager.h"
#include "PassManagerT.h"         // PassManagerT implementation
#include "llvm/Module.h"
#include "Support/STLExtras.h"
#include "Support/TypeInfo.h"
#include <typeinfo>
#include <sys/time.h>
#include <stdio.h>

//===----------------------------------------------------------------------===//
//   AnalysisID Class Implementation
//

static std::vector<const PassInfo*> CFGOnlyAnalyses;

void RegisterPassBase::setPreservesCFG() {
  CFGOnlyAnalyses.push_back(PIObj);
}

//===----------------------------------------------------------------------===//
//   AnalysisResolver Class Implementation
//

void AnalysisResolver::setAnalysisResolver(Pass *P, AnalysisResolver *AR) {
  assert(P->Resolver == 0 && "Pass already in a PassManager!");
  P->Resolver = AR;
}

//===----------------------------------------------------------------------===//
//   AnalysisUsage Class Implementation
//

// preservesCFG - This function should be called to by the pass, iff they do
// not:
//
//  1. Add or remove basic blocks from the function
//  2. Modify terminator instructions in any way.
//
// This function annotates the AnalysisUsage info object to say that analyses
// that only depend on the CFG are preserved by this pass.
//
void AnalysisUsage::preservesCFG() {
  // Since this transformation doesn't modify the CFG, it preserves all analyses
  // that only depend on the CFG (like dominators, loop info, etc...)
  //
  Preserved.insert(Preserved.end(),
                   CFGOnlyAnalyses.begin(), CFGOnlyAnalyses.end());
}


//===----------------------------------------------------------------------===//
// PassManager implementation - The PassManager class is a simple Pimpl class
// that wraps the PassManagerT template.
//
PassManager::PassManager() : PM(new PassManagerT<Module>()) {}
PassManager::~PassManager() { delete PM; }
void PassManager::add(Pass *P) { PM->add(P); }
bool PassManager::run(Module &M) { return PM->run(M); }


//===----------------------------------------------------------------------===//
// TimingInfo Class - This class is used to calculate information about the
// amount of time each pass takes to execute.  This only happens with
// -time-passes is enabled on the command line.
//
static cl::opt<bool>
EnableTiming("time-passes",
            cl::desc("Time each pass, printing elapsed time for each on exit"));

static double getTime() {
  struct timeval T;
  gettimeofday(&T, 0);
  return T.tv_sec + T.tv_usec/1000000.0;
}

// Create method.  If Timing is enabled, this creates and returns a new timing
// object, otherwise it returns null.
//
TimingInfo *TimingInfo::create() {
  return EnableTiming ? new TimingInfo() : 0;
}

void TimingInfo::passStarted(Pass *P) { TimingData[P] -= getTime(); }
void TimingInfo::passEnded(Pass *P) { TimingData[P] += getTime(); }

// TimingDtor - Print out information about timing information
TimingInfo::~TimingInfo() {
  // Iterate over all of the data, converting it into the dual of the data map,
  // so that the data is sorted by amount of time taken, instead of pointer.
  //
  std::vector<std::pair<double, Pass*> > Data;
  double TotalTime = 0;
  for (std::map<Pass*, double>::iterator I = TimingData.begin(),
         E = TimingData.end(); I != E; ++I)
    // Throw out results for "grouping" pass managers...
    if (!dynamic_cast<AnalysisResolver*>(I->first)) {
      Data.push_back(std::make_pair(I->second, I->first));
      TotalTime += I->second;
    }
  
  // Sort the data by time as the primary key, in reverse order...
  std::sort(Data.begin(), Data.end(), std::greater<std::pair<double, Pass*> >());

  // Print out timing header...
  std::cerr << std::string(79, '=') << "\n"
       << "                      ... Pass execution timing report ...\n"
       << std::string(79, '=') << "\n  Total Execution Time: " << TotalTime
       << " seconds\n\n  % Time: Seconds:\tPass Name:\n";

  // Loop through all of the timing data, printing it out...
  for (unsigned i = 0, e = Data.size(); i != e; ++i) {
    fprintf(stderr, "  %6.2f%% %fs\t%s\n", Data[i].first*100 / TotalTime,
            Data[i].first, Data[i].second->getPassName());
  }
  std::cerr << "  100.00% " << TotalTime << "s\tTOTAL\n"
       << std::string(79, '=') << "\n";
}


void PMDebug::PrintArgumentInformation(const Pass *P) {
  // Print out passes in pass manager...
  if (const AnalysisResolver *PM = dynamic_cast<const AnalysisResolver*>(P)) {
    for (unsigned i = 0, e = PM->getNumContainedPasses(); i != e; ++i)
      PrintArgumentInformation(PM->getContainedPass(i));

  } else {  // Normal pass.  Print argument information...
    // Print out arguments for registered passes that are _optimizations_
    if (const PassInfo *PI = P->getPassInfo())
      if (PI->getPassType() & PassInfo::Optimization)
        std::cerr << " -" << PI->getPassArgument();
  }
}

void PMDebug::PrintPassInformation(unsigned Depth, const char *Action,
                                   Pass *P, Annotable *V) {
  if (PassDebugging >= Executions) {
    std::cerr << (void*)P << std::string(Depth*2+1, ' ') << Action << " '" 
              << P->getPassName();
    if (V) {
      std::cerr << "' on ";

      if (dynamic_cast<Module*>(V)) {
        std::cerr << "Module\n"; return;
      } else if (Function *F = dynamic_cast<Function*>(V))
        std::cerr << "Function '" << F->getName();
      else if (BasicBlock *BB = dynamic_cast<BasicBlock*>(V))
        std::cerr << "BasicBlock '" << BB->getName();
      else if (Value *Val = dynamic_cast<Value*>(V))
        std::cerr << typeid(*Val).name() << " '" << Val->getName();
    }
    std::cerr << "'...\n";
  }
}

void PMDebug::PrintAnalysisSetInfo(unsigned Depth, const char *Msg,
                                   Pass *P, const std::vector<AnalysisID> &Set){
  if (PassDebugging >= Details && !Set.empty()) {
    std::cerr << (void*)P << std::string(Depth*2+3, ' ') << Msg << " Analyses:";
    for (unsigned i = 0; i != Set.size(); ++i)
      std::cerr << "  " << Set[i]->getPassName();
    std::cerr << "\n";
  }
}

//===----------------------------------------------------------------------===//
// Pass Implementation
//

void Pass::addToPassManager(PassManagerT<Module> *PM, AnalysisUsage &AU) {
  PM->addPass(this, AU);
}

// dumpPassStructure - Implement the -debug-passes=Structure option
void Pass::dumpPassStructure(unsigned Offset) {
  std::cerr << std::string(Offset*2, ' ') << getPassName() << "\n";
}

// getPassName - Use C++ RTTI to get a SOMEWHAT intelligable name for the pass.
//
const char *Pass::getPassName() const {
  if (const PassInfo *PI = getPassInfo())
    return PI->getPassName();
  return typeid(*this).name();
}

// print - Print out the internal state of the pass.  This is called by Analyse
// to print out the contents of an analysis.  Otherwise it is not neccesary to
// implement this method.
//
void Pass::print(std::ostream &O) const {
  O << "Pass::print not implemented for pass: '" << getPassName() << "'!\n";
}

// dump - call print(std::cerr);
void Pass::dump() const {
  print(std::cerr, 0);
}

//===----------------------------------------------------------------------===//
// FunctionPass Implementation
//

// run - On a module, we run this pass by initializing, runOnFunction'ing once
// for every function in the module, then by finalizing.
//
bool FunctionPass::run(Module &M) {
  bool Changed = doInitialization(M);
  
  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I)
    if (!I->isExternal())      // Passes are not run on external functions!
    Changed |= runOnFunction(*I);
  
  return Changed | doFinalization(M);
}

// run - On a function, we simply initialize, run the function, then finalize.
//
bool FunctionPass::run(Function &F) {
  if (F.isExternal()) return false;// Passes are not run on external functions!

  return doInitialization(*F.getParent()) | runOnFunction(F)
       | doFinalization(*F.getParent());
}

void FunctionPass::addToPassManager(PassManagerT<Module> *PM,
                                    AnalysisUsage &AU) {
  PM->addPass(this, AU);
}

void FunctionPass::addToPassManager(PassManagerT<Function> *PM,
                                    AnalysisUsage &AU) {
  PM->addPass(this, AU);
}

//===----------------------------------------------------------------------===//
// BasicBlockPass Implementation
//

// To run this pass on a function, we simply call runOnBasicBlock once for each
// function.
//
bool BasicBlockPass::runOnFunction(Function &F) {
  bool Changed = false;
  for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I)
    Changed |= runOnBasicBlock(*I);
  return Changed;
}

// To run directly on the basic block, we initialize, runOnBasicBlock, then
// finalize.
//
bool BasicBlockPass::run(BasicBlock &BB) {
  Module &M = *BB.getParent()->getParent();
  return doInitialization(M) | runOnBasicBlock(BB) | doFinalization(M);
}

void BasicBlockPass::addToPassManager(PassManagerT<Function> *PM,
                                      AnalysisUsage &AU) {
  PM->addPass(this, AU);
}

void BasicBlockPass::addToPassManager(PassManagerT<BasicBlock> *PM,
                                      AnalysisUsage &AU) {
  PM->addPass(this, AU);
}


//===----------------------------------------------------------------------===//
// Pass Registration mechanism
//
static std::map<TypeInfo, PassInfo*> *PassInfoMap = 0;
static std::vector<PassRegistrationListener*> *Listeners = 0;

// getPassInfo - Return the PassInfo data structure that corresponds to this
// pass...
const PassInfo *Pass::getPassInfo() const {
  if (PassInfoCache) return PassInfoCache;
  if (PassInfoMap == 0) return 0;
  std::map<TypeInfo, PassInfo*>::iterator I = PassInfoMap->find(typeid(*this));
  return (I != PassInfoMap->end()) ? I->second : 0;
}

void RegisterPassBase::registerPass(PassInfo *PI) {
  if (PassInfoMap == 0)
    PassInfoMap = new std::map<TypeInfo, PassInfo*>();

  assert(PassInfoMap->find(PI->getTypeInfo()) == PassInfoMap->end() &&
         "Pass already registered!");
  PIObj = PI;
  PassInfoMap->insert(std::make_pair(TypeInfo(PI->getTypeInfo()), PI));

  // Notify any listeners...
  if (Listeners)
    for (std::vector<PassRegistrationListener*>::iterator
           I = Listeners->begin(), E = Listeners->end(); I != E; ++I)
      (*I)->passRegistered(PI);
}

RegisterPassBase::~RegisterPassBase() {
  assert(PassInfoMap && "Pass registered but not in map!");
  std::map<TypeInfo, PassInfo*>::iterator I =
    PassInfoMap->find(PIObj->getTypeInfo());
  assert(I != PassInfoMap->end() && "Pass registered but not in map!");

  // Remove pass from the map...
  PassInfoMap->erase(I);
  if (PassInfoMap->empty()) {
    delete PassInfoMap;
    PassInfoMap = 0;
  }

  // Notify any listeners...
  if (Listeners)
    for (std::vector<PassRegistrationListener*>::iterator
           I = Listeners->begin(), E = Listeners->end(); I != E; ++I)
      (*I)->passUnregistered(PIObj);

  // Delete the PassInfo object itself...
  delete PIObj;
}



//===----------------------------------------------------------------------===//
// PassRegistrationListener implementation
//

// PassRegistrationListener ctor - Add the current object to the list of
// PassRegistrationListeners...
PassRegistrationListener::PassRegistrationListener() {
  if (!Listeners) Listeners = new std::vector<PassRegistrationListener*>();
  Listeners->push_back(this);
}

// dtor - Remove object from list of listeners...
PassRegistrationListener::~PassRegistrationListener() {
  std::vector<PassRegistrationListener*>::iterator I =
    std::find(Listeners->begin(), Listeners->end(), this);
  assert(Listeners && I != Listeners->end() &&
         "PassRegistrationListener not registered!");
  Listeners->erase(I);

  if (Listeners->empty()) {
    delete Listeners;
    Listeners = 0;
  }
}

// enumeratePasses - Iterate over the registered passes, calling the
// passEnumerate callback on each PassInfo object.
//
void PassRegistrationListener::enumeratePasses() {
  if (PassInfoMap)
    for (std::map<TypeInfo, PassInfo*>::iterator I = PassInfoMap->begin(),
           E = PassInfoMap->end(); I != E; ++I)
      passEnumerate(I->second);
}
