//===- llvm/PassManager.h - Container for Passes -----------------*- C++ -*--=//
//
// This file defines the PassManager class.  This class is used to hold,
// maintain, and optimize execution of Pass's.  The PassManager class ensures
// that analysis results are available before a pass runs, and that Pass's are
// destroyed when the PassManager is destroyed.
//
// The PassManagerT template is instantiated three times to do its job.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_PASSMANAGER_H
#define LLVM_PASSMANAGER_H

#include "llvm/Pass.h"
#include <string>
#include <algorithm>
class Annotable;

//===----------------------------------------------------------------------===//
// PMDebug class - a set of debugging functions, that are not to be
// instantiated by the template.
//
struct PMDebug {
  // If compiled in debug mode, these functions can be enabled by setting
  // -debug-pass on the command line of the tool being used.
  //
  static void PrintPassStructure(Pass *P);
  static void PrintPassInformation(unsigned,const char*,Pass *, Annotable *);
  static void PrintAnalysisSetInfo(unsigned,const char*,Pass *P,
                                   const std::vector<AnalysisID> &);
};



//===----------------------------------------------------------------------===//
// Declare the PassManagerTraits which will be specialized...
//
template<class UnitType> class PassManagerTraits;   // Do not define.


//===----------------------------------------------------------------------===//
// PassManagerT - Container object for passes.  The PassManagerT destructor
// deletes all passes contained inside of the PassManagerT, so you shouldn't 
// delete passes manually, and all passes should be dynamically allocated.
//
template<typename UnitType>
class PassManagerT : public PassManagerTraits<UnitType>,public AnalysisResolver{
  typedef typename PassManagerTraits<UnitType>::PassClass       PassClass;
  typedef typename PassManagerTraits<UnitType>::SubPassClass SubPassClass;
  typedef typename PassManagerTraits<UnitType>::BatcherClass BatcherClass;
  typedef typename PassManagerTraits<UnitType>::ParentClass   ParentClass;
  typedef          PassManagerTraits<UnitType>                     Traits;

  friend typename PassManagerTraits<UnitType>::PassClass;
  friend typename PassManagerTraits<UnitType>::SubPassClass;  
  friend class PassManagerTraits<UnitType>;

  std::vector<PassClass*> Passes;    // List of pass's to run

  // The parent of this pass manager...
  ParentClass * const Parent;

  // The current batcher if one is in use, or null
  BatcherClass *Batcher;

  // CurrentAnalyses - As the passes are being run, this map contains the
  // analyses that are available to the current pass for use.  This is accessed
  // through the getAnalysis() function in this class and in Pass.
  //
  std::map<AnalysisID, Pass*> CurrentAnalyses;

  // LastUseOf - This map keeps track of the last usage in our pipeline of a
  // particular pass.  When executing passes, the memory for .first is free'd
  // after .second is run.
  //
  std::map<Pass*, Pass*> LastUseOf;

public:
  PassManagerT(ParentClass *Par = 0) : Parent(Par), Batcher(0) {}
  ~PassManagerT() {
    // Delete all of the contained passes...
    for (std::vector<PassClass*>::iterator I = Passes.begin(), E = Passes.end();
         I != E; ++I)
      delete *I;
  }

