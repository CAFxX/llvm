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


#include "SparcInternals.h"
#include "llvm/Target/Sparc.h"
#include "llvm/CodeGen/InstrScheduling.h"
#include "llvm/CodeGen/InstrSelection.h"
#include "llvm/CodeGen/MachineCodeForInstruction.h"
#include "llvm/CodeGen/MachineCodeForMethod.h"
#include "llvm/CodeGen/PhyRegAlloc.h"
#include "llvm/Method.h"
#include "llvm/PassManager.h"
#include <iostream>
using std::cerr;

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

class RegisterAllocation : public MethodPass {
  TargetMachine &Target;
public:
  inline RegisterAllocation(TargetMachine &T) : Target(T) {}
  bool runOnMethod(Method *M) {
    if (DEBUG_RA)
      cerr << "\n******************** Method "<< M->getName()
           << " ********************\n";
    
    MethodLiveVarInfo LVI(M );   // Analyze live varaibles
    LVI.analyze();
    
    PhyRegAlloc PRA(M, Target, &LVI); // allocate registers
    PRA.allocateRegisters();

    if (DEBUG_RA) cerr << "\nRegister allocation complete!\n";
    return false;
  }
};

static MachineInstr* minstrVec[MAX_INSTR_PER_VMINSTR];

//---------------------------------------------------------------------------
// class InsertPrologEpilogCode
//
// Insert SAVE/RESTORE instructions for the method
//
// Insert prolog code at the unique method entry point.
// Insert epilog code at each method exit point.
// InsertPrologEpilog invokes these only if the method is not compiled
// with the leaf method optimization.
//
//---------------------------------------------------------------------------

class InsertPrologEpilogCode : public MethodPass {
  TargetMachine &Target;
public:
  inline InsertPrologEpilogCode(TargetMachine &T) : Target(T) {}
  bool runOnMethod(Method *M) {
    MachineCodeForMethod &mcodeInfo = MachineCodeForMethod::get(M);
    if (!mcodeInfo.isCompiledAsLeafMethod()) {
      InsertPrologCode(M);
      InsertEpilogCode(M);
    }
    return false;
  }

  void InsertPrologCode(Method *M);
  void InsertEpilogCode(Method *M);
};

void InsertPrologEpilogCode::InsertPrologCode(Method* method)
{
  BasicBlock* entryBB = method->getEntryNode();
  unsigned N = GetInstructionsForProlog(entryBB, Target, minstrVec);
  assert(N <= MAX_INSTR_PER_VMINSTR);
  MachineCodeForBasicBlock& bbMvec = entryBB->getMachineInstrVec();
  bbMvec.insert(bbMvec.begin(), minstrVec, minstrVec+N);
}


void InsertPrologEpilogCode::InsertEpilogCode(Method* method)
{
  for (Method::iterator I=method->begin(), E=method->end(); I != E; ++I)
    if ((*I)->getTerminator()->getOpcode() == Instruction::Ret)
      {
        BasicBlock* exitBB = *I;
        unsigned N = GetInstructionsForEpilog(exitBB, Target, minstrVec);
        
        MachineCodeForBasicBlock& bbMvec = exitBB->getMachineInstrVec();
        MachineCodeForInstruction &termMvec =
          MachineCodeForInstruction::get(exitBB->getTerminator());
        
        // Remove the NOPs in the delay slots of the return instruction
        const MachineInstrInfo &mii = Target.getInstrInfo();
        unsigned numNOPs = 0;
        while (termMvec.back()->getOpCode() == NOP)
          {
            assert( termMvec.back() == bbMvec.back());
            termMvec.pop_back();
            bbMvec.pop_back();
            ++numNOPs;
          }
        assert(termMvec.back() == bbMvec.back());
        
        // Check that we found the right number of NOPs and have the right
        // number of instructions to replace them.
        unsigned ndelays = mii.getNumDelaySlots(termMvec.back()->getOpCode());
        assert(numNOPs == ndelays && "Missing NOPs in delay slots?");
        assert(N == ndelays && "Cannot use epilog code for delay slots?");
        
        // Append the epilog code to the end of the basic block.
        bbMvec.push_back(minstrVec[0]);
      }
}


