//===-- Sparc.cpp - General implementation file for the Sparc Target ------===//
//
// This file contains the code for the Sparc Target that does not fit in any of
// the other files in this directory.
//
//===----------------------------------------------------------------------===//

#include "SparcInternals.h"
#include "llvm/Target/TargetMachineImpls.h"
#include "llvm/Function.h"
#include "llvm/PassManager.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionInfo.h"
#include "llvm/CodeGen/PreSelection.h"
#include "llvm/CodeGen/StackSlots.h"
#include "llvm/CodeGen/PeepholeOpts.h"
#include "llvm/CodeGen/InstrSelection.h"
#include "llvm/CodeGen/InstrScheduling.h"
#include "llvm/CodeGen/RegisterAllocation.h"
#include "llvm/CodeGen/MachineCodeForInstruction.h"
#include "llvm/Reoptimizer/Mapping/MappingInfo.h" 
#include "llvm/Reoptimizer/Mapping/FInfo.h" 
#include "Support/CommandLine.h"
using std::cerr;

static const unsigned ImplicitRegUseList[] = { 0 }; /* not used yet */
// Build the MachineInstruction Description Array...
const TargetInstrDescriptor SparcMachineInstrDesc[] = {
#define I(ENUM, OPCODESTRING, NUMOPERANDS, RESULTPOS, MAXIMM, IMMSE, \
          NUMDELAYSLOTS, LATENCY, SCHEDCLASS, INSTFLAGS)             \
  { OPCODESTRING, NUMOPERANDS, RESULTPOS, MAXIMM, IMMSE,             \
          NUMDELAYSLOTS, LATENCY, SCHEDCLASS, INSTFLAGS, 0,          \
          ImplicitRegUseList, ImplicitRegUseList },
#include "SparcInstr.def"
};

//---------------------------------------------------------------------------
// Command line options to control choice of code generation passes.
//---------------------------------------------------------------------------

static cl::opt<bool> DisablePreSelect("nopreselect",
                                      cl::desc("Disable preselection pass"));

static cl::opt<bool> DisableSched("nosched",
                                  cl::desc("Disable local scheduling pass"));

static cl::opt<bool> DisablePeephole("nopeephole",
                                cl::desc("Disable peephole optimization pass"));

//----------------------------------------------------------------------------
// allocateSparcTargetMachine - Allocate and return a subclass of TargetMachine
// that implements the Sparc backend. (the llvm/CodeGen/Sparc.h interface)
//----------------------------------------------------------------------------

TargetMachine *allocateSparcTargetMachine(unsigned Configuration) {
  return new UltraSparc();
}

//---------------------------------------------------------------------------
// class UltraSparcFrameInfo 
// 
//   Interface to stack frame layout info for the UltraSPARC.
//   Starting offsets for each area of the stack frame are aligned at
//   a multiple of getStackFrameSizeAlignment().
//---------------------------------------------------------------------------

int
UltraSparcFrameInfo::getFirstAutomaticVarOffset(MachineFunction& ,
                                                bool& pos) const
{
  pos = false;                          // static stack area grows downwards
  return StaticAreaOffsetFromFP;
}

int
UltraSparcFrameInfo::getRegSpillAreaOffset(MachineFunction& mcInfo,
                                           bool& pos) const
{
  // ensure no more auto vars are added
  mcInfo.getInfo()->freezeAutomaticVarsArea();
  
  pos = false;                          // static stack area grows downwards
  unsigned autoVarsSize = mcInfo.getInfo()->getAutomaticVarsSize();
  return StaticAreaOffsetFromFP - autoVarsSize; 
}

int
UltraSparcFrameInfo::getTmpAreaOffset(MachineFunction& mcInfo,
                                      bool& pos) const
{
  MachineFunctionInfo *MFI = mcInfo.getInfo();
  MFI->freezeAutomaticVarsArea();     // ensure no more auto vars are added
  MFI->freezeSpillsArea();            // ensure no more spill slots are added
  
  pos = false;                          // static stack area grows downwards
  unsigned autoVarsSize = MFI->getAutomaticVarsSize();
  unsigned spillAreaSize = MFI->getRegSpillsSize();
  int offset = autoVarsSize + spillAreaSize;
  return StaticAreaOffsetFromFP - offset;
}

