//===-- X86.h - Top-level interface for X86 representation ------*- C++ -*-===//
//
// This file contains the entry points for global functions defined in the x86
// target library, as used by the LLVM JIT.
//
// FIXME: This file will be dramatically changed in the future
//
//===----------------------------------------------------------------------===//

#ifndef TARGET_X86_H
#define TARGET_X86_H

#include <iosfwd>
class MachineFunction;
class Function;
class TargetMachine;

/// X86PrintCode - Print out the specified machine code function to the
/// specified stream.  This function should work regardless of whether or not
/// the function is in SSA form or not.
///
void X86PrintCode(const MachineFunction *MF, std::ostream &O);

/// X86SimpleInstructionSelection - This function converts an LLVM function into
/// a machine code representation is a very simple peep-hole fashion.  The
/// generated code sucks but the implementation is nice and simple.
///
MachineFunction *X86SimpleInstructionSelection(Function &F, TargetMachine &TM);

/// X86SimpleRegisterAllocation - This function converts the specified machine
/// code function from SSA form to use explicit registers by spilling every
/// register.  Wow, great policy huh?
///
inline void X86SimpleRegisterAllocation(MachineFunction *MF) {}

/// X86EmitCodeToMemory - This function converts a register allocated function
/// into raw machine code in a dynamically allocated chunk of memory.  A pointer
/// to the start of the function is returned.
///
inline void *X86EmitCodeToMemory(MachineFunction *MF) { return 0; }


// Put symbolic names in a namespace to avoid causing these to clash with all
// kinds of other things...
//
namespace X86 {
  // Defines a large number of symbolic names for X86 registers.  This defines a
  // mapping from register name to register number.
  //
  enum Register {
#define R(ENUM, NAME, FLAGS, TSFLAGS) ENUM,
#include "X86RegisterInfo.def"
  };

  // This defines a large number of symbolic names for X86 instruction opcodes.
  enum Opcode {
#define I(ENUM, NAME, FLAGS, TSFLAGS) ENUM,
#include "X86InstrInfo.def"
  };
}

#endif
