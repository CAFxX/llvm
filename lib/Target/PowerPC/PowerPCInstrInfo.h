//===- PowerPCInstrInfo.h - PowerPC Instruction Information -----*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file contains the PowerPC implementation of the TargetInstrInfo class.
//
//===----------------------------------------------------------------------===//

#ifndef POWERPC_INSTRUCTIONINFO_H
#define POWERPC_INSTRUCTIONINFO_H

#include "PowerPC.h"
#include "PowerPCRegisterInfo.h"
#include "llvm/Target/TargetInstrInfo.h"

namespace llvm {

namespace PPCII {
	enum {
		ArgCountShift = 0,
		ArgCountMask = 7,
		
		Arg0TypeShift = 3,
		Arg1TypeShift = 8,
		Arg2TypeShift = 13,
		Arg3TypeShift = 18,
		Arg4TypeShift = 23,
		VMX = 1<<28,
		PPC64 = 1<<29,
		ArgTypeMask = 31
	};
	
	enum {
		None = 0,
		Gpr = 1,
		Gpr0 = 2,
		Simm16 = 3,
		Zimm16 = 4,
		PCRelimm24 = 5,
		Imm24 = 6,
		Imm5 = 7,
		PCRelimm14 = 8,
		Imm14 = 9,
		Imm2 = 10,
		Crf = 11,
		Imm3 = 12,
		Imm1 = 13,
		Fpr = 14,
		Imm4 = 15,
		Imm8 = 16,
		Disimm16 = 17,
		Disimm14 = 18,
		Spr = 19,
		Sgr = 20,
		Imm15 = 21,
		Vpr = 22
	};
}

class PowerPCInstrInfo : public TargetInstrInfo {
  const PowerPCRegisterInfo RI;
  bool is64bit;
public:
  PowerPCInstrInfo(bool is64b);

  /// getRegisterInfo - TargetInstrInfo is a superset of MRegister info.  As
  /// such, whenever a client has an instance of instruction info, it should
  /// always be able to get register info as well (through this method).
  ///
  virtual const MRegisterInfo &getRegisterInfo() const { return RI; }

  //
  // Return true if the instruction is a register to register move and
  // leave the source and dest operands in the passed parameters.
  //
  virtual bool isMoveInstr(const MachineInstr& MI,
                           unsigned& sourceReg,
                           unsigned& destReg) const;

  static unsigned invertPPCBranchOpcode(unsigned Opcode) {
    switch (Opcode) {
    default: assert(0 && "Unknown PPC branch opcode!");
    case PPC::BEQ: return PPC::BNE;
    case PPC::BNE: return PPC::BEQ;
    case PPC::BLT: return PPC::BGE;
    case PPC::BGE: return PPC::BLT;
    case PPC::BGT: return PPC::BLE;
    case PPC::BLE: return PPC::BGT;
    } 
  }
};

}

#endif
