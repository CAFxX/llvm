//===- MRegisterInfo.cpp - Target Register Information Implementation -----===//
//
// This file implements the MRegisterInfo interface.
//
//===----------------------------------------------------------------------===//

#include "llvm/Target/MRegisterInfo.h"

MRegisterInfo::MRegisterInfo(const MRegisterDesc *D, unsigned NR,
                             regclass_iterator RCB, regclass_iterator RCE)
  : Desc(D), NumRegs(NR), RegClassBegin(RCB), RegClassEnd(RCE) {
  assert(NumRegs < FirstVirtualRegister &&
         "Target has too many physical registers!");

  PhysRegClasses = new const TargetRegisterClass*[NumRegs];
  for (unsigned i = 0; i != NumRegs; ++i)
    PhysRegClasses[i] = 0;

  // Fill in the PhysRegClasses map
  for (MRegisterInfo::regclass_iterator I = regclass_begin(),
         E = regclass_end(); I != E; ++I)
    for (unsigned i=0; i < (*I)->getNumRegs(); ++i) {
      assert(PhysRegClasses[(*I)->getRegister(i)] == 0 &&
             "Register in more than one class?");
      PhysRegClasses[(*I)->getRegister(i)] = *I;
    }
}


MRegisterInfo::~MRegisterInfo() {
  delete[] PhysRegClasses;
}
