//===- PowerPCRegisterInfo.cpp - PowerPC Register Information ---*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file contains the PowerPC implementation of the MRegisterInfo class.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "reginfo"
#include "PowerPC.h"
#include "PowerPCRegisterInfo.h"
#include "PowerPCInstrBuilder.h"
#include "llvm/Constants.h"
#include "llvm/Type.h"
#include "llvm/CodeGen/ValueTypes.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/Target/TargetFrameInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "Support/CommandLine.h"
#include "Support/Debug.h"
#include "Support/STLExtras.h"
#include <iostream>
using namespace llvm;

namespace llvm {
  // Switch toggling compilation for AIX
  extern cl::opt<bool> AIX;
}

PowerPCRegisterInfo::PowerPCRegisterInfo(bool is64b)
  : PowerPCGenRegisterInfo(PPC::ADJCALLSTACKDOWN, PPC::ADJCALLSTACKUP),
    is64bit(is64b) {}

static unsigned getIdx(const TargetRegisterClass *RC) {
  if (RC == PowerPC::GPRCRegisterClass) {
    switch (RC->getSize()) {
      default: assert(0 && "Invalid data size!");
      case 1:  return 0;
      case 2:  return 1;
      case 4:  return 2;
      case 8:  return 3;
    }
  } else if (RC == PowerPC::FPRCRegisterClass) {
    switch (RC->getSize()) {
      default: assert(0 && "Invalid data size!");
      case 4:  return 4;
      case 8:  return 5;
    }
  }
  std::cerr << "Invalid register class to getIdx()!\n";
  abort();
}

int 
PowerPCRegisterInfo::storeRegToStackSlot(MachineBasicBlock &MBB,
                                         MachineBasicBlock::iterator MI,
                                         unsigned SrcReg, int FrameIdx,
                                         const TargetRegisterClass *RC) const {
  static const unsigned Opcode[] = { 
    PPC::STB, PPC::STH, PPC::STW, PPC::STD, PPC::STFS, PPC::STFD 
  };
  unsigned OC = Opcode[getIdx(RC)];
  if (SrcReg == PPC::LR) {
    MBB.insert(MI, BuildMI(PPC::MFLR, 0, PPC::R0));
    MBB.insert(MI, addFrameReference(BuildMI(OC,3).addReg(PPC::R0),FrameIdx));
    return 2;
  } else {
    MBB.insert(MI, addFrameReference(BuildMI(OC, 3).addReg(SrcReg),FrameIdx));
    return 1;
  }
}

int 
PowerPCRegisterInfo::loadRegFromStackSlot(MachineBasicBlock &MBB,
                                          MachineBasicBlock::iterator MI,
                                          unsigned DestReg, int FrameIdx,
                                          const TargetRegisterClass *RC) const {
  static const unsigned Opcode[] = { 
    PPC::LBZ, PPC::LHZ, PPC::LWZ, PPC::LD, PPC::LFS, PPC::LFD 
  };
  unsigned OC = Opcode[getIdx(RC)];
  if (DestReg == PPC::LR) {
    MBB.insert(MI, addFrameReference(BuildMI(OC, 2, PPC::R0), FrameIdx));
    MBB.insert(MI, BuildMI(PPC::MTLR, 1).addReg(PPC::R0));
    return 2;
  } else {
    MBB.insert(MI, addFrameReference(BuildMI(OC, 2, DestReg), FrameIdx));
    return 1;
  }
}

int PowerPCRegisterInfo::copyRegToReg(MachineBasicBlock &MBB,
                                      MachineBasicBlock::iterator MI,
                                      unsigned DestReg, unsigned SrcReg,
                                      const TargetRegisterClass *RC) const {
  MachineInstr *I;

  if (RC == PowerPC::GPRCRegisterClass) {
    I = BuildMI(PPC::OR, 2, DestReg).addReg(SrcReg).addReg(SrcReg);
  } else if (RC == PowerPC::FPRCRegisterClass) {
    I = BuildMI(PPC::FMR, 1, DestReg).addReg(SrcReg);
  } else { 
    std::cerr << "Attempt to copy register that is not GPR or FPR";
    abort();
  }
  MBB.insert(MI, I);
  return 1;
}

//===----------------------------------------------------------------------===//
// Stack Frame Processing methods
//===----------------------------------------------------------------------===//

// hasFP - Return true if the specified function should have a dedicated frame
// pointer register.  This is true if the function has variable sized allocas or
// if frame pointer elimination is disabled.
//
static bool hasFP(MachineFunction &MF) {
  return NoFramePointerElim || MF.getFrameInfo()->hasVarSizedObjects();
}

void PowerPCRegisterInfo::
eliminateCallFramePseudoInstr(MachineFunction &MF, MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator I) const {
  if (hasFP(MF)) {
    // If we have a frame pointer, convert as follows:
    // ADJCALLSTACKDOWN -> addi, r1, r1, -amount
    // ADJCALLSTACKUP   -> addi, r1, r1, amount
    MachineInstr *Old = I;
    unsigned Amount = Old->getOperand(0).getImmedValue();
    if (Amount != 0) {
      // We need to keep the stack aligned properly.  To do this, we round the
      // amount of space needed for the outgoing arguments up to the next
      // alignment boundary.
      unsigned Align = MF.getTarget().getFrameInfo()->getStackAlignment();
      Amount = (Amount+Align-1)/Align*Align;
      
      MachineInstr *New;
      if (Old->getOpcode() == PPC::ADJCALLSTACKDOWN) {
        New = BuildMI(PPC::ADDI, 2, PPC::R1).addReg(PPC::R1)
                .addSImm(-Amount);
      } else {
        assert(Old->getOpcode() == PPC::ADJCALLSTACKUP);
        New = BuildMI(PPC::ADDI, 2, PPC::R1).addReg(PPC::R1)
                .addSImm(Amount);
      }
      
      // Replace the pseudo instruction with a new instruction...
      MBB.insert(I, New);
    }
  }
  
  MBB.erase(I);
}