  // run - Run all of the queued passes on the specified module in an optimal
  // way.
  virtual bool runOnUnit(UnitType *M) {
    bool MadeChanges = false;
    closeBatcher();
    CurrentAnalyses.clear();

    // LastUserOf - This contains the inverted LastUseOfMap...
    std::map<Pass *, std::vector<Pass*> > LastUserOf;
    for (std::map<Pass*, Pass*>::iterator I = LastUseOf.begin(),
                                          E = LastUseOf.end(); I != E; ++I)
      LastUserOf[I->second].push_back(I->first);


    // Output debug information...
    if (Parent == 0) PMDebug::PrintPassStructure(this);

    // Run all of the passes
    for (unsigned i = 0, e = Passes.size(); i < e; ++i) {
      PassClass *P = Passes[i];
      
      PMDebug::PrintPassInformation(getDepth(), "Executing Pass", P,
                                    (Annotable*)M);

      // Get information about what analyses the pass uses...
      AnalysisUsage AnUsage;
      P->getAnalysisUsage(AnUsage);
      PMDebug::PrintAnalysisSetInfo(getDepth(), "Required", P,
                                    AnUsage.getRequiredSet());

#ifndef NDEBUG
      // All Required analyses should be available to the pass as it runs!
      for (vector<AnalysisID>::const_iterator
             I = AnUsage.getRequiredSet().begin(), 
             E = AnUsage.getRequiredSet().end(); I != E; ++I) {
        assert(getAnalysisOrNullUp(*I) && "Analysis used but not available!");
      }
#endif

      // Run the sub pass!
      bool Changed = Traits::runPass(P, M);
      MadeChanges |= Changed;

      if (Changed)
        PMDebug::PrintPassInformation(getDepth()+1, "Made Modification", P,
                                      (Annotable*)M);
      PMDebug::PrintAnalysisSetInfo(getDepth(), "Preserved", P,
                                    AnUsage.getPreservedSet());
      PMDebug::PrintAnalysisSetInfo(getDepth(), "Provided", P,
                                    AnUsage.getProvidedSet());


      // Erase all analyses not in the preserved set...
      if (!AnUsage.preservesAll()) {
        const std::vector<AnalysisID> &PreservedSet = AnUsage.getPreservedSet();
        for (std::map<AnalysisID, Pass*>::iterator I = CurrentAnalyses.begin(),
               E = CurrentAnalyses.end(); I != E; )
          if (std::find(PreservedSet.begin(), PreservedSet.end(), I->first) !=
              PreservedSet.end())
            ++I; // This analysis is preserved, leave it in the available set...
          else {
#if MAP_DOESNT_HAVE_BROKEN_ERASE_MEMBER
            I = CurrentAnalyses.erase(I);   // Analysis not preserved!
#else
            // GCC 2.95.3 STL doesn't have correct erase member!
            CurrentAnalyses.erase(I);
            I = CurrentAnalyses.begin();
#endif
          }
      }

      // Add all analyses in the provided set...
      for (std::vector<AnalysisID>::const_iterator
             I = AnUsage.getProvidedSet().begin(),
             E = AnUsage.getProvidedSet().end(); I != E; ++I)
        CurrentAnalyses[*I] = P;

      // Free memory for any passes that we are the last use of...
      std::vector<Pass*> &DeadPass = LastUserOf[P];
      for (std::vector<Pass*>::iterator I = DeadPass.begin(),E = DeadPass.end();
           I != E; ++I) {
        PMDebug::PrintPassInformation(getDepth()+1, "Freeing Pass", *I,
                                      (Annotable*)M);
        (*I)->releaseMemory();
      }
    }
    return MadeChanges;
  }

  // dumpPassStructure - Implement the -debug-passes=PassStructure option
  virtual void dumpPassStructure(unsigned Offset = 0) {
    std::cerr << std::string(Offset*2, ' ') << Traits::getPMName()
              << " Pass Manager\n";
    for (std::vector<PassClass*>::iterator I = Passes.begin(), E = Passes.end();
         I != E; ++I) {
      PassClass *P = *I;
      P->dumpPassStructure(Offset+1);

      // Loop through and see which classes are destroyed after this one...
      for (std::map<Pass*, Pass*>::iterator I = LastUseOf.begin(),
                                            E = LastUseOf.end(); I != E; ++I) {
        if (P == I->second) {
          std::cerr << "Fr" << std::string(Offset*2, ' ');
          I->first->dumpPassStructure(0);
        }
      }
    }
  }