/*---------------------------------------------------------------------------
Scheduling guidelines for SPARC IIi:

I-Cache alignment rules (pg 326)
-- Align a branch target instruction so that it's entire group is within
   the same cache line (may be 1-4 instructions).
** Don't let a branch that is predicted taken be the last instruction
   on an I-cache line: delay slot will need an entire line to be fetched
-- Make a FP instruction or a branch be the 4th instruction in a group.
   For branches, there are tradeoffs in reordering to make this happen
   (see pg. 327).
** Don't put a branch in a group that crosses a 32-byte boundary!
   An artificial branch is inserted after every 32 bytes, and having
   another branch will force the group to be broken into 2 groups. 

iTLB rules:
-- Don't let a loop span two memory pages, if possible

Branch prediction performance:
-- Don't make the branch in a delay slot the target of a branch
-- Try not to have 2 predicted branches within a group of 4 instructions
   (because each such group has a single branch target field).
-- Try to align branches in slots 0, 2, 4 or 6 of a cache line (to avoid
   the wrong prediction bits being used in some cases).

D-Cache timing constraints:
-- Signed int loads of less than 64 bits have 3 cycle latency, not 2
-- All other loads that hit in D-Cache have 2 cycle latency
-- All loads are returned IN ORDER, so a D-Cache miss will delay a later hit
-- Mis-aligned loads or stores cause a trap.  In particular, replace
   mis-aligned FP double precision l/s with 2 single-precision l/s.
-- Simulations of integer codes show increase in avg. group size of
   33% when code (including esp. non-faulting loads) is moved across
   one branch, and 50% across 2 branches.

E-Cache timing constraints:
-- Scheduling for E-cache (D-Cache misses) is effective (due to load buffering)

Store buffer timing constraints:
-- Stores can be executed in same cycle as instruction producing the value
-- Stores are buffered and have lower priority for E-cache until
   highwater mark is reached in the store buffer (5 stores)

Pipeline constraints:
-- Shifts can only use IEU0.
-- CC setting instructions can only use IEU1.
-- Several other instructions must only use IEU1:
   EDGE(?), ARRAY(?), CALL, JMPL, BPr, PST, and FCMP.
-- Two instructions cannot store to the same register file in a single cycle
   (single write port per file).

Issue and grouping constraints:
-- FP and branch instructions must use slot 4.
-- Shift instructions cannot be grouped with other IEU0-specific instructions.
-- CC setting instructions cannot be grouped with other IEU1-specific instrs.
-- Several instructions must be issued in a single-instruction group:
	MOVcc or MOVr, MULs/x and DIVs/x, SAVE/RESTORE, many others
-- A CALL or JMPL breaks a group, ie, is not combined with subsequent instrs.
-- 
-- 

Branch delay slot scheduling rules:
-- A CTI couple (two back-to-back CTI instructions in the dynamic stream)
   has a 9-instruction penalty: the entire pipeline is flushed when the
   second instruction reaches stage 9 (W-Writeback).
-- Avoid putting multicycle instructions, and instructions that may cause
   load misses, in the delay slot of an annulling branch.
-- Avoid putting WR, SAVE..., RESTORE and RETURN instructions in the
   delay slot of an annulling branch.

 *--------------------------------------------------------------------------- */

//---------------------------------------------------------------------------
// List of CPUResources for UltraSPARC IIi.
//---------------------------------------------------------------------------

static const CPUResource  AllIssueSlots(   "All Instr Slots", 4);
static const CPUResource  IntIssueSlots(   "Int Instr Slots", 3);
static const CPUResource  First3IssueSlots("Instr Slots 0-3", 3);
static const CPUResource  LSIssueSlots(    "Load-Store Instr Slot", 1);
static const CPUResource  CTIIssueSlots(   "Ctrl Transfer Instr Slot", 1);
static const CPUResource  FPAIssueSlots(   "Int Instr Slot 1", 1);
static const CPUResource  FPMIssueSlots(   "Int Instr Slot 1", 1);

// IEUN instructions can use either Alu and should use IAluN.
// IEU0 instructions must use Alu 1 and should use both IAluN and IAlu0. 
// IEU1 instructions must use Alu 2 and should use both IAluN and IAlu1. 
static const CPUResource  IAluN("Int ALU 1or2", 2);
static const CPUResource  IAlu0("Int ALU 1",    1);
static const CPUResource  IAlu1("Int ALU 2",    1);

static const CPUResource  LSAluC1("Load/Store Unit Addr Cycle", 1);
static const CPUResource  LSAluC2("Load/Store Unit Issue Cycle", 1);
static const CPUResource  LdReturn("Load Return Unit", 1);

static const CPUResource  FPMAluC1("FP Mul/Div Alu Cycle 1", 1);
static const CPUResource  FPMAluC2("FP Mul/Div Alu Cycle 2", 1);
static const CPUResource  FPMAluC3("FP Mul/Div Alu Cycle 3", 1);

static const CPUResource  FPAAluC1("FP Other Alu Cycle 1", 1);
static const CPUResource  FPAAluC2("FP Other Alu Cycle 2", 1);
static const CPUResource  FPAAluC3("FP Other Alu Cycle 3", 1);

static const CPUResource  IRegReadPorts("Int Reg ReadPorts", INT_MAX); // CHECK
static const CPUResource  IRegWritePorts("Int Reg WritePorts", 2);     // CHECK
static const CPUResource  FPRegReadPorts("FP Reg Read Ports", INT_MAX);// CHECK
static const CPUResource  FPRegWritePorts("FP Reg Write Ports", 1);    // CHECK

static const CPUResource  CTIDelayCycle( "CTI  delay cycle", 1);
static const CPUResource  FCMPDelayCycle("FCMP delay cycle", 1);