int
UltraSparcFrameInfo::getDynamicAreaOffset(MachineFunction& mcInfo,
                                          bool& pos) const
{
  // Dynamic stack area grows downwards starting at top of opt-args area.
  // The opt-args, required-args, and register-save areas are empty except
  // during calls and traps, so they are shifted downwards on each
  // dynamic-size alloca.
  pos = false;
  unsigned optArgsSize = mcInfo.getInfo()->getMaxOptionalArgsSize();
  if (int extra = optArgsSize % getStackFrameSizeAlignment())
    optArgsSize += (getStackFrameSizeAlignment() - extra);
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
//   for classes such as TargetInstrInfo. 
// 
//---------------------------------------------------------------------------

UltraSparc::UltraSparc()
  : TargetMachine("UltraSparc-Native", false),
    schedInfo(*this),
    regInfo(*this),
    frameInfo(*this),
    cacheInfo(*this),
    optInfo(*this) {
}


// addPassesToEmitAssembly - This method controls the entire code generation
// process for the ultra sparc.
//
bool UltraSparc::addPassesToEmitAssembly(PassManager &PM, std::ostream &Out)
{
  // FIXME: implement the switch instruction in the instruction selector.
  PM.add(createLowerSwitchPass());

  // Construct and initialize the MachineFunction object for this fn.
  PM.add(createMachineCodeConstructionPass(*this));

  //Insert empty stackslots in the stack frame of each function
  //so %fp+offset-8 and %fp+offset-16 are empty slots now!
  PM.add(createStackSlotsPass(*this));

  // Specialize LLVM code for this target machine and then
  // run basic dataflow optimizations on LLVM code.
  if (!DisablePreSelect) {
    PM.add(createPreSelectionPass(*this));
    PM.add(createReassociatePass());
    PM.add(createLICMPass());
    PM.add(createGCSEPass());
  }

  PM.add(createInstructionSelectionPass(*this));

  if (!DisableSched)
    PM.add(createInstructionSchedulingWithSSAPass(*this));

  PM.add(getRegisterAllocator(*this));

  PM.add(getPrologEpilogInsertionPass());

  if (!DisablePeephole)
    PM.add(createPeepholeOptsPass(*this));

  PM.add(MappingInfoForFunction(Out));  

  // Output assembly language to the .s file.  Assembly emission is split into
  // two parts: Function output and Global value output.  This is because
  // function output is pipelined with all of the rest of code generation stuff,
  // allowing machine code representations for functions to be free'd after the
  // function has been emitted.
  //
  PM.add(getFunctionAsmPrinterPass(Out));
  PM.add(createMachineCodeDestructionPass()); // Free stuff no longer needed

  // Emit Module level assembly after all of the functions have been processed.
  PM.add(getModuleAsmPrinterPass(Out));

  // Emit bytecode to the assembly file into its special section next
  PM.add(getEmitBytecodeToAsmPass(Out));
  PM.add(getFunctionInfo(Out)); 
  return false;
}

// addPassesToJITCompile - This method controls the JIT method of code
// generation for the UltraSparc.
//
bool UltraSparc::addPassesToJITCompile(PassManager &PM) {
  // FIXME: implement the switch instruction in the instruction selector.
  PM.add(createLowerSwitchPass());

  // Construct and initialize the MachineFunction object for this fn.
  PM.add(createMachineCodeConstructionPass(*this));

  //Insert empty stackslots in the stack frame of each function
  //so %fp+offset-8 and %fp+offset-16 are empty slots now!
  PM.add(createStackSlotsPass(*this));

  // Specialize LLVM code for this target machine and then
  // run basic dataflow optimizations on LLVM code.
  if (!DisablePreSelect) {
    PM.add(createPreSelectionPass(*this));
    PM.add(createReassociatePass());
    PM.add(createLICMPass());
    PM.add(createGCSEPass());
  }

  PM.add(createInstructionSelectionPass(*this));

  if (!DisableSched)
    PM.add(createInstructionSchedulingWithSSAPass(*this));

  // new pass: convert Value* in MachineOperand to an unsigned register
  // this brings it in line with what the X86 JIT's RegisterAllocator expects
  //PM.add(createAddRegNumToValuesPass());

  PM.add(getRegisterAllocator(*this));
  PM.add(getPrologEpilogInsertionPass());

  if (!DisablePeephole)
    PM.add(createPeepholeOptsPass(*this));

  return false; // success!
}
