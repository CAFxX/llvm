/* Title:   FunctionLiveVarInfo.h             -*- C++ -*-
   Author:  Ruchira Sasanka
   Date:    Jun 30, 01
   Purpose: 

   This is the interface for live variable info of a function that is required 
   by any other part of the compiler

   It must be called like:

       FunctionLiveVarInfo FLVI(Function *);  // initializes data structures
       FLVI.analyze();                     // do the actural live variable anal

 After the analysis, getInSetOfBB or getOutSetofBB can be called to get 
 live var info of a BB.

 The live var set before an instruction can be obtained in 2 ways:

 1. Use the method getLiveVarSetAfterInst(Instruction *) to get the LV Info 
    just after an instruction. (also exists getLiveVarSetBeforeInst(..))

    This function caluclates the LV info for a BB only once and caches that 
    info. If the cache does not contain the LV info of the instruction, it 
    calculates the LV info for the whole BB and caches them.

    Getting liveVar info this way uses more memory since, LV info should be 
    cached. However, if you need LV info of nearly all the instructions of a
    BB, this is the best and simplest interfrace.


 2. Use the OutSet and applyTranferFuncForInst(const Instruction *const Inst) 
    declared in LiveVarSet and  traverse the instructions of a basic block in 
    reverse (using const_reverse_iterator in the BB class). 

    This is the most memory efficient method if you need LV info for 
    only several instructions in a BasicBlock. An example is given below:


    LiveVarSet LVSet;  // this will be the set used to traverse through each BB

    // Initialize LVSet so that it is the same as OutSet of the BB
    LVSet.setUnion( LVI->getOutSetOfBB( *BBI ) );  
 
    BasicBlock::InstListType::const_reverse_iterator 
      InstIterator = InstListInBB.rbegin(); // get the rev iter for inst in BB

      // iterate over all the instructions in BB in reverse
    for( ; InstIterator != InstListInBB.rend(); InstIterator++) {  

      //...... all  code here which uses LVSet ........

      LVSet.applyTranferFuncForInst(*InstIterator);

      // Now LVSet contains live vars ABOVE the current instrution
    }

    See buildInterferenceGraph() for the above example.
*/


#ifndef METH_LIVE_VAR_INFO_H
#define METH_LIVE_VAR_INFO_H

#include "llvm/Pass.h"
#include "llvm/Analysis/LiveVar/ValueSet.h"

class BBLiveVar;
class MachineInstr;

class FunctionLiveVarInfo : public FunctionPass {
  // Machine Instr to LiveVarSet Map for providing LVset BEFORE each inst
  std::map<const MachineInstr *, const ValueSet *> MInst2LVSetBI; 

  // Machine Instr to LiveVarSet Map for providing LVset AFTER each inst
  std::map<const MachineInstr *, const ValueSet *> MInst2LVSetAI; 

  // Stored Function that the data is computed with respect to
  const Function *M;

  // --------- private methods -----------------------------------------

  // constructs BBLiveVars and init Def and In sets
  void constructBBs(const Function *F);
    
  // do one backward pass over the CFG
  bool doSingleBackwardPass(const Function *F, unsigned int iter); 

  // calculates live var sets for instructions in a BB
  void calcLiveVarSetsForBB(const BasicBlock *BB);
  
public:
  static AnalysisID ID;    // We are an analysis, we must have an ID

  FunctionLiveVarInfo(AnalysisID id = ID) { assert(id == ID); }

  virtual const char *getPassName() const { return "Live Variable Analysis"; }

  // --------- Implement the FunctionPass interface ----------------------

  // runOnFunction - Perform analysis, update internal data structures.
  virtual bool runOnFunction(Function *F);

  // releaseMemory - After LiveVariable analysis has been used, forget!
  virtual void releaseMemory();

  // getAnalysisUsage - Provide self!
  virtual void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.setPreservesAll();
    AU.addProvided(ID);
  }

  // --------- Functions to access analysis results -------------------

  // gets OutSet of a BB
  const ValueSet &getOutSetOfBB(const BasicBlock *BB) const;

  // gets InSet of a BB
  const ValueSet &getInSetOfBB(const BasicBlock *BB) const;

  // gets the Live var set BEFORE an instruction
  const ValueSet &getLiveVarSetBeforeMInst(const MachineInstr *MI,
                                           const BasicBlock *BB);

  // gets the Live var set AFTER an instruction
  const ValueSet &getLiveVarSetAfterMInst(const MachineInstr *MI,
                                          const BasicBlock *BB);
};

#endif