//---------------------------------------------------------------------------
// const InstrClassRUsage SparcRUsageDesc[]
// 
// Purpose:
//   Resource usage information for instruction in each scheduling class.
//   The InstrRUsage Objects for individual classes are specified first.
//   Note that fetch and decode are decoupled from the execution pipelines
//   via an instr buffer, so they are not included in the cycles below.
//---------------------------------------------------------------------------

static const InstrClassRUsage NoneClassRUsage = {
  SPARC_NONE,
  /*totCycles*/ 7,
  
  /* maxIssueNum */ 4,
  /* isSingleIssue */ false,
  /* breaksGroup */ false,
  /* numBubbles */ 0,
  
  /*numSlots*/ 4,
  /* feasibleSlots[] */ { 0, 1, 2, 3 },
  
  /*numEntries*/ 0,
  /* V[] */ {
    /*Cycle G */
    /*Ccle E */
    /*Cycle C */
    /*Cycle N1*/
    /*Cycle N1*/
    /*Cycle N1*/
    /*Cycle W */
  }
};

static const InstrClassRUsage IEUNClassRUsage = {
  SPARC_IEUN,
  /*totCycles*/ 7,
  
  /* maxIssueNum */ 3,
  /* isSingleIssue */ false,
  /* breaksGroup */ false,
  /* numBubbles */ 0,
  
  /*numSlots*/ 3,
  /* feasibleSlots[] */ { 0, 1, 2 },
  
  /*numEntries*/ 4,
  /* V[] */ {
    /*Cycle G */ { AllIssueSlots.rid, 0, 1 },
                 { IntIssueSlots.rid, 0, 1 },
    /*Cycle E */ { IAluN.rid, 1, 1 },
    /*Cycle C */
    /*Cycle N1*/
    /*Cycle N1*/
    /*Cycle N1*/
    /*Cycle W */ { IRegWritePorts.rid, 6, 1  }
  }
};

static const InstrClassRUsage IEU0ClassRUsage = {
  SPARC_IEU0,
  /*totCycles*/ 7,
  
  /* maxIssueNum */ 1,
  /* isSingleIssue */ false,
  /* breaksGroup */ false,
  /* numBubbles */ 0,
  
  /*numSlots*/ 3,
  /* feasibleSlots[] */ { 0, 1, 2 },
  
  /*numEntries*/ 5,
  /* V[] */ {
    /*Cycle G */ { AllIssueSlots.rid, 0, 1 },
                 { IntIssueSlots.rid, 0, 1 },
    /*Cycle E */ { IAluN.rid, 1, 1 },
                 { IAlu0.rid, 1, 1 },
    /*Cycle C */
    /*Cycle N1*/
    /*Cycle N1*/
    /*Cycle N1*/
    /*Cycle W */ { IRegWritePorts.rid, 6, 1 }
  }
};

static const InstrClassRUsage IEU1ClassRUsage = {
  SPARC_IEU1,
  /*totCycles*/ 7,
  
  /* maxIssueNum */ 1,
  /* isSingleIssue */ false,
  /* breaksGroup */ false,
  /* numBubbles */ 0,
  
  /*numSlots*/ 3,
  /* feasibleSlots[] */ { 0, 1, 2 },
  
  /*numEntries*/ 5,
  /* V[] */ {
    /*Cycle G */ { AllIssueSlots.rid, 0, 1 },
               { IntIssueSlots.rid, 0, 1 },
    /*Cycle E */ { IAluN.rid, 1, 1 },
               { IAlu1.rid, 1, 1 },
    /*Cycle C */
    /*Cycle N1*/
    /*Cycle N1*/
    /*Cycle N1*/
    /*Cycle W */ { IRegWritePorts.rid, 6, 1 }
  }
};

static const InstrClassRUsage FPMClassRUsage = {
  SPARC_FPM,
  /*totCycles*/ 7,
  
  /* maxIssueNum */ 1,
  /* isSingleIssue */ false,
  /* breaksGroup */ false,
  /* numBubbles */ 0,
  
  /*numSlots*/ 4,
  /* feasibleSlots[] */ { 0, 1, 2, 3 },
  
  /*numEntries*/ 7,
  /* V[] */ {
    /*Cycle G */ { AllIssueSlots.rid,   0, 1 },
                 { FPMIssueSlots.rid,   0, 1 },
    /*Cycle E */ { FPRegReadPorts.rid,  1, 1 },
    /*Cycle C */ { FPMAluC1.rid,        2, 1 },
    /*Cycle N1*/ { FPMAluC2.rid,        3, 1 },
    /*Cycle N1*/ { FPMAluC3.rid,        4, 1 },
    /*Cycle N1*/
    /*Cycle W */ { FPRegWritePorts.rid, 6, 1 }
  }
};