void
PowerPCRegisterInfo::eliminateFrameIndex(MachineFunction &MF,
                                         MachineBasicBlock::iterator II) const {
  unsigned i = 0;
  MachineInstr &MI = *II;
  while (!MI.getOperand(i).isFrameIndex()) {
    ++i;
    assert(i < MI.getNumOperands() && "Instr doesn't have FrameIndex operand!");
  }

  int FrameIndex = MI.getOperand(i).getFrameIndex();

  // Replace the FrameIndex with base register with GPR1.
  MI.SetMachineOperandReg(i, PPC::R1);

  // Take into account whether it's an add or mem instruction
  unsigned OffIdx = (i == 2) ? 1 : 2;

  // Now add the frame object offset to the offset from r1.
  int Offset = MF.getFrameInfo()->getObjectOffset(FrameIndex) +
               MI.getOperand(OffIdx).getImmedValue();

  // Fixed offsets have a negative frame index.  Fixed negative offests denote
  // spilled callee save regs.  Fixed positive offset is the va_start offset,
  // and needs to be added to the amount we decremented the stack pointer.
  // Positive frame indices are regular offsets from the stack pointer, and
  // also need the stack size added.
  if (FrameIndex >= 0 || (FrameIndex < 0 && Offset >= 24))
    Offset += MF.getFrameInfo()->getStackSize();

  MI.SetMachineOperandConst(OffIdx,MachineOperand::MO_SignExtendedImmed,Offset);
}


void PowerPCRegisterInfo::emitPrologue(MachineFunction &MF) const {
  MachineBasicBlock &MBB = MF.front();   // Prolog goes in entry BB
  MachineBasicBlock::iterator MBBI = MBB.begin();
  MachineFrameInfo *MFI = MF.getFrameInfo();
  MachineInstr *MI;
  
  // Get the number of bytes to allocate from the FrameInfo
  unsigned NumBytes = MFI->getStackSize();

  // If we have calls, we cannot use the red zone to store callee save registers
  // and we must set up a stack frame, so calculate the necessary size here.
  if (MFI->hasCalls()) {
    // We reserve argument space for call sites in the function immediately on 
    // entry to the current function.  This eliminates the need for add/sub 
    // brackets around call sites.
    NumBytes += MFI->getMaxCallFrameSize();
  }

  // Do we need to allocate space on the stack?
  if (NumBytes == 0) return;

  // Add the size of R1 to  NumBytes size for the store of R1 to the bottom 
  // of the stack and round the size to a multiple of the alignment.
  unsigned Align = MF.getTarget().getFrameInfo()->getStackAlignment();
  unsigned Size = getRegClass(PPC::R1)->getSize();
  NumBytes = (NumBytes+Size+Align-1)/Align*Align;

  // Update frame info to pretend that this is part of the stack...
  MFI->setStackSize(NumBytes);

  // adjust stack pointer: r1 -= numbytes
  if (NumBytes <= 32768) {
    unsigned StoreOpcode = is64bit ? PPC::STDU : PPC::STWU;
    MI = BuildMI(StoreOpcode, 3).addReg(PPC::R1).addSImm(-NumBytes)
      .addReg(PPC::R1);
    MBB.insert(MBBI, MI);
  } else {
    int NegNumbytes = -NumBytes;
    unsigned StoreOpcode = is64bit ? PPC::STDUX : PPC::STWUX;
    MI = BuildMI(PPC::LIS, 1, PPC::R0).addSImm(NegNumbytes >> 16);
    MBB.insert(MBBI, MI);
    MI = BuildMI(PPC::ORI, 2, PPC::R0).addReg(PPC::R0)
      .addImm(NegNumbytes & 0xFFFF);
    MBB.insert(MBBI, MI);
    MI = BuildMI(StoreOpcode, 3).addReg(PPC::R1).addReg(PPC::R1)
      .addReg(PPC::R0);
    MBB.insert(MBBI, MI);
  }
}

void PowerPCRegisterInfo::emitEpilogue(MachineFunction &MF,
                                       MachineBasicBlock &MBB) const {
  const MachineFrameInfo *MFI = MF.getFrameInfo();
  MachineBasicBlock::iterator MBBI = prior(MBB.end());
  MachineInstr *MI;
  assert(MBBI->getOpcode() == PPC::BLR &&
         "Can only insert epilog into returning blocks");
  
  // Get the number of bytes allocated from the FrameInfo...
  unsigned NumBytes = MFI->getStackSize();

  if (NumBytes != 0) {
    unsigned Opcode = is64bit ? PPC::LD : PPC::LWZ;
    MI = BuildMI(Opcode, 2, PPC::R1).addSImm(0).addReg(PPC::R1);
    MBB.insert(MBBI, MI);
  }
}

#include "PowerPCGenRegisterInfo.inc"

const TargetRegisterClass*
PowerPCRegisterInfo::getRegClassForType(const Type* Ty) const {
  switch (Ty->getTypeID()) {
    default:              assert(0 && "Invalid type to getClass!");
    case Type::LongTyID:
    case Type::ULongTyID:
      if (!is64bit) assert(0 && "Long values can't fit in registers!");
    case Type::BoolTyID:
    case Type::SByteTyID:
    case Type::UByteTyID:
    case Type::ShortTyID:
    case Type::UShortTyID:
    case Type::IntTyID:
    case Type::UIntTyID:
    case Type::PointerTyID: return &GPRCInstance;
     
    case Type::FloatTyID:
    case Type::DoubleTyID: return &FPRCInstance;
  }
}