  Pass *getAnalysisOrNullDown(AnalysisID ID) const {
    std::map<AnalysisID, Pass*>::const_iterator I = CurrentAnalyses.find(ID);
    if (I == CurrentAnalyses.end()) {
      if (Batcher)
        return ((AnalysisResolver*)Batcher)->getAnalysisOrNullDown(ID);
      return 0;
    }
    return I->second;
  }

  Pass *getAnalysisOrNullUp(AnalysisID ID) const {
    std::map<AnalysisID, Pass*>::const_iterator I = CurrentAnalyses.find(ID);
    if (I == CurrentAnalyses.end()) {
      if (Parent)
        return Parent->getAnalysisOrNullUp(ID);
      return 0;
    }
    return I->second;
  }

  // markPassUsed - Inform higher level pass managers (and ourselves)
  // that these analyses are being used by this pass.  This is used to
  // make sure that analyses are not free'd before we have to use
  // them...
  //
  void markPassUsed(AnalysisID P, Pass *User) {
    std::map<AnalysisID, Pass*>::iterator I = CurrentAnalyses.find(P);
    if (I != CurrentAnalyses.end()) {
      LastUseOf[I->second] = User;    // Local pass, extend the lifetime
    } else {
      // Pass not in current available set, must be a higher level pass
      // available to us, propogate to parent pass manager...  We tell the
      // parent that we (the passmanager) are using the analysis so that it
      // frees the analysis AFTER this pass manager runs.
      //
      assert(Parent != 0 && "Pass available but not found! "
             "Did your analysis pass 'Provide' itself?");
      Parent->markPassUsed(P, this);
    }
  }

  // Return the number of parent PassManagers that exist
  virtual unsigned getDepth() const {
    if (Parent == 0) return 0;
    return 1 + Parent->getDepth();
  }

  // add - Add a pass to the queue of passes to run.  This passes ownership of
  // the Pass to the PassManager.  When the PassManager is destroyed, the pass
  // will be destroyed as well, so there is no need to delete the pass.  This
  // implies that all passes MUST be new'd.
  //
  void add(PassClass *P) {
    // Get information about what analyses the pass uses...
    AnalysisUsage AnUsage;
    P->getAnalysisUsage(AnUsage);
    const std::vector<AnalysisID> &Required = AnUsage.getRequiredSet();

    // Loop over all of the analyses used by this pass,
    for (std::vector<AnalysisID>::const_iterator I = Required.begin(),
           E = Required.end(); I != E; ++I) {
      if (getAnalysisOrNullDown(*I) == 0)
        add((PassClass*)I->createPass());
    }

    // Tell the pass to add itself to this PassManager... the way it does so
    // depends on the class of the pass, and is critical to laying out passes in
    // an optimal order..
    //
    P->addToPassManager(this, AnUsage);
  }

private:

  // addPass - These functions are used to implement the subclass specific
  // behaviors present in PassManager.  Basically the add(Pass*) method ends up
  // reflecting its behavior into a Pass::addToPassManager call.  Subclasses of
  // Pass override it specifically so that they can reflect the type
  // information inherent in "this" back to the PassManager.
  //
  // For generic Pass subclasses (which are interprocedural passes), we simply
  // add the pass to the end of the pass list and terminate any accumulation of
  // FunctionPass's that are present.
  //
  void addPass(PassClass *P, AnalysisUsage &AnUsage) {
    const std::vector<AnalysisID> &RequiredSet = AnUsage.getRequiredSet();
    const std::vector<AnalysisID> &ProvidedSet = AnUsage.getProvidedSet();

    // Providers are analysis classes which are forbidden to modify the module
    // they are operating on, so they are allowed to be reordered to before the
    // batcher...
    //
    if (Batcher && ProvidedSet.empty())
      closeBatcher();                     // This pass cannot be batched!
    
    // Set the Resolver instance variable in the Pass so that it knows where to 
    // find this object...
    //
    setAnalysisResolver(P, this);
    Passes.push_back(P);

    // Inform higher level pass managers (and ourselves) that these analyses are
    // being used by this pass.  This is used to make sure that analyses are not
    // free'd before we have to use them...
    //
    for (std::vector<AnalysisID>::const_iterator I = RequiredSet.begin(),
           E = RequiredSet.end(); I != E; ++I)
      markPassUsed(*I, P);     // Mark *I as used by P

    // Erase all analyses not in the preserved set...
    if (!AnUsage.preservesAll()) {
      const std::vector<AnalysisID> &PreservedSet = AnUsage.getPreservedSet();
      for (std::map<AnalysisID, Pass*>::iterator I = CurrentAnalyses.begin(),
             E = CurrentAnalyses.end(); I != E; )
        if (std::find(PreservedSet.begin(), PreservedSet.end(), I->first) !=
            PreservedSet.end())
          ++I;  // This analysis is preserved, leave it in the available set...
        else {
#if MAP_DOESNT_HAVE_BROKEN_ERASE_MEMBER
          I = CurrentAnalyses.erase(I);   // Analysis not preserved!
#else
          CurrentAnalyses.erase(I);// GCC 2.95.3 STL doesn't have correct erase!
          I = CurrentAnalyses.begin();
#endif
        }
    }

    // Add all analyses in the provided set...
    for (std::vector<AnalysisID>::const_iterator I = ProvidedSet.begin(),
           E = ProvidedSet.end(); I != E; ++I)
      CurrentAnalyses[*I] = P;

    // For now assume that our results are never used...
    LastUseOf[P] = P;
  }
  
  // For FunctionPass subclasses, we must be sure to batch the FunctionPass's
  // together in a BatcherClass object so that all of the analyses are run
  // together a function at a time.
  //
  void addPass(SubPassClass *MP, AnalysisUsage &AnUsage) {
    if (Batcher == 0) // If we don't have a batcher yet, make one now.
      Batcher = new BatcherClass(this);
    // The Batcher will queue them passes up
    MP->addToPassManager(Batcher, AnUsage);
  }

  // closeBatcher - Terminate the batcher that is being worked on.
  void closeBatcher() {
    if (Batcher) {
      Passes.push_back(Batcher);
      Batcher = 0;
    }
  }
};



//===----------------------------------------------------------------------===//
// PassManagerTraits<BasicBlock> Specialization
//
// This pass manager is used to group together all of the BasicBlockPass's
// into a single unit.
//
template<> struct PassManagerTraits<BasicBlock> : public BasicBlockPass {
  // PassClass - The type of passes tracked by this PassManager
  typedef BasicBlockPass PassClass;

  // SubPassClass - The types of classes that should be collated together
  // This is impossible to match, so BasicBlock instantiations of PassManagerT
  // do not collate.
  //
  typedef PassManagerT<Module> SubPassClass;

  // BatcherClass - The type to use for collation of subtypes... This class is
  // never instantiated for the PassManager<BasicBlock>, but it must be an 
  // instance of PassClass to typecheck.
  //
  typedef PassClass BatcherClass;

  // ParentClass - The type of the parent PassManager...
  typedef PassManagerT<Function> ParentClass;

  // PMType - The type of the passmanager that subclasses this class
  typedef PassManagerT<BasicBlock> PMType;

  // runPass - Specify how the pass should be run on the UnitType
  static bool runPass(PassClass *P, BasicBlock *M) {
    // todo, init and finalize
    return P->runOnBasicBlock(M);
  }

  // getPMName() - Return the name of the unit the PassManager operates on for
  // debugging.
  const char *getPMName() const { return "BasicBlock"; }

  // Implement the BasicBlockPass interface...
  virtual bool doInitialization(Module *M);
  virtual bool runOnBasicBlock(BasicBlock *BB);
  virtual bool doFinalization(Module *M);
};