static const InstrClassRUsage FPAClassRUsage = {
  SPARC_FPA,
  /*totCycles*/ 7,
  
  /* maxIssueNum */ 1,
  /* isSingleIssue */ false,
  /* breaksGroup */ false,
  /* numBubbles */ 0,
  
  /*numSlots*/ 4,
  /* feasibleSlots[] */ { 0, 1, 2, 3 },
  
  /*numEntries*/ 7,
  /* V[] */ {
    /*Cycle G */ { AllIssueSlots.rid,   0, 1 },
                 { FPAIssueSlots.rid,   0, 1 },
    /*Cycle E */ { FPRegReadPorts.rid,  1, 1 },
    /*Cycle C */ { FPAAluC1.rid,        2, 1 },
    /*Cycle N1*/ { FPAAluC2.rid,        3, 1 },
    /*Cycle N1*/ { FPAAluC3.rid,        4, 1 },
    /*Cycle N1*/
    /*Cycle W */ { FPRegWritePorts.rid, 6, 1 }
  }
};

static const InstrClassRUsage LDClassRUsage = {
  SPARC_LD,
  /*totCycles*/ 7,
  
  /* maxIssueNum */ 1,
  /* isSingleIssue */ false,
  /* breaksGroup */ false,
  /* numBubbles */ 0,
  
  /*numSlots*/ 3,
  /* feasibleSlots[] */ { 0, 1, 2, },
  
  /*numEntries*/ 6,
  /* V[] */ {
    /*Cycle G */ { AllIssueSlots.rid,    0, 1 },
                 { First3IssueSlots.rid, 0, 1 },
                 { LSIssueSlots.rid,     0, 1 },
    /*Cycle E */ { LSAluC1.rid,          1, 1 },
    /*Cycle C */ { LSAluC2.rid,          2, 1 },
                 { LdReturn.rid,         2, 1 },
    /*Cycle N1*/
    /*Cycle N1*/
    /*Cycle N1*/
    /*Cycle W */ { IRegWritePorts.rid,   6, 1 }
  }
};

static const InstrClassRUsage STClassRUsage = {
  SPARC_ST,
  /*totCycles*/ 7,
  
  /* maxIssueNum */ 1,
  /* isSingleIssue */ false,
  /* breaksGroup */ false,
  /* numBubbles */ 0,
  
  /*numSlots*/ 3,
  /* feasibleSlots[] */ { 0, 1, 2 },
  
  /*numEntries*/ 4,
  /* V[] */ {
    /*Cycle G */ { AllIssueSlots.rid,    0, 1 },
                 { First3IssueSlots.rid, 0, 1 },
                 { LSIssueSlots.rid,     0, 1 },
    /*Cycle E */ { LSAluC1.rid,          1, 1 },
    /*Cycle C */ { LSAluC2.rid,          2, 1 }
    /*Cycle N1*/
    /*Cycle N1*/
    /*Cycle N1*/
    /*Cycle W */
  }
};

static const InstrClassRUsage CTIClassRUsage = {
  SPARC_CTI,
  /*totCycles*/ 7,
  
  /* maxIssueNum */ 1,
  /* isSingleIssue */ false,
  /* breaksGroup */ false,
  /* numBubbles */ 0,
  
  /*numSlots*/ 4,
  /* feasibleSlots[] */ { 0, 1, 2, 3 },
  
  /*numEntries*/ 4,
  /* V[] */ {
    /*Cycle G */ { AllIssueSlots.rid,    0, 1 },
		 { CTIIssueSlots.rid,    0, 1 },
    /*Cycle E */ { IAlu0.rid,            1, 1 },
    /*Cycles E-C */ { CTIDelayCycle.rid, 1, 2 }
    /*Cycle C */             
    /*Cycle N1*/
    /*Cycle N1*/
    /*Cycle N1*/
    /*Cycle W */
  }
};

static const InstrClassRUsage SingleClassRUsage = {
  SPARC_SINGLE,
  /*totCycles*/ 7,
  
  /* maxIssueNum */ 1,
  /* isSingleIssue */ true,
  /* breaksGroup */ false,
  /* numBubbles */ 0,
  
  /*numSlots*/ 1,
  /* feasibleSlots[] */ { 0 },
  
  /*numEntries*/ 5,
  /* V[] */ {
    /*Cycle G */ { AllIssueSlots.rid,    0, 1 },
                 { AllIssueSlots.rid,    0, 1 },
                 { AllIssueSlots.rid,    0, 1 },
                 { AllIssueSlots.rid,    0, 1 },
    /*Cycle E */ { IAlu0.rid,            1, 1 }
    /*Cycle C */
    /*Cycle N1*/
    /*Cycle N1*/
    /*Cycle N1*/
    /*Cycle W */
  }
};


static const InstrClassRUsage SparcRUsageDesc[] = {
  NoneClassRUsage,
  IEUNClassRUsage,
  IEU0ClassRUsage,
  IEU1ClassRUsage,
  FPMClassRUsage,
  FPAClassRUsage,
  CTIClassRUsage,
  LDClassRUsage,
  STClassRUsage,
  SingleClassRUsage
};



//---------------------------------------------------------------------------
// const InstrIssueDelta  SparcInstrIssueDeltas[]
// 
// Purpose:
//   Changes to issue restrictions information in InstrClassRUsage for
//   instructions that differ from other instructions in their class.
//---------------------------------------------------------------------------

