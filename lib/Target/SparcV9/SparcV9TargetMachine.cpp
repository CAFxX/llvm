//===-- Sparc.cpp - General implementation file for the Sparc Target ------===//
//
// This file contains the code for the Sparc Target that does not fit in any of
// the other files in this directory.
//
//===----------------------------------------------------------------------===//

#include "SparcInternals.h"
#include "llvm/Target/Sparc.h"
#include "llvm/CodeGen/InstrScheduling.h"
#include "llvm/CodeGen/InstrSelection.h"
#include "llvm/CodeGen/MachineCodeForInstruction.h"
#include "llvm/CodeGen/MachineCodeForMethod.h"
#include "llvm/CodeGen/RegisterAllocation.h"
#include "llvm/Function.h"
#include "llvm/BasicBlock.h"
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

TargetMachine *allocateSparcTargetMachine() { return new UltraSparc(); }



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
  mcInfo.freezeAutomaticVarsArea();     // ensure no more auto vars are added
  
  pos = false;                          // static stack area grows downwards
  unsigned int autoVarsSize = mcInfo.getAutomaticVarsSize();
  return StaticAreaOffsetFromFP - autoVarsSize; 
}

int
UltraSparcFrameInfo::getTmpAreaOffset(MachineCodeForMethod& mcInfo,
                                      bool& pos) const
{
  mcInfo.freezeAutomaticVarsArea();     // ensure no more auto vars are added
  mcInfo.freezeSpillsArea();            // ensure no more spill slots are added
  
  pos = false;                          // static stack area grows downwards
  unsigned int autoVarsSize = mcInfo.getAutomaticVarsSize();
  unsigned int spillAreaSize = mcInfo.getRegSpillsSize();
  int offset = autoVarsSize + spillAreaSize;
  return StaticAreaOffsetFromFP - offset;
}

int
UltraSparcFrameInfo::getDynamicAreaOffset(MachineCodeForMethod& mcInfo,
                                          bool& pos) const
{
  // Dynamic stack area grows downwards starting at top of opt-args area.
  // The opt-args, required-args, and register-save areas are empty except
  // during calls and traps, so they are shifted downwards on each
  // dynamic-size alloca.
  pos = false;
  unsigned int optArgsSize = mcInfo.getMaxOptionalArgsSize();
  int offset = optArgsSize + FirstOptionalOutgoingArgOffsetFromSP;
  assert((offset - OFFSET) % getStackFrameSizeAlignment() == 0);
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

class ConstructMachineCodeForFunction : public MethodPass {
  TargetMachine &Target;
public:
  inline ConstructMachineCodeForFunction(TargetMachine &T) : Target(T) {}
  bool runOnMethod(Function *F) {
    MachineCodeForMethod::construct(F, Target);
    return false;
  }
};

class InstructionSelection : public MethodPass {
  TargetMachine &Target;
public:
  inline InstructionSelection(TargetMachine &T) : Target(T) {}
  bool runOnMethod(Function *F) {
    if (SelectInstructionsForMethod(F, Target)) {
      cerr << "Instr selection failed for function " << F->getName() << "\n";
      abort();
    }
    return false;
  }
};

struct FreeMachineCodeForFunction : public MethodPass {
  static void freeMachineCode(Instruction *I) {
    MachineCodeForInstruction::destroy(I);
  }
  
  bool runOnMethod(Function *F) {
    for (Function::iterator FI = F->begin(), FE = F->end(); FI != FE; ++FI)
      for (BasicBlock::iterator I = (*FI)->begin(), E = (*FI)->end();
           I != E; ++I)
        MachineCodeForInstruction::get(*I).dropAllReferences();
    
    for (Function::iterator FI = F->begin(), FE = F->end(); FI != FE; ++FI)
      for (BasicBlock::iterator I = (*FI)->begin(), E = (*FI)->end();
           I != E; ++I)
        freeMachineCode(*I);
    
    return false;
  }
};



// addPassesToEmitAssembly - This method controls the entire code generation
// process for the ultra sparc.
//
void UltraSparc::addPassesToEmitAssembly(PassManager &PM, std::ostream &Out) {
  // Construct and initialize the MachineCodeForMethod object for this fn.
  PM.add(new ConstructMachineCodeForFunction(*this));

  PM.add(new InstructionSelection(*this));

  PM.add(createInstructionSchedulingWithSSAPass(*this));

  PM.add(getRegisterAllocator(*this));
  
  //PM.add(new OptimizeLeafProcedures());
  //PM.add(new DeleteFallThroughBranches());
  //PM.add(new RemoveChainedBranches());    // should be folded with previous
  //PM.add(new RemoveRedundantOps());       // operations with %g0, NOP, etc.
  
  PM.add(createPrologEpilogCodeInserter(*this));
  
  // Output assembly language to the .s file.  Assembly emission is split into
  // two parts: Function output and Global value output.  This is because
  // function output is pipelined with all of the rest of code generation stuff,
  // allowing machine code representations for functions to be free'd after the
  // function has been emitted.
  //
  PM.add(getMethodAsmPrinterPass(PM, Out));
  PM.add(new FreeMachineCodeForFunction());  // Free stuff no longer needed

  // Emit Module level assembly after all of the functions have been processed.
  PM.add(getModuleAsmPrinterPass(PM, Out));

  // Emit bytecode to the sparc assembly file into its special section next
  PM.add(getEmitBytecodeToAsmPass(Out));
}
