//===-- DelaySlotFiller.cpp - SparcV8 delay slot filler -------------------===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// Simple local delay slot filler for SparcV8 machine code
//
//===----------------------------------------------------------------------===//

#include "SparcV8.h"
#include "SparcV8InstrInfo.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Module.h"
#include "llvm/Assembly/Writer.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/Mangler.h"
#include "Support/Statistic.h"
#include "Support/StringExtras.h"
#include "Support/CommandLine.h"
#include <cctype>
using namespace llvm;

namespace {
  Statistic<> FilledSlots ("delayslotfiller", "Num. of delay slots filled");

  struct Filler : public MachineFunctionPass {
    /// Target machine description which we query for reg. names, data
    /// layout, etc.
    ///
    TargetMachine &TM;

    Filler (TargetMachine &tm) : TM (tm) { }

    virtual const char *getPassName () const {
      return "SparcV8 Delay Slot Filler";
    }

    bool runOnMachineBasicBlock (MachineBasicBlock &MBB);
    bool runOnMachineFunction (MachineFunction &F) {
      bool Changed = false;
      for (MachineFunction::iterator FI = F.begin (), FE = F.end ();
           FI != FE; ++FI)
        Changed |= runOnMachineBasicBlock (*FI);
      return Changed;
    }

  };
} // end of anonymous namespace

/// createSparcV8DelaySlotFillerPass - Returns a pass that fills in delay
/// slots in SparcV8 MachineFunctions
///
FunctionPass *llvm::createSparcV8DelaySlotFillerPass (TargetMachine &tm) {
  return new Filler (tm);
}

static bool hasDelaySlot (unsigned Opcode) {
  switch (Opcode) {
    case V8::CALL:
    case V8::RETL:
      return true;
    default:
      return false;
  }
}

/// runOnMachineBasicBlock - Fill in delay slots for the given basic block.
///
bool Filler::runOnMachineBasicBlock (MachineBasicBlock &MBB) {
  for (MachineBasicBlock::iterator I = MBB.begin (); I != MBB.end (); ++I)
    if (hasDelaySlot (I->getOpcode ())) {
      MachineBasicBlock::iterator J = I;
      ++J;
      MBB.insert (J, BuildMI (V8::NOP, 0));
      ++FilledSlots;
    }
  return false;
}