static const InstrIssueDelta  SparcInstrIssueDeltas[] = {

  // opCode,  isSingleIssue,  breaksGroup,  numBubbles

				// Special cases for single-issue only
				// Other single issue cases are below.
//{ LDDA,	true,	true,	0 },
//{ STDA,	true,	true,	0 },
//{ LDDF,	true,	true,	0 },
//{ LDDFA,	true,	true,	0 },
  { ADDC,	true,	true,	0 },
  { ADDCcc,	true,	true,	0 },
  { SUBC,	true,	true,	0 },
  { SUBCcc,	true,	true,	0 },
//{ LDSTUB,	true,	true,	0 },
//{ SWAP,	true,	true,	0 },
//{ SWAPA,	true,	true,	0 },
//{ CAS,	true,	true,	0 },
//{ CASA,	true,	true,	0 },
//{ CASX,	true,	true,	0 },
//{ CASXA,	true,	true,	0 },
//{ LDFSR,	true,	true,	0 },
//{ LDFSRA,	true,	true,	0 },
//{ LDXFSR,	true,	true,	0 },
//{ LDXFSRA,	true,	true,	0 },
//{ STFSR,	true,	true,	0 },
//{ STFSRA,	true,	true,	0 },
//{ STXFSR,	true,	true,	0 },
//{ STXFSRA,	true,	true,	0 },
//{ SAVED,	true,	true,	0 },
//{ RESTORED,	true,	true,	0 },
//{ FLUSH,	true,	true,	9 },
//{ FLUSHW,	true,	true,	9 },
//{ ALIGNADDR,	true,	true,	0 },
  { RETURN,	true,	true,	0 },
//{ DONE,	true,	true,	0 },
//{ RETRY,	true,	true,	0 },
//{ TCC,	true,	true,	0 },
//{ SHUTDOWN,	true,	true,	0 },
  
				// Special cases for breaking group *before*
				// CURRENTLY NOT SUPPORTED!
  { CALL,	false,	false,	0 },
  { JMPLCALL,	false,	false,	0 },
  { JMPLRET,	false,	false,	0 },
  
				// Special cases for breaking the group *after*
  { MULX,	true,	true,	(4+34)/2 },
  { FDIVS,	false,	true,	0 },
  { FDIVD,	false,	true,	0 },
  { FDIVQ,	false,	true,	0 },
  { FSQRTS,	false,	true,	0 },
  { FSQRTD,	false,	true,	0 },
  { FSQRTQ,	false,	true,	0 },
//{ FCMP{LE,GT,NE,EQ}, false, true, 0 },
  
				// Instructions that introduce bubbles
//{ MULScc,	true,	true,	2 },
//{ SMULcc,	true,	true,	(4+18)/2 },
//{ UMULcc,	true,	true,	(4+19)/2 },
  { SDIVX,	true,	true,	68 },
  { UDIVX,	true,	true,	68 },
//{ SDIVcc,	true,	true,	36 },
//{ UDIVcc,	true,	true,	37 },
  { WRCCR,	true,	true,	4 },
//{ WRPR,	true,	true,	4 },
//{ RDCCR,	true,	true,	0 }, // no bubbles after, but see below
//{ RDPR,	true,	true,	0 },
};




//---------------------------------------------------------------------------
// const InstrRUsageDelta SparcInstrUsageDeltas[]
// 
// Purpose:
//   Changes to resource usage information in InstrClassRUsage for
//   instructions that differ from other instructions in their class.
//---------------------------------------------------------------------------

