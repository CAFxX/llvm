//===-- PowerPCTargetMachine.cpp - Define TargetMachine for PowerPC -------===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
// 
//
//===----------------------------------------------------------------------===//

#include "PowerPC.h"
#include "PowerPCTargetMachine.h"
#include "PowerPCFrameInfo.h"
#include "PPC32TargetMachine.h"
#include "PPC64TargetMachine.h"
#include "PPC32JITInfo.h"
#include "PPC64JITInfo.h"
#include "llvm/Module.h"
#include "llvm/PassManager.h"
#include "llvm/CodeGen/IntrinsicLowering.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Target/TargetMachineRegistry.h"
#include "llvm/Transforms/Scalar.h"
#include "Support/CommandLine.h"
#include <iostream>
using namespace llvm;

namespace llvm {
  cl::opt<bool> AIX("aix", 
                    cl::desc("Generate AIX/xcoff instead of Darwin/MachO"), 
                    cl::Hidden);
}

namespace {
  const std::string PPC32ID = "PowerPC/32bit";
  const std::string PPC64ID = "PowerPC/64bit";
  
  // Register the targets
  RegisterTarget<PPC32TargetMachine> 
  X("ppc32", "  PowerPC 32-bit (experimental)");

#if 0
  RegisterTarget<PPC64TargetMachine> 
  Y("ppc64", "  PowerPC 64-bit (unimplemented)");
#endif
}

PowerPCTargetMachine::PowerPCTargetMachine(const std::string &name,
                                           IntrinsicLowering *IL,
                                           const TargetData &TD,
                                           const PowerPCFrameInfo &TFI,
                                           const PowerPCJITInfo &TJI) 
  : TargetMachine(name, IL, TD), FrameInfo(TFI), JITInfo(TJI) 
{}

unsigned PowerPCTargetMachine::getJITMatchQuality() {
#if defined(__POWERPC__) || defined (__ppc__) || defined(_POWER)
  return 10;
#else
  return 0;
#endif
}

/// addPassesToEmitAssembly - Add passes to the specified pass manager
/// to implement a static compiler for this target.
///
bool PowerPCTargetMachine::addPassesToEmitAssembly(PassManager &PM,
                                                   std::ostream &Out) {
  bool LP64 = (0 != dynamic_cast<PPC64TargetMachine *>(this));
  
  // FIXME: Implement efficient support for garbage collection intrinsics.
  PM.add(createLowerGCPass());

  // FIXME: Implement the invoke/unwind instructions!
  PM.add(createLowerInvokePass());

  // FIXME: Implement the switch instruction in the instruction selector!
  PM.add(createLowerSwitchPass());

  PM.add(createLowerConstantExpressionsPass());

  // Make sure that no unreachable blocks are instruction selected.
  PM.add(createUnreachableBlockEliminationPass());

  if (LP64)
    PM.add(createPPC64ISelSimple(*this));
  else
    PM.add(createPPC32ISelSimple(*this));

  if (PrintMachineCode)
    PM.add(createMachineFunctionPrinterPass(&std::cerr));

  PM.add(createRegisterAllocator());

  if (PrintMachineCode)
    PM.add(createMachineFunctionPrinterPass(&std::cerr));

  PM.add(createPrologEpilogCodeInserter());
  
  // Must run branch selection immediately preceding the asm printer
  PM.add(createPPCBranchSelectionPass());
  
  if (AIX)
    PM.add(createPPC64AsmPrinter(Out, *this));
  else
    PM.add(createPPC32AsmPrinter(Out, *this));
    
  PM.add(createMachineCodeDeleter());
  return false;
}

void PowerPCJITInfo::addPassesToJITCompile(FunctionPassManager &PM) {
  // FIXME: Implement efficient support for garbage collection intrinsics.
  PM.add(createLowerGCPass());

  // FIXME: Implement the invoke/unwind instructions!
  PM.add(createLowerInvokePass());

  // FIXME: Implement the switch instruction in the instruction selector!
  PM.add(createLowerSwitchPass());

  PM.add(createLowerConstantExpressionsPass());

  // Make sure that no unreachable blocks are instruction selected.
  PM.add(createUnreachableBlockEliminationPass());

  PM.add(createPPC32ISelSimple(TM));
  PM.add(createRegisterAllocator());
  PM.add(createPrologEpilogCodeInserter());
}

void PowerPCJITInfo::replaceMachineCodeForFunction(void *Old, void *New) {
  assert(0 && "Cannot execute PowerPCJITInfo::replaceMachineCodeForFunction()");
}

void *PowerPCJITInfo::getJITStubForFunction(Function *F, 
                                            MachineCodeEmitter &MCE) {
  assert(0 && "Cannot execute PowerPCJITInfo::getJITStubForFunction()");
  return 0;
}

/// PowerPCTargetMachine ctor - Create an ILP32 architecture model
///
PPC32TargetMachine::PPC32TargetMachine(const Module &M, IntrinsicLowering *IL)
  : PowerPCTargetMachine(PPC32ID, IL, 
                         TargetData(PPC32ID,false,4,4,4,4,4,4,2,1,4),
                         PowerPCFrameInfo(*this, false), PPC32JITInfo(*this)) {}

/// PPC64TargetMachine ctor - Create a LP64 architecture model
///
PPC64TargetMachine::PPC64TargetMachine(const Module &M, IntrinsicLowering *IL)
  : PowerPCTargetMachine(PPC64ID, IL,
                         TargetData(PPC64ID,false,8,4,4,4,4,4,2,1,4),
                         PowerPCFrameInfo(*this, true), PPC64JITInfo(*this)) {}

unsigned PPC32TargetMachine::getModuleMatchQuality(const Module &M) {
  if (M.getEndianness()  == Module::BigEndian &&
      M.getPointerSize() == Module::Pointer32)
    return 10;                                   // Direct match
  else if (M.getEndianness() != Module::AnyEndianness ||
           M.getPointerSize() != Module::AnyPointerSize)
    return 0;                                    // Match for some other target

  return getJITMatchQuality()/2;
}

unsigned PPC64TargetMachine::getModuleMatchQuality(const Module &M) {
  if (M.getEndianness()  == Module::BigEndian &&
      M.getPointerSize() == Module::Pointer64)
    return 10;                                   // Direct match
  else if (M.getEndianness() != Module::AnyEndianness ||
           M.getPointerSize() != Module::AnyPointerSize)
    return 0;                                    // Match for some other target

  return getJITMatchQuality()/2;
}
