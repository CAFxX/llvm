//===-- X86TargetMachine.cpp - Define TargetMachine for the X86 -----------===//
// 
// This file defines the X86 specific subclass of TargetMachine.
//
//===----------------------------------------------------------------------===//

#include "X86TargetMachine.h"
#include "X86.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Target/TargetMachineImpls.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/PassManager.h"
#include "Support/CommandLine.h"
#include "Support/Statistic.h"
#include <iostream>

namespace {
  cl::opt<bool> NoLocalRA("no-local-ra",
                          cl::desc("Use Simple RA instead of Local RegAlloc"));
  cl::opt<bool> PrintCode("print-machineinstrs",
			  cl::desc("Print generated machine code"));
}

// allocateX86TargetMachine - Allocate and return a subclass of TargetMachine
// that implements the X86 backend.
//
TargetMachine *allocateX86TargetMachine(unsigned Configuration) {
  return new X86TargetMachine(Configuration);
}


/// X86TargetMachine ctor - Create an ILP32 architecture model
///
X86TargetMachine::X86TargetMachine(unsigned Config)
  : TargetMachine("X86", 
		  (Config & TM::EndianMask) == TM::LittleEndian,
		  1, 4, 
		  (Config & TM::PtrSizeMask) == TM::PtrSize64 ? 8 : 4,
		  (Config & TM::PtrSizeMask) == TM::PtrSize64 ? 8 : 4) {
}


/// addPassesToJITCompile - Add passes to the specified pass manager to
/// implement a fast dynamic compiler for this target.  Return true if this is
/// not supported for this target.
///
bool X86TargetMachine::addPassesToJITCompile(PassManager &PM) {
  // For the moment we have decided that malloc and free will be
  // taken care of by converting them to calls, using the existing
  // LLVM scalar transforms pass to do this.
  PM.add(createLowerAllocationsPass());

  PM.add(createSimpleX86InstructionSelector(*this));

  // TODO: optional optimizations go here

  // Print the instruction selected machine code...
  if (PrintCode)
    PM.add(createMachineFunctionPrinterPass());

  // Perform register allocation to convert to a concrete x86 representation
  if (NoLocalRA)
    PM.add(createSimpleRegisterAllocator(*this));
  else
    PM.add(createLocalRegisterAllocator(*this));

  // Print the instruction selected machine code...
  // PM.add(createMachineFunctionPrinterPass());

  // Print the register-allocated code
  if (PrintCode)
    PM.add(createX86CodePrinterPass(*this, std::cerr));

  return false; // success!
}

