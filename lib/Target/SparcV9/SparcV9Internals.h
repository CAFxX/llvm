//===-- SparcInternals.h ----------------------------------------*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
// 
// This file defines stuff that is to be private to the Sparc backend, but is
// shared among different portions of the backend.
//
//===----------------------------------------------------------------------===//

#ifndef SPARC_INTERNALS_H
#define SPARC_INTERNALS_H

#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetSchedInfo.h"
#include "llvm/Target/TargetFrameInfo.h"
#include "llvm/Target/TargetCacheInfo.h"
#include "llvm/Target/TargetRegInfo.h"
#include "llvm/Type.h"
#include "SparcRegClassInfo.h"
#include "Config/sys/types.h"

namespace llvm {

class LiveRange;
class SparcTargetMachine;
class Pass;

enum SparcInstrSchedClass {
  SPARC_NONE,		/* Instructions with no scheduling restrictions */
  SPARC_IEUN,		/* Integer class that can use IEU0 or IEU1 */
  SPARC_IEU0,		/* Integer class IEU0 */
  SPARC_IEU1,		/* Integer class IEU1 */
  SPARC_FPM,		/* FP Multiply or Divide instructions */
  SPARC_FPA,		/* All other FP instructions */	
  SPARC_CTI,		/* Control-transfer instructions */
  SPARC_LD,		/* Load instructions */
  SPARC_ST,		/* Store instructions */
  SPARC_SINGLE,		/* Instructions that must issue by themselves */
  
  SPARC_INV,		/* This should stay at the end for the next value */
  SPARC_NUM_SCHED_CLASSES = SPARC_INV
};


//---------------------------------------------------------------------------
// enum SparcMachineOpCode. 
// const TargetInstrDescriptor SparcMachineInstrDesc[]
// 
// Purpose:
//   Description of UltraSparc machine instructions.
// 
//---------------------------------------------------------------------------

namespace V9 {
  enum SparcMachineOpCode {
#define I(ENUM, OPCODESTRING, NUMOPERANDS, RESULTPOS, MAXIMM, IMMSE, \
          NUMDELAYSLOTS, LATENCY, SCHEDCLASS, INSTFLAGS)             \
   ENUM,
#include "SparcInstr.def"

    // End-of-array marker
    INVALID_OPCODE,
    NUM_REAL_OPCODES = PHI,		// number of valid opcodes
    NUM_TOTAL_OPCODES = INVALID_OPCODE
  };
}

// Array of machine instruction descriptions...
extern const TargetInstrDescriptor SparcMachineInstrDesc[];

//---------------------------------------------------------------------------
// class SparcSchedInfo
// 
// Purpose:
//   Interface to instruction scheduling information for UltraSPARC.
//   The parameter values above are based on UltraSPARC IIi.
//---------------------------------------------------------------------------

class SparcSchedInfo: public TargetSchedInfo {
public:
  SparcSchedInfo(const TargetMachine &tgt);
protected:
  virtual void initializeResources();
};

//---------------------------------------------------------------------------
// class SparcCacheInfo 
// 
// Purpose:
//   Interface to cache parameters for the UltraSPARC.
//   Just use defaults for now.
//---------------------------------------------------------------------------

struct SparcCacheInfo: public TargetCacheInfo {
  SparcCacheInfo(const TargetMachine &T) : TargetCacheInfo(T) {} 
};


/// createStackSlotsPass - External interface to stack-slots pass that enters 2
/// empty slots at the top of each function stack
///
Pass *createStackSlotsPass(const TargetMachine &TM);

/// Specializes LLVM code for a target machine.
///
FunctionPass *createPreSelectionPass(const TargetMachine &TM);

/// Peephole optimization pass operating on machine code
///
FunctionPass *createPeepholeOptsPass(const TargetMachine &TM);

/// Writes out assembly code for the module, one function at a time
///
FunctionPass *createAsmPrinterPass(std::ostream &Out, const TargetMachine &TM);

/// getPrologEpilogInsertionPass - Inserts prolog/epilog code.
///
FunctionPass* createPrologEpilogInsertionPass();

/// getBytecodeAsmPrinterPass - Emits final LLVM bytecode to assembly file.
///
Pass* createBytecodeAsmPrinterPass(std::ostream &Out);

FunctionPass *createSparcMachineCodeDestructionPass();

} // End llvm namespace

#endif
