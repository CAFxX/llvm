//===- X86InstrInfo.cpp - X86 Instruction Information -----------*- C++ -*-===//
//
// This file contains the X86 implementation of the MachineInstrInfo class.
//
//===----------------------------------------------------------------------===//

#include "X86InstrInfo.h"
#include "X86.h"
#include "llvm/CodeGen/MachineInstr.h"

#define I(ENUM, NAME, BASEOPCODE, FLAGS, TSFLAGS, IMPDEFS, IMPUSES)
#define IMPREGSLIST(NAME, ...) \
  static const unsigned NAME[] = { __VA_ARGS__ };
#include "X86InstrInfo.def"


// X86Insts - Turn the InstrInfo.def file into a bunch of instruction
// descriptors
//
static const MachineInstrDescriptor X86Insts[] = {
#define I(ENUM, NAME, BASEOPCODE, FLAGS, TSFLAGS, IMPDEFS, IMPUSES)   \
             { NAME,                    \
               -1, /* Always vararg */  \
               ((TSFLAGS) & X86II::Void) ? -1 : 0,  /* Result is in 0 */ \
               0, false, 0, 0, TSFLAGS, FLAGS, TSFLAGS, IMPDEFS, IMPUSES },
#include "X86InstrInfo.def"
};

X86InstrInfo::X86InstrInfo()
  : MachineInstrInfo(X86Insts, sizeof(X86Insts)/sizeof(X86Insts[0]), 0) {
}


static unsigned char BaseOpcodes[] = {
#define I(ENUM, NAME, BASEOPCODE, FLAGS, TSFLAGS, IMPDEFS, IMPUSES) BASEOPCODE,
#include "X86InstrInfo.def"
};

// getBaseOpcodeFor - This function returns the "base" X86 opcode for the
// specified opcode number.
//
unsigned char X86InstrInfo::getBaseOpcodeFor(unsigned Opcode) const {
  assert(Opcode < sizeof(BaseOpcodes)/sizeof(BaseOpcodes[0]) &&
         "Opcode out of range!");
  return BaseOpcodes[Opcode];
}
