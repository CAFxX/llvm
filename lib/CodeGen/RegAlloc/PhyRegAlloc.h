/* Title:   PhyRegAlloc.h
   Author:  Ruchira Sasanka
   Date:    Aug 20, 01
   Purpose: This is the main entry point for register allocation.

   Notes:
   =====

 * RegisterClasses: Each RegClass accepts a 
   MachineRegClass which contains machine specific info about that register
   class. The code in the RegClass is machine independent and they use
   access functions in the MachineRegClass object passed into it to get
   machine specific info.

 * Machine dependent work: All parts of the register coloring algorithm
   except coloring of an individual node are machine independent.

   Register allocation must be done  as:	

      MethodLiveVarInfo LVI(*MethodI );           // compute LV info
      LVI.analyze();

      TargetMachine &target = ....	                        


      PhyRegAlloc PRA(*MethodI, target, &LVI);     // allocate regs
      PRA.allocateRegisters();



*/ 

#ifndef PHY_REG_ALLOC_H
#define PHY_REG_ALLOC_H

#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/RegClass.h"
#include "llvm/CodeGen/LiveRangeInfo.h"
#include "llvm/Analysis/LiveVar/MethodLiveVarInfo.h"
#include "llvm/Analysis/LoopDepth.h"

#include <deque>


//----------------------------------------------------------------------------
// Class AddedInstrns:
// When register allocator inserts new instructions in to the existing 
// instruction stream, it does NOT directly modify the instruction stream.
// Rather, it creates an object of AddedInstrns and stick it in the 
// AddedInstrMap for an existing instruction. This class contains two vectors
// to store such instructions added before and after an existing instruction.
//----------------------------------------------------------------------------

class AddedInstrns
{
 public:
  deque<MachineInstr *> InstrnsBefore;  // Added insts BEFORE an existing inst
  deque<MachineInstr *> InstrnsAfter;   // Added insts AFTER an existing inst

  AddedInstrns() : InstrnsBefore(), InstrnsAfter() { }
};

typedef hash_map<const MachineInstr *, AddedInstrns *> AddedInstrMapType;



//----------------------------------------------------------------------------
// class PhyRegAlloc:
// Main class the register allocator. Call allocateRegisters() to allocate
// registers for a Method.
//----------------------------------------------------------------------------


class PhyRegAlloc: public NonCopyable
{

  vector<RegClass *> RegClassList  ;    // vector of register classes
  const TargetMachine &TM;              // target machine
  const Method* Meth;                   // name of the method we work on
  MachineCodeForMethod& mcInfo;         // descriptor for method's native code
  MethodLiveVarInfo *const LVI;         // LV information for this method 
                                        // (already computed for BBs) 
  LiveRangeInfo LRI;                    // LR info  (will be computed)
  const MachineRegInfo &MRI;            // Machine Register information
  const unsigned NumOfRegClasses;       // recorded here for efficiency

  
  AddedInstrMapType AddedInstrMap;      // to store instrns added in this phase
  LoopDepthCalculator LoopDepthCalc;    // to calculate loop depths 
  ReservedColorListType ResColList;     // A set of reserved regs if desired.
                                        // currently not used



  //------- ------------------ private methods---------------------------------

  void addInterference(const Value *const Def, const LiveVarSet *const LVSet, 
		       const bool isCallInst);

  void addInterferencesForArgs();
  void createIGNodeListsAndIGs();
  void buildInterferenceGraphs();

  void setCallInterferences(const MachineInstr *MInst, 
			    const LiveVarSet *const LVSetAft );

  void move2DelayedInstr(const MachineInstr *OrigMI, 
			 const MachineInstr *DelayedMI );

  void markUnusableSugColors();
  void allocateStackSpace4SpilledLRs();

  void insertCode4SpilledLR     (const LiveRange *LR, 
                                 MachineInstr *MInst,
                                 const BasicBlock *BB,
                                 const unsigned OpNum);

  inline void constructLiveRanges() 
    { LRI.constructLiveRanges(); }      

  void colorIncomingArgs();
  void colorCallRetArgs();
  void updateMachineCode();

  void printLabel(const Value *const Val);
  void printMachineCode();

  friend class UltraSparcRegInfo;


  int getUsableUniRegAtMI(RegClass *RC,  const int RegType, 
			  const MachineInstr *MInst,
			  const LiveVarSet *LVSetBef, MachineInstr *MIBef, 
			  MachineInstr *MIAft );

  int getUnusedUniRegAtMI(RegClass *RC,  const MachineInstr *MInst, 
		       const LiveVarSet *LVSetBef);

  void setRelRegsUsedByThisInst(RegClass *RC, const MachineInstr *MInst );
  int getUniRegNotUsedByThisInst(RegClass *RC, const MachineInstr *MInst);

  void addInterf4PseudoInstr(const MachineInstr *MInst);


 public:

  PhyRegAlloc(Method *const M, const TargetMachine& TM, 
	      MethodLiveVarInfo *const Lvi);

  ~PhyRegAlloc(); 

  // main method called for allocating registers
  //
  void allocateRegisters();           

};


#endif

