//===- SparcV9JITInfo.h - SparcV9 Target JIT interface ----------*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file contains the SparcV9 implementation of the TargetJITInfo class,
// which makes target-specific hooks available to the target-independent
// LLVM JIT compiler.
//
//===----------------------------------------------------------------------===//

#ifndef SPARCV9JITINFO_H
#define SPARCV9JITINFO_H

#include "llvm/Target/TargetJITInfo.h"

namespace llvm {
  class TargetMachine;

  class SparcV9JITInfo : public TargetJITInfo {
    TargetMachine &TM;
  public:
    SparcV9JITInfo(TargetMachine &tm) : TM(tm) {}

    /// addPassesToJITCompile - Add passes to the specified pass manager to
    /// implement a fast dynamic compiler for this target.  Return true if this
    /// is not supported for this target.
    ///
    virtual void addPassesToJITCompile(FunctionPassManager &PM);
    
    /// replaceMachineCodeForFunction - Make it so that calling the function
    /// whose machine code is at OLD turns into a call to NEW, perhaps by
    /// overwriting OLD with a branch to NEW.  This is used for self-modifying
    /// code.
    ///
    virtual void replaceMachineCodeForFunction (void *Old, void *New);
  };
}

#endif
