//===- X86InstructionInfo.h - X86 Instruction Information ---------*-C++-*-===//
//
// This file contains the X86 implementation of the MachineInstrInfo class.
//
//===----------------------------------------------------------------------===//

#ifndef X86INSTRUCTIONINFO_H
#define X86INSTRUCTIONINFO_H

#include "llvm/Target/MachineInstrInfo.h"
#include "X86RegisterInfo.h"

/// X86II - This namespace holds all of the target specific flags that
/// instruction info tracks.
///
namespace X86II {
  enum {
    //===------------------------------------------------------------------===//
    // Instruction types.  These are the standard/most common forms for X86
    // instructions.
    //

    // PseudoFrm - This represents an instruction that is a pseudo instruction
    // or one that has not been implemented yet.  It is illegal to code generate
    // it, but tolerated for intermediate implementation stages.
    Pseudo         = 0,

    /// Raw - This form is for instructions that don't have any operands, so
    /// they are just a fixed opcode value, like 'leave'.
    RawFrm         = 1,
    
    /// AddRegFrm - This form is used for instructions like 'push r32' that have
    /// their one register operand added to their opcode.
    AddRegFrm      = 2,

    /// MRMDestReg - This form is used for instructions that use the Mod/RM byte
    /// to specify a destination, which in this case is a register.
    ///
    MRMDestReg     = 3,

    /// MRMDestMem - This form is used for instructions that use the Mod/RM byte
    /// to specify a destination, which in this case is memory.
    ///
    MRMDestMem     = 4,

    /// MRMSrcReg - This form is used for instructions that use the Mod/RM byte
    /// to specify a source, which in this case is a register.
    ///
    MRMSrcReg      = 5,

    /// MRMSrcMem - This form is used for instructions that use the Mod/RM byte
    /// to specify a source, which in this case is memory.
    ///
    MRMSrcMem      = 6,
  
    /// MRMS[0-7][rm] - These forms are used to represent instructions that use
    /// a Mod/RM byte, and use the middle field to hold extended opcode
    /// information.  In the intel manual these are represented as /0, /1, ...
    ///

    // First, instructions that operate on a register r/m operand...
    MRMS0r = 16,  MRMS1r = 17,  MRMS2r = 18,  MRMS3r = 19, // Format /0 /1 /2 /3
    MRMS4r = 20,  MRMS5r = 21,  MRMS6r = 22,  MRMS7r = 23, // Format /4 /5 /6 /7

    // Next, instructions that operate on a memory r/m operand...
    MRMS0m = 24,  MRMS1m = 25,  MRMS2m = 26,  MRMS3m = 27, // Format /0 /1 /2 /3
    MRMS4m = 28,  MRMS5m = 29,  MRMS6m = 30,  MRMS7m = 31, // Format /4 /5 /6 /7

    FormMask       = 31,

    //===------------------------------------------------------------------===//
    // Actual flags...

    /// Void - Set if this instruction produces no value
    Void        = 1 << 5,

    // OpSize - Set if this instruction requires an operand size prefix (0x66),
    // which most often indicates that the instruction operates on 16 bit data
    // instead of 32 bit data.
    OpSize      = 1 << 6,

    // Op0Mask - There are several prefix bytes that are used to form two byte
    // opcodes.  These are currently 0x0F, and 0xD8-0xDF.  This mask is used to
    // obtain the setting of this field.  If no bits in this field is set, there
    // is no prefix byte for obtaining a multibyte opcode.
    //
    Op0Mask     = 0xF << 7,

    // TB - TwoByte - Set if this instruction has a two byte opcode, which
    // starts with a 0x0F byte before the real opcode.
    TB          = 1 << 7,

    // D8-DF - These escape opcodes are used by the floating point unit.  These
    // values must remain sequential.
    D8 = 2 << 7,   D9 = 3 << 7,   DA = 4 << 7,   DB = 5 << 7,
    DC = 6 << 7,   DD = 7 << 7,   DE = 8 << 7,   DF = 9 << 7,

    // This three-bit field describes the size of a memory operand.  Zero is
    // unused so that we can tell if we forgot to set a value.
    Arg8     = 1 << 11,
    Arg16    = 2 << 11,
    Arg32    = 3 << 11,
    ArgF32   = 4 << 11,
    ArgF64   = 5 << 11,
    ArgF80   = 6 << 11,
    ArgMask  = 7 << 11,

    // Bits 14 -> 31 are unused
  };
}

class X86InstrInfo : public MachineInstrInfo {
  const X86RegisterInfo RI;
public:
  X86InstrInfo();

  /// getRegisterInfo - MachineInstrInfo is a superset of MRegister info.  As
  /// such, whenever a client has an instance of instruction info, it should
  /// always be able to get register info as well (through this method).
  ///
  virtual const MRegisterInfo &getRegisterInfo() const { return RI; }

  /// print - Print out an x86 instruction in intel syntax
  ///
  virtual void print(const MachineInstr *MI, std::ostream &O,
                     const TargetMachine &TM) const;

  // getBaseOpcodeFor - This function returns the "base" X86 opcode for the
  // specified opcode number.
  //
  unsigned char getBaseOpcodeFor(unsigned Opcode) const;
};


#endif
