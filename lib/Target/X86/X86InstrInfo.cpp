//===- X86InstrInfo.cpp - X86 Instruction Information ---------------===//
//
// This file contains the X86 implementation of the MachineInstrInfo class.
//
//===----------------------------------------------------------------------===//

#include "X86InstrInfo.h"
#include "llvm/CodeGen/MachineInstr.h"
#include <iostream>

// X86Insts - Turn the InstrInfo.def file into a bunch of instruction
// descriptors
//
static const MachineInstrDescriptor X86Insts[] = {
#define I(ENUM, NAME, FLAGS, TSFLAGS)   \
             { NAME,                    \
               -1, /* Always vararg */  \
               ((TSFLAGS) & X86II::Void) ? -1 : 0,  /* Result is in 0 */ \
               0, false, 0, 0, TSFLAGS, FLAGS, TSFLAGS },
#include "X86InstrInfo.def"
};

X86InstrInfo::X86InstrInfo()
  : MachineInstrInfo(X86Insts, sizeof(X86Insts)/sizeof(X86Insts[0]), 0) {
}


// print - Print out an x86 instruction in GAS syntax
void X86InstrInfo::print(const MachineInstr *MI, std::ostream &O) const {
  // FIXME: This sucks.
  O << getName(MI->getOpCode()) << "\n";
}

