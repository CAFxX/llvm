//===- X86RegisterInfo.h - X86 Register Information Impl --------*- C++ -*-===//
//
// This file contains the X86 implementation of the MRegisterInfo class.
//
//===----------------------------------------------------------------------===//

#ifndef X86REGISTERINFO_H
#define X86REGISTERINFO_H

#include "llvm/Target/MRegisterInfo.h"

class Type;

struct X86RegisterInfo : public MRegisterInfo {
  X86RegisterInfo();

  const unsigned* getCalleeSaveRegs() const;

  /// Returns register class appropriate for input SSA register
  /// 
  const TargetRegisterClass *getClassForReg(unsigned Reg) const;
  const TargetRegisterClass* getRegClassForType(const Type* Ty) const;

  /// Code Generation virtual methods...
  void storeRegToStackSlot(MachineBasicBlock &MBB,
			   MachineBasicBlock::iterator &MBBI,
			   unsigned SrcReg, int FrameIndex,
			   const TargetRegisterClass *RC) const;

  void loadRegFromStackSlot(MachineBasicBlock &MBB,
			    MachineBasicBlock::iterator &MBBI,
			    unsigned DestReg, int FrameIndex,
			    const TargetRegisterClass *RC) const;
  
  void copyRegToReg(MachineBasicBlock &MBB, MachineBasicBlock::iterator &MBBI,
		   unsigned DestReg, unsigned SrcReg,
		   const TargetRegisterClass *RC) const;

  void eliminateCallFramePseudoInstr(MachineFunction &MF,
				     MachineBasicBlock &MBB,
				     MachineBasicBlock::iterator &I) const;

  void eliminateFrameIndex(MachineFunction &MF,
			   MachineBasicBlock::iterator &II) const;

  void processFunctionBeforeFrameFinalized(MachineFunction &MF) const;

  void emitPrologue(MachineFunction &MF) const;
  void emitEpilogue(MachineFunction &MF, MachineBasicBlock &MBB) const;
};

#endif
