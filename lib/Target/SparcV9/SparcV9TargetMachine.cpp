// $Id$
//***************************************************************************
// File:
//	Sparc.cpp
// 
// Purpose:
//	
// History:
//	7/15/01	 -  Vikram Adve  -  Created
//**************************************************************************/

#include "llvm/Target/Sparc.h"
#include "SparcInternals.h"
#include "llvm/Method.h"
#include "llvm/CodeGen/InstrScheduling.h"
#include "llvm/CodeGen/InstrSelection.h"

#include "llvm/Analysis/LiveVar/MethodLiveVarInfo.h"
#include "llvm/CodeGen/PhyRegAlloc.h"

// Build the MachineInstruction Description Array...
const MachineInstrDescriptor SparcMachineInstrDesc[] = {
#define I(ENUM, OPCODESTRING, NUMOPERANDS, RESULTPOS, MAXIMM, IMMSE, \
          NUMDELAYSLOTS, LATENCY, SCHEDCLASS, INSTFLAGS)             \
  { OPCODESTRING, NUMOPERANDS, RESULTPOS, MAXIMM, IMMSE,             \
          NUMDELAYSLOTS, LATENCY, SCHEDCLASS, INSTFLAGS },
#include "SparcInstr.def"
};

//----------------------------------------------------------------------------
// allocateSparcTargetMachine - Allocate and return a subclass of TargetMachine
// that implements the Sparc backend. (the llvm/CodeGen/Sparc.h interface)
//----------------------------------------------------------------------------
//

TargetMachine *allocateSparcTargetMachine() { return new UltraSparc(); }


//----------------------------------------------------------------------------
// Entry point for register allocation for a module
//----------------------------------------------------------------------------

void AllocateRegisters(Method *M, TargetMachine &TM)
{
 
  if ( (M)->isExternal() )     // don't process prototypes
    return;
    
  if( DEBUG_RA ) {
    cerr << endl << "******************** Method "<< (M)->getName();
    cerr <<        " ********************" <<endl;
  }
    
  MethodLiveVarInfo LVI(M );   // Analyze live varaibles
  LVI.analyze();
  
    
  PhyRegAlloc PRA(M, TM , &LVI); // allocate registers
  PRA.allocateRegisters();
    

  if( DEBUG_RA )  cerr << endl << "Register allocation complete!" << endl;

}



//---------------------------------------------------------------------------
// class UltraSparcSchedInfo 
// 
// Purpose:
//   Scheduling information for the UltraSPARC.
//   Primarily just initializes machine-dependent parameters in
//   class MachineSchedInfo.
//---------------------------------------------------------------------------

/*ctor*/
UltraSparcSchedInfo::UltraSparcSchedInfo(const MachineInstrInfo* mii)
  : MachineSchedInfo((unsigned int) SPARC_NUM_SCHED_CLASSES,
		     mii,
		     SparcRUsageDesc,
		     SparcInstrUsageDeltas,
		     SparcInstrIssueDeltas,
		     sizeof(SparcInstrUsageDeltas)/sizeof(InstrRUsageDelta),
		     sizeof(SparcInstrIssueDeltas)/sizeof(InstrIssueDelta))
{
  maxNumIssueTotal = 4;
  longestIssueConflict = 0;		// computed from issuesGaps[]
  
  branchMispredictPenalty = 4;		// 4 for SPARC IIi
  branchTargetUnknownPenalty = 2;	// 2 for SPARC IIi
  l1DCacheMissPenalty = 8;		// 7 or 9 for SPARC IIi
  l1ICacheMissPenalty = 8;		// ? for SPARC IIi
  
  inOrderLoads = true;			// true for SPARC IIi
  inOrderIssue = true;			// true for SPARC IIi
  inOrderExec  = false;			// false for most architectures
  inOrderRetire= true;			// true for most architectures
  
  // must be called after above parameters are initialized.
  this->initializeResources();
}

void
UltraSparcSchedInfo::initializeResources()
{
  // Compute MachineSchedInfo::instrRUsages and MachineSchedInfo::issueGaps
  MachineSchedInfo::initializeResources();
  
  // Machine-dependent fixups go here.  None for now.
}




//---------------------------------------------------------------------------
// class UltraSparcMachine 
// 
// Purpose:
//   Primary interface to machine description for the UltraSPARC.
//   Primarily just initializes machine-dependent parameters in
//   class TargetMachine, and creates machine-dependent subclasses
//   for classes such as MachineInstrInfo. 
// 
//---------------------------------------------------------------------------

UltraSparc::UltraSparc()
  : TargetMachine("UltraSparc-Native"),
    instrInfo(),
    schedInfo(&instrInfo),
    regInfo( this )
{
  optSizeForSubWordData = 4;
  minMemOpWordSize = 8; 
  maxAtomicMemOpWordSize = 8;
}





bool UltraSparc::compileMethod(Method *M) {

  if (SelectInstructionsForMethod(M, *this))
    {
      cerr << "Instruction selection failed for method " << M->getName()
	   << "\n\n";
      return true;
    }
  
  if (ScheduleInstructionsWithSSA(M, *this))
    {
      cerr << "Instruction scheduling before allocation failed for method "
	   << M->getName() << "\n\n";
      return true;
    }
  
  AllocateRegisters(M, *this);    // allocate registers


  return false;
}



