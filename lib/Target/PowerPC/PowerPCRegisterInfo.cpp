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

PowerPCRegisterInfo::PowerPCRegisterInfo()
  : PowerPCGenRegisterInfo(PPC32::ADJCALLSTACKDOWN,
                           PPC32::ADJCALLSTACKUP) {}

static unsigned getIdx(const TargetRegisterClass *RC) {
  if (RC == PowerPC::GPRCRegisterClass) {
    switch (RC->getSize()) {
      default: assert(0 && "Invalid data size!");
      case 1:  return 0;
      case 2:  return 1;
      case 4:  return 2;
    }
  } else if (RC == PowerPC::FPRCRegisterClass) {
    switch (RC->getSize()) {
      default: assert(0 && "Invalid data size!");
      case 4:  return 3;
      case 8:  return 4;
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
    PPC32::STB, PPC32::STH, PPC32::STW, PPC32::STFS, PPC32::STFD 
  };
  unsigned OC = Opcode[getIdx(RC)];
  if (SrcReg == PPC32::LR) {
    MBB.insert(MI, BuildMI(PPC32::MFLR, 0, PPC32::R0));
    OC = PPC32::STW;
    SrcReg = PPC32::R0;
  }
  MBB.insert(MI, addFrameReference(BuildMI(OC, 3).addReg(SrcReg),FrameIdx));
  return 1;
}

int 
PowerPCRegisterInfo::loadRegFromStackSlot(MachineBasicBlock &MBB,
                                          MachineBasicBlock::iterator MI,
                                          unsigned DestReg, int FrameIdx,
                                          const TargetRegisterClass *RC) const {
  static const unsigned Opcode[] = { 
    PPC32::LBZ, PPC32::LHZ, PPC32::LWZ, PPC32::LFS, PPC32::LFD 
  };
  unsigned OC = Opcode[getIdx(RC)];
  bool LoadLR = false;
  if (DestReg == PPC32::LR) {
    DestReg = PPC32::R0;
    LoadLR = true;
    OC = PPC32::LWZ;
  }
  MBB.insert(MI, addFrameReference(BuildMI(OC, 2, DestReg), FrameIdx));
  if (LoadLR)
    MBB.insert(MI, BuildMI(PPC32::MTLR, 1).addReg(PPC32::R0));

  return 1;
}

int PowerPCRegisterInfo::copyRegToReg(MachineBasicBlock &MBB,
                                      MachineBasicBlock::iterator MI,
                                      unsigned DestReg, unsigned SrcReg,
                                      const TargetRegisterClass *RC) const {
  MachineInstr *I;

  if (RC == PowerPC::GPRCRegisterClass) {
    I = BuildMI(PPC32::OR, 2, DestReg).addReg(SrcReg).addReg(SrcReg);
  } else if (RC == PowerPC::FPRCRegisterClass) {
    I = BuildMI(PPC32::FMR, 1, DestReg).addReg(SrcReg);
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
    // adjcallstackdown instruction => 'sub r1, r1, <amt>' and 
    // adjcallstackup instruction   => 'add r1, r1, <amt>'
    MachineInstr *Old = I;
    int Amount = Old->getOperand(0).getImmedValue();
    if (Amount != 0) {
      // We need to keep the stack aligned properly.  To do this, we round the
      // amount of space needed for the outgoing arguments up to the next
      // alignment boundary.
      unsigned Align = MF.getTarget().getFrameInfo()->getStackAlignment();
      Amount = (Amount+Align-1)/Align*Align;
      
      MachineInstr *New;
      if (Old->getOpcode() == PPC32::ADJCALLSTACKDOWN) {
        New = BuildMI(PPC32::ADDI, 2, PPC32::R1).addReg(PPC32::R1)
                .addSImm(-Amount);
      } else {
        assert(Old->getOpcode() == PPC32::ADJCALLSTACKUP);
        New = BuildMI(PPC32::ADDI, 2, PPC32::R1).addReg(PPC32::R1)
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
  MI.SetMachineOperandReg(i, PPC32::R1);

  // Take into account whether it's an add or mem instruction
  unsigned OffIdx = (i == 2) ? 1 : 2;
  // Now add the frame object offset to the offset from r1.
  int Offset = MF.getFrameInfo()->getObjectOffset(FrameIndex) +
               MI.getOperand(OffIdx).getImmedValue()+4;

  if (!hasFP(MF))
    Offset += MF.getFrameInfo()->getStackSize();

  MI.SetMachineOperandConst(OffIdx,MachineOperand::MO_SignExtendedImmed,Offset);
  DEBUG(std::cerr << "offset = " << Offset << std::endl);
}


void PowerPCRegisterInfo::emitPrologue(MachineFunction &MF) const {
  MachineBasicBlock &MBB = MF.front();   // Prolog goes in entry BB
  MachineBasicBlock::iterator MBBI = MBB.begin();
  MachineFrameInfo *MFI = MF.getFrameInfo();
  MachineInstr *MI;
  
  // Get the number of bytes to allocate from the FrameInfo
  unsigned NumBytes = MFI->getStackSize();

  // If we have calls, save the LR value on the stack
  if (MFI->hasCalls() || true) {
    // When we have no frame pointer, we reserve argument space for call sites
    // in the function immediately on entry to the current function.  This
    // eliminates the need for add/sub brackets around call sites.
    NumBytes += MFI->getMaxCallFrameSize() + 
                24 + // Predefined PowerPC link area
                32 + // Predefined PowerPC params area
                 0 + // local variables - managed by LLVM
             0 * 4 + // volatile GPRs used - managed by LLVM
             0 * 8;  // volatile FPRs used - managed by LLVM
             
    // Round the size to a multiple of the alignment (don't forget the 4 byte
    // offset though).
    unsigned Align = MF.getTarget().getFrameInfo()->getStackAlignment();
    NumBytes = ((NumBytes+4)+Align-1)/Align*Align - 4;

    // Store the incoming LR so it is preserved across calls
    MI = BuildMI(PPC32::MFLR, 0, PPC32::R0);
    MBB.insert(MBBI, MI);
    MI = BuildMI(PPC32::STW, 3).addReg(PPC32::R0).addSImm(8).addReg(PPC32::R1);
    MBB.insert(MBBI, MI);
  }

  // Update frame info to pretend that this is part of the stack...
  MFI->setStackSize(NumBytes);
  
  // Do we need to allocate space on the stack?
  if (NumBytes == 0) return;

  // adjust stack pointer: r1 -= numbytes
  if (NumBytes <= 32768) {
    MI = BuildMI(PPC32::STWU, 3).addReg(PPC32::R1).addSImm(-NumBytes)
      .addReg(PPC32::R1);
    MBB.insert(MBBI, MI);
  } else {
    int NegNumbytes = -NumBytes;
    MI = BuildMI(PPC32::LIS, 1, PPC32::R0).addSImm(NegNumbytes >> 16);
    MBB.insert(MBBI, MI);
    MI = BuildMI(PPC32::ORI, 2, PPC32::R0).addReg(PPC32::R0)
      .addImm(NegNumbytes & 0xFFFF);
    MBB.insert(MBBI, MI);
    MI = BuildMI(PPC32::STWUX, 3).addReg(PPC32::R1).addReg(PPC32::R1)
      .addReg(PPC32::R0);
    MBB.insert(MBBI, MI);
  }
}

void PowerPCRegisterInfo::emitEpilogue(MachineFunction &MF,
                                       MachineBasicBlock &MBB) const {
  const MachineFrameInfo *MFI = MF.getFrameInfo();
  MachineBasicBlock::iterator MBBI = prior(MBB.end());
  MachineInstr *MI;
  assert(MBBI->getOpcode() == PPC32::BLR &&
         "Can only insert epilog into returning blocks");
  
  // Get the number of bytes allocated from the FrameInfo...
  unsigned NumBytes = MFI->getStackSize();

  if (NumBytes == 0 && (MFI->hasCalls() || true)) {
    // Don't need to adjust the stack pointer, just gotta fix up the LR
    MI = BuildMI(PPC32::LWZ, 2,PPC32::R0).addSImm(NumBytes+8).addReg(PPC32::R1);
    MBB.insert(MBBI, MI);
    MI = BuildMI(PPC32::MTLR, 1).addReg(PPC32::R0);
    MBB.insert(MBBI, MI);
  } else if (NumBytes <= 32767-8) {
    // We're within range to load the return address and restore the stack
    // pointer with immediate offsets only.
    if (MFI->hasCalls() || true) {
      MI = BuildMI(PPC32::LWZ,2,PPC32::R0).addSImm(NumBytes+8).addReg(PPC32::R1);
      MBB.insert(MBBI, MI);
      MI = BuildMI(PPC32::MTLR, 1).addReg(PPC32::R0);
      MBB.insert(MBBI, MI);
    }
    MI = BuildMI(PPC32::ADDI, 2, PPC32::R1).addReg(PPC32::R1).addSImm(NumBytes);
    MBB.insert(MBBI, MI);
  } else {
    MI = BuildMI(PPC32::LWZ, 2, PPC32::R1).addSImm(0).addReg(PPC32::R1);
    MBB.insert(MBBI, MI);
    // We're not within range to load the return address with an immediate
    // offset before restoring the stack pointer, so do it after from its spot
    // in the linkage area.
    if (MFI->hasCalls() || true) {
      MI = BuildMI(PPC32::LWZ, 2, PPC32::R0).addSImm(8).addReg(PPC32::R1);
      MBB.insert(MBBI, MI);
      MI = BuildMI(PPC32::MTLR, 1).addReg(PPC32::R0);
      MBB.insert(MBBI, MI);
    }
  }
}

#include "PowerPCGenRegisterInfo.inc"

const TargetRegisterClass*
PowerPCRegisterInfo::getRegClassForType(const Type* Ty) const {
  switch (Ty->getTypeID()) {
    case Type::LongTyID:
    case Type::ULongTyID: assert(0 && "Long values can't fit in registers!");
    default:              assert(0 && "Invalid type to getClass!");
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