static const InstrRUsageDelta SparcInstrUsageDeltas[] = {

  // MachineOpCode, Resource, Start cycle, Num cycles

  // 
  // JMPL counts as a load/store instruction for issue!
  //
  { JMPLCALL, LSIssueSlots.rid,  0,  1 },
  { JMPLRET,  LSIssueSlots.rid,  0,  1 },
  
  // 
  // Many instructions cannot issue for the next 2 cycles after an FCMP
  // We model that with a fake resource FCMPDelayCycle.
  // 
  { FCMPS,    FCMPDelayCycle.rid, 1, 3 },
  { FCMPD,    FCMPDelayCycle.rid, 1, 3 },
  { FCMPQ,    FCMPDelayCycle.rid, 1, 3 },
  
  { MULX,     FCMPDelayCycle.rid, 1, 1 },
  { SDIVX,    FCMPDelayCycle.rid, 1, 1 },
  { UDIVX,    FCMPDelayCycle.rid, 1, 1 },
//{ SMULcc,   FCMPDelayCycle.rid, 1, 1 },
//{ UMULcc,   FCMPDelayCycle.rid, 1, 1 },
//{ SDIVcc,   FCMPDelayCycle.rid, 1, 1 },
//{ UDIVcc,   FCMPDelayCycle.rid, 1, 1 },
  { STD,      FCMPDelayCycle.rid, 1, 1 },
  { FMOVRSZ,  FCMPDelayCycle.rid, 1, 1 },
  { FMOVRSLEZ,FCMPDelayCycle.rid, 1, 1 },
  { FMOVRSLZ, FCMPDelayCycle.rid, 1, 1 },
  { FMOVRSNZ, FCMPDelayCycle.rid, 1, 1 },
  { FMOVRSGZ, FCMPDelayCycle.rid, 1, 1 },
  { FMOVRSGEZ,FCMPDelayCycle.rid, 1, 1 },
  
  // 
  // Some instructions are stalled in the GROUP stage if a CTI is in
  // the E or C stage.  We model that with a fake resource CTIDelayCycle.
  // 
  { LDD,      CTIDelayCycle.rid,  1, 1 },
//{ LDDA,     CTIDelayCycle.rid,  1, 1 },
//{ LDDSTUB,  CTIDelayCycle.rid,  1, 1 },
//{ LDDSTUBA, CTIDelayCycle.rid,  1, 1 },
//{ SWAP,     CTIDelayCycle.rid,  1, 1 },
//{ SWAPA,    CTIDelayCycle.rid,  1, 1 },
//{ CAS,      CTIDelayCycle.rid,  1, 1 },
//{ CASA,     CTIDelayCycle.rid,  1, 1 },
//{ CASX,     CTIDelayCycle.rid,  1, 1 },
//{ CASXA,    CTIDelayCycle.rid,  1, 1 },
  
  //
  // Signed int loads of less than dword size return data in cycle N1 (not C)
  // and put all loads in consecutive cycles into delayed load return mode.
  //
  { LDSB,    LdReturn.rid,  2, -1 },
  { LDSB,    LdReturn.rid,  3,  1 },
  
  { LDSH,    LdReturn.rid,  2, -1 },
  { LDSH,    LdReturn.rid,  3,  1 },
  
  { LDSW,    LdReturn.rid,  2, -1 },
  { LDSW,    LdReturn.rid,  3,  1 },

  //
  // RDPR from certain registers and RD from any register are not dispatchable
  // until four clocks after they reach the head of the instr. buffer.
  // Together with their single-issue requirement, this means all four issue
  // slots are effectively blocked for those cycles, plus the issue cycle.
  // This does not increase the latency of the instruction itself.
  // 
  { RDCCR,   AllIssueSlots.rid,     0,  5 },
  { RDCCR,   AllIssueSlots.rid,     0,  5 },
  { RDCCR,   AllIssueSlots.rid,     0,  5 },
  { RDCCR,   AllIssueSlots.rid,     0,  5 },

#undef EXPLICIT_BUBBLES_NEEDED
#ifdef EXPLICIT_BUBBLES_NEEDED
  // 
  // MULScc inserts one bubble.
  // This means it breaks the current group (captured in UltraSparcSchedInfo)
  // *and occupies all issue slots for the next cycle
  // 
//{ MULScc,  AllIssueSlots.rid, 2, 2-1 },
//{ MULScc,  AllIssueSlots.rid, 2, 2-1 },
//{ MULScc,  AllIssueSlots.rid, 2, 2-1 },
//{ MULScc,  AllIssueSlots.rid,  2, 2-1 },
  
  // 
  // SMULcc inserts between 4 and 18 bubbles, depending on #leading 0s in rs1.
  // We just model this with a simple average.
  // 
//{ SMULcc,  AllIssueSlots.rid, 2, ((4+18)/2)-1 },
//{ SMULcc,  AllIssueSlots.rid, 2, ((4+18)/2)-1 },
//{ SMULcc,  AllIssueSlots.rid, 2, ((4+18)/2)-1 },
//{ SMULcc,  AllIssueSlots.rid,  2, ((4+18)/2)-1 },
  
  // SMULcc inserts between 4 and 19 bubbles, depending on #leading 0s in rs1.
//{ UMULcc,  AllIssueSlots.rid, 2, ((4+19)/2)-1 },
//{ UMULcc,  AllIssueSlots.rid, 2, ((4+19)/2)-1 },
//{ UMULcc,  AllIssueSlots.rid, 2, ((4+19)/2)-1 },
//{ UMULcc,  AllIssueSlots.rid,  2, ((4+19)/2)-1 },
  
  // 
  // MULX inserts between 4 and 34 bubbles, depending on #leading 0s in rs1.
  // 
  { MULX,    AllIssueSlots.rid, 2, ((4+34)/2)-1 },
  { MULX,    AllIssueSlots.rid, 2, ((4+34)/2)-1 },
  { MULX,    AllIssueSlots.rid, 2, ((4+34)/2)-1 },
  { MULX,    AllIssueSlots.rid,  2, ((4+34)/2)-1 },
  
  // 
  // SDIVcc inserts 36 bubbles.
  // 
//{ SDIVcc,  AllIssueSlots.rid, 2, 36-1 },
//{ SDIVcc,  AllIssueSlots.rid, 2, 36-1 },
//{ SDIVcc,  AllIssueSlots.rid, 2, 36-1 },
//{ SDIVcc,  AllIssueSlots.rid,  2, 36-1 },
  
  // UDIVcc inserts 37 bubbles.
//{ UDIVcc,  AllIssueSlots.rid, 2, 37-1 },
//{ UDIVcc,  AllIssueSlots.rid, 2, 37-1 },
//{ UDIVcc,  AllIssueSlots.rid, 2, 37-1 },
//{ UDIVcc,  AllIssueSlots.rid,  2, 37-1 },
  
  // 
  // SDIVX inserts 68 bubbles.
  // 
  { SDIVX,   AllIssueSlots.rid, 2, 68-1 },
  { SDIVX,   AllIssueSlots.rid, 2, 68-1 },
  { SDIVX,   AllIssueSlots.rid, 2, 68-1 },
  { SDIVX,   AllIssueSlots.rid,  2, 68-1 },
  
  // 
  // UDIVX inserts 68 bubbles.
  // 
  { UDIVX,   AllIssueSlots.rid, 2, 68-1 },
  { UDIVX,   AllIssueSlots.rid, 2, 68-1 },
  { UDIVX,   AllIssueSlots.rid, 2, 68-1 },
  { UDIVX,   AllIssueSlots.rid,  2, 68-1 },
  
  // 
  // WR inserts 4 bubbles.
  // 
//{ WR,     AllIssueSlots.rid, 2, 68-1 },
//{ WR,     AllIssueSlots.rid, 2, 68-1 },
//{ WR,     AllIssueSlots.rid, 2, 68-1 },
//{ WR,     AllIssueSlots.rid,  2, 68-1 },
  
  // 
  // WRPR inserts 4 bubbles.
  // 
//{ WRPR,   AllIssueSlots.rid, 2, 68-1 },
//{ WRPR,   AllIssueSlots.rid, 2, 68-1 },
//{ WRPR,   AllIssueSlots.rid, 2, 68-1 },
//{ WRPR,   AllIssueSlots.rid,  2, 68-1 },
  
  // 
  // DONE inserts 9 bubbles.
  // 
//{ DONE,   AllIssueSlots.rid, 2, 9-1 },
//{ DONE,   AllIssueSlots.rid, 2, 9-1 },
//{ DONE,   AllIssueSlots.rid, 2, 9-1 },
//{ DONE,   AllIssueSlots.rid, 2, 9-1 },
  
  // 
  // RETRY inserts 9 bubbles.
  // 
//{ RETRY,   AllIssueSlots.rid, 2, 9-1 },
//{ RETRY,   AllIssueSlots.rid, 2, 9-1 },
//{ RETRY,   AllIssueSlots.rid, 2, 9-1 },
//{ RETRY,   AllIssueSlots.rid,  2, 9-1 },

#endif  /*EXPLICIT_BUBBLES_NEEDED */
};

