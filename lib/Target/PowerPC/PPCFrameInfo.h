//===-- PowerPCFrameInfo.h - Define TargetFrameInfo for PowerPC -*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
//
//----------------------------------------------------------------------------

#ifndef POWERPC_FRAMEINFO_H
#define POWERPC_FRAMEINFO_H

#include "PowerPC.h"
#include "llvm/Target/TargetFrameInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/MRegisterInfo.h"
#include <map>

namespace llvm {

class PowerPCFrameInfo: public TargetFrameInfo {
  const TargetMachine &TM;
  std::pair<unsigned, int> LR[1];
  
public:

  PowerPCFrameInfo(const TargetMachine &inTM)
    : TargetFrameInfo(TargetFrameInfo::StackGrowsDown, 16, 0), TM(inTM) {
    LR[0].first = PPC::LR;
    LR[0].second = 8;
  }

  const std::pair<unsigned, int> *
  getCalleeSaveSpillSlots(unsigned &NumEntries) const {
    NumEntries = 1;
    return &LR[0];
  }
};

} // End llvm namespace

#endif
