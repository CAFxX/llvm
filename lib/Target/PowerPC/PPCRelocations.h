//===- PPC32Relocations.h - PPC32 Code Relocations --------------*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file defines the PowerPC 32-bit target-specific relocation types.
//
//===----------------------------------------------------------------------===//

#ifndef PPC32RELOCATIONS_H
#define PPC32RELOCATIONS_H

#include "llvm/CodeGen/MachineRelocation.h"

namespace llvm {
  namespace PPC {
    enum RelocationType {
      // reloc_pcrel_bx - PC relative relocation, for the b or bl instructions.
      reloc_pcrel_bx,

      // reloc_absolute_loadhi - Absolute relocation, for the loadhi instruction
      // (which is really addis).  Add the high 16-bits of the specified global
      // address into the immediate field of the addis.
      reloc_absolute_loadhi,

      // reloc_absolute_la - Absolute relocation, for the la instruction (which
      // is really an addi).  Add the low 16-bits of teh specified global
      // address into the immediate field of the addi.
      reloc_absolute_la,
    };
  }
}

#endif