// Additional delays to be captured in code:
// 1. RDPR from several state registers (page 349)
// 2. RD   from *any* register (page 349)
// 3. Writes to TICK, PSTATE, TL registers and FLUSH{W} instr (page 349)
// 4. Integer store can be in same group as instr producing value to store.
// 5. BICC and BPICC can be in the same group as instr producing CC (pg 350)
// 6. FMOVr cannot be in the same or next group as an IEU instr (pg 351).
// 7. The second instr. of a CTI group inserts 9 bubbles (pg 351)
// 8. WR{PR}, SVAE, SAVED, RESTORE, RESTORED, RETURN, RETRY, and DONE that
//    follow an annulling branch cannot be issued in the same group or in
//    the 3 groups following the branch.
// 9. A predicted annulled load does not stall dependent instructions.
//    Other annulled delay slot instructions *do* stall dependents, so
//    nothing special needs to be done for them during scheduling.
//10. Do not put a load use that may be annulled in the same group as the
//    branch.  The group will stall until the load returns.
//11. Single-prec. FP loads lock 2 registers, for dependency checking.
//
// 
// Additional delays we cannot or will not capture:
// 1. If DCTI is last word of cache line, it is delayed until next line can be
//    fetched.  Also, other DCTI alignment-related delays (pg 352)
// 2. Load-after-store is delayed by 7 extra cycles if load hits in D-Cache.
//    Also, several other store-load and load-store conflicts (pg 358)
// 3. MEMBAR, LD{X}FSR, LDD{A} and a bunch of other load stalls (pg 358)
// 4. There can be at most 8 outstanding buffered store instructions
//     (including some others like MEMBAR, LDSTUB, CAS{AX}, and FLUSH)



//---------------------------------------------------------------------------
// class UltraSparcSchedInfo 
// 
// Purpose:
//   Scheduling information for the UltraSPARC.
//   Primarily just initializes machine-dependent parameters in
//   class MachineSchedInfo.
//---------------------------------------------------------------------------