//===----------------------------------------------------------------------===//
// PassManagerTraits<Function> Specialization
//
// This pass manager is used to group together all of the FunctionPass's
// into a single unit.
//
template<> struct PassManagerTraits<Function> : public FunctionPass {
  // PassClass - The type of passes tracked by this PassManager
  typedef FunctionPass PassClass;

  // SubPassClass - The types of classes that should be collated together
  typedef BasicBlockPass SubPassClass;

  // BatcherClass - The type to use for collation of subtypes...
  typedef PassManagerT<BasicBlock> BatcherClass;

  // ParentClass - The type of the parent PassManager...
  typedef PassManagerT<Module> ParentClass;

  // PMType - The type of the passmanager that subclasses this class
  typedef PassManagerT<Function> PMType;

  // runPass - Specify how the pass should be run on the UnitType
  static bool runPass(PassClass *P, Function *F) {
    return P->runOnFunction(F);
  }

  // getPMName() - Return the name of the unit the PassManager operates on for
  // debugging.
  const char *getPMName() const { return "Function"; }

  // Implement the FunctionPass interface...
  virtual bool doInitialization(Module *M);
  virtual bool runOnFunction(Function *F);
  virtual bool doFinalization(Module *M);
};



//===----------------------------------------------------------------------===//
// PassManagerTraits<Module> Specialization
//
// This is the top level PassManager implementation that holds generic passes.
//
template<> struct PassManagerTraits<Module> : public Pass {
  // PassClass - The type of passes tracked by this PassManager
  typedef Pass PassClass;

  // SubPassClass - The types of classes that should be collated together
  typedef FunctionPass SubPassClass;

  // BatcherClass - The type to use for collation of subtypes...
  typedef PassManagerT<Function> BatcherClass;

  // ParentClass - The type of the parent PassManager...
  typedef AnalysisResolver ParentClass;

  // runPass - Specify how the pass should be run on the UnitType
  static bool runPass(PassClass *P, Module *M) { return P->run(M); }

  // getPMName() - Return the name of the unit the PassManager operates on for
  // debugging.
  const char *getPMName() const { return "Module"; }

  // run - Implement the Pass interface...
  virtual bool run(Module *M) {
    return ((PassManagerT<Module>*)this)->runOnUnit(M);
  }
};



//===----------------------------------------------------------------------===//
// PassManagerTraits Method Implementations
//

// PassManagerTraits<BasicBlock> Implementations
//
inline bool PassManagerTraits<BasicBlock>::doInitialization(Module *M) {
  bool Changed = false;
  for (unsigned i = 0, e = ((PMType*)this)->Passes.size(); i != e; ++i)
    ((PMType*)this)->Passes[i]->doInitialization(M);
  return Changed;
}

inline bool PassManagerTraits<BasicBlock>::runOnBasicBlock(BasicBlock *BB) {
  return ((PMType*)this)->runOnUnit(BB);
}

inline bool PassManagerTraits<BasicBlock>::doFinalization(Module *M) {
  bool Changed = false;
  for (unsigned i = 0, e = ((PMType*)this)->Passes.size(); i != e; ++i)
    ((PMType*)this)->Passes[i]->doFinalization(M);
  return Changed;
}


// PassManagerTraits<Function> Implementations
//
inline bool PassManagerTraits<Function>::doInitialization(Module *M) {
  bool Changed = false;
  for (unsigned i = 0, e = ((PMType*)this)->Passes.size(); i != e; ++i)
    ((PMType*)this)->Passes[i]->doInitialization(M);
  return Changed;
}

inline bool PassManagerTraits<Function>::runOnFunction(Function *F) {
  return ((PMType*)this)->runOnUnit(F);
}

inline bool PassManagerTraits<Function>::doFinalization(Module *M) {
  bool Changed = false;
  for (unsigned i = 0, e = ((PMType*)this)->Passes.size(); i != e; ++i)
    ((PMType*)this)->Passes[i]->doFinalization(M);
  return Changed;
}

#endif