/*ctor*/
UltraSparcSchedInfo::UltraSparcSchedInfo(const TargetMachine& tgt)
  : MachineSchedInfo(tgt,
                     (unsigned int) SPARC_NUM_SCHED_CLASSES,
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
// class UltraSparcFrameInfo 
// 
// Purpose:
//   Interface to stack frame layout info for the UltraSPARC.
//   Starting offsets for each area of the stack frame are aligned at
//   a multiple of getStackFrameSizeAlignment().
//---------------------------------------------------------------------------

int
UltraSparcFrameInfo::getFirstAutomaticVarOffset(MachineCodeForMethod& ,
                                                bool& pos) const
{
  pos = false;                          // static stack area grows downwards
  return StaticAreaOffsetFromFP;
}

int
UltraSparcFrameInfo::getRegSpillAreaOffset(MachineCodeForMethod& mcInfo,
                                           bool& pos) const
{
  pos = false;                          // static stack area grows downwards
  unsigned int autoVarsSize = mcInfo.getAutomaticVarsSize();
  if (int mod = autoVarsSize % getStackFrameSizeAlignment())  
    autoVarsSize += (getStackFrameSizeAlignment() - mod);
  return StaticAreaOffsetFromFP - autoVarsSize; 
}

int
UltraSparcFrameInfo::getTmpAreaOffset(MachineCodeForMethod& mcInfo,
                                      bool& pos) const
{
  pos = false;                          // static stack area grows downwards
  unsigned int autoVarsSize = mcInfo.getAutomaticVarsSize();
  unsigned int spillAreaSize = mcInfo.getRegSpillsSize();
  int offset = autoVarsSize + spillAreaSize;
  if (int mod = offset % getStackFrameSizeAlignment())  
    offset += (getStackFrameSizeAlignment() - mod);
  return StaticAreaOffsetFromFP - offset;
}

int
UltraSparcFrameInfo::getDynamicAreaOffset(MachineCodeForMethod& mcInfo,
                                          bool& pos) const
{
  // dynamic stack area grows downwards starting at top of opt-args area
  unsigned int optArgsSize = mcInfo.getMaxOptionalArgsSize();
  int offset = optArgsSize + FirstOptionalOutgoingArgOffsetFromSP;
  assert(offset % getStackFrameSizeAlignment() == 0);
  return offset;
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
    instrInfo(*this),
    schedInfo(*this),
    regInfo(*this),
    frameInfo(*this),
    cacheInfo(*this)
{
  optSizeForSubWordData = 4;
  minMemOpWordSize = 8; 
  maxAtomicMemOpWordSize = 8;
}



//===---------------------------------------------------------------------===//
// GenerateCodeForTarget Pass
// 
// Native code generation for a specified target.
//===---------------------------------------------------------------------===//

class ConstructMachineCodeForMethod : public MethodPass {
  TargetMachine &Target;
public:
  inline ConstructMachineCodeForMethod(TargetMachine &T) : Target(T) {}
  bool runOnMethod(Method *M) {
    MachineCodeForMethod::construct(M, Target);
    return false;
  }
};

class InstructionSelection : public MethodPass {
  TargetMachine &Target;
public:
  inline InstructionSelection(TargetMachine &T) : Target(T) {}
  bool runOnMethod(Method *M) {
    if (SelectInstructionsForMethod(M, Target))
      cerr << "Instr selection failed for method " << M->getName() << "\n";
    return false;
  }
};

class InstructionScheduling : public MethodPass {
  TargetMachine &Target;
public:
  inline InstructionScheduling(TargetMachine &T) : Target(T) {}
  bool runOnMethod(Method *M) {
    if (ScheduleInstructionsWithSSA(M, Target))
      cerr << "Instr scheduling failed for method " << M->getName() << "\n\n";
    return false;
  }
};

struct FreeMachineCodeForMethod : public MethodPass {
  static void freeMachineCode(Instruction *I) {
    MachineCodeForInstruction::destroy(I);
  }

  bool runOnMethod(Method *M) {
    for_each(M->inst_begin(), M->inst_end(), freeMachineCode);
    // Don't destruct MachineCodeForMethod - The global printer needs it
    //MachineCodeForMethod::destruct(M);
    return false;
  }
};


void UltraSparc::addPassesToEmitAssembly(PassManager &PM, std::ostream &Out) {
  // Construct and initialize the MachineCodeForMethod object for this method.
  PM.add(new ConstructMachineCodeForMethod(*this));

  PM.add(new InstructionSelection(*this));

  //PM.add(new InstructionScheduling(*this));

  PM.add(new RegisterAllocation(*this));
  
  //PM.add(new OptimizeLeafProcedures());
  //PM.add(new DeleteFallThroughBranches());
  //PM.add(new RemoveChainedBranches());    // should be folded with previous
  //PM.add(new RemoveRedundantOps());       // operations with %g0, NOP, etc.
  
  PM.add(new InsertPrologEpilogCode(*this));
  
  // Output assembly language to the .s file.  Assembly emission is split into
  // two parts: Method output and Global value output.  This is because method
  // output is pipelined with all of the rest of code generation stuff,
  // allowing machine code representations for methods to be free'd after the
  // method has been emitted.
  //
  PM.add(getMethodAsmPrinterPass(PM, Out));
  PM.add(new FreeMachineCodeForMethod());  // Free stuff no longer needed

  // Emit Module level assembly after all of the methods have been processed.
  PM.add(getModuleAsmPrinterPass(PM, Out));
}
