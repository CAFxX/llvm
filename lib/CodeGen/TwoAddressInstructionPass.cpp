//===-- TwoAddressInstructionPass.cpp - Two-Address instruction pass ------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the TwoAddress instruction pass which is used
// by most register allocators. Two-Address instructions are rewritten
// from:
//
//     A = B op C
//
// to:
//
//     A = B
//     A op= C
//
// Note that if a register allocator chooses to use this pass, that it
// has to be capable of handling the non-SSA nature of these rewritten
// virtual registers.
//
// It is also worth noting that the duplicate operand of the two
// address instruction is removed.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "twoaddrinstr"
#include "llvm/CodeGen/Passes.h"
#include "llvm/Function.h"
#include "llvm/CodeGen/LiveVariables.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/SSARegMap.h"
#include "llvm/Target/MRegisterInfo.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/Debug.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/STLExtras.h"
using namespace llvm;

namespace {
  Statistic<> numTwoAddressInstrs("twoaddressinstruction",
                                  "Number of two-address instructions");

  struct TwoAddressInstructionPass : public MachineFunctionPass {
    virtual void getAnalysisUsage(AnalysisUsage &AU) const;

    /// runOnMachineFunction - pass entry point
    bool runOnMachineFunction(MachineFunction&);
  };

  RegisterPass<TwoAddressInstructionPass> 
  X("twoaddressinstruction", "Two-Address instruction pass");
};

const PassInfo *llvm::TwoAddressInstructionPassID = X.getPassInfo();

void TwoAddressInstructionPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addPreserved<LiveVariables>();
  AU.addPreservedID(PHIEliminationID);
  MachineFunctionPass::getAnalysisUsage(AU);
}

/// runOnMachineFunction - Reduce two-address instructions to two
/// operands.
///
bool TwoAddressInstructionPass::runOnMachineFunction(MachineFunction &MF) {
  DEBUG(std::cerr << "Machine Function\n");
  const TargetMachine &TM = MF.getTarget();
  const MRegisterInfo &MRI = *TM.getRegisterInfo();
  const TargetInstrInfo &TII = *TM.getInstrInfo();
  LiveVariables* LV = getAnalysisToUpdate<LiveVariables>();

  bool MadeChange = false;

  DEBUG(std::cerr << "********** REWRITING TWO-ADDR INSTRS **********\n");
  DEBUG(std::cerr << "********** Function: "
                  << MF.getFunction()->getName() << '\n');

  for (MachineFunction::iterator mbbi = MF.begin(), mbbe = MF.end();
       mbbi != mbbe; ++mbbi) {
    for (MachineBasicBlock::iterator mi = mbbi->begin(), me = mbbi->end();
         mi != me; ++mi) {
      unsigned opcode = mi->getOpcode();

      // ignore if it is not a two-address instruction
      if (!TII.isTwoAddrInstr(opcode))
        continue;

      ++numTwoAddressInstrs;
      DEBUG(std::cerr << '\t'; mi->print(std::cerr, &TM));
      assert(mi->getOperand(1).isRegister() && mi->getOperand(1).getReg() &&
             mi->getOperand(1).isUse() && "two address instruction invalid");

      // if the two operands are the same we just remove the use
      // and mark the def as def&use, otherwise we have to insert a copy.
      if (mi->getOperand(0).getReg() != mi->getOperand(1).getReg()) {
        // rewrite:
        //     a = b op c
        // to:
        //     a = b
        //     a = a op c
        unsigned regA = mi->getOperand(0).getReg();
        unsigned regB = mi->getOperand(1).getReg();

        assert(MRegisterInfo::isVirtualRegister(regA) &&
               MRegisterInfo::isVirtualRegister(regB) &&
               "cannot update physical register live information");

        // first make sure we do not have a use of a in the
        // instruction (a = b + a for example) because our
        // transformation will not work. This should never occur
        // because we are in SSA form.
#ifndef NDEBUG
        for (unsigned i = 1; i != mi->getNumOperands(); ++i)
          assert(!mi->getOperand(i).isRegister() ||
                 mi->getOperand(i).getReg() != regA);
#endif

        const TargetRegisterClass* rc = MF.getSSARegMap()->getRegClass(regA);
        MRI.copyRegToReg(*mbbi, mi, regA, regB, rc);

        MachineBasicBlock::iterator prevMi = prior(mi);
        DEBUG(std::cerr << "\t\tprepend:\t"; prevMi->print(std::cerr, &TM));

        if (LV) {
          // update live variables for regA
          LiveVariables::VarInfo& varInfo = LV->getVarInfo(regA);
          varInfo.DefInst = prevMi;

          // update live variables for regB
          if (LV->removeVirtualRegisterKilled(regB, mbbi, mi))
            LV->addVirtualRegisterKilled(regB, prevMi);

          if (LV->removeVirtualRegisterDead(regB, mbbi, mi))
            LV->addVirtualRegisterDead(regB, prevMi);
        }

        // replace all occurences of regB with regA
        for (unsigned i = 1, e = mi->getNumOperands(); i != e; ++i) {
          if (mi->getOperand(i).isRegister() && 
              mi->getOperand(i).getReg() == regB)
            mi->SetMachineOperandReg(i, regA);
        }
      }

      assert(mi->getOperand(0).isDef());
      mi->getOperand(0).setUse();
      mi->RemoveOperand(1);
      MadeChange = true;

      DEBUG(std::cerr << "\t\trewrite to:\t"; mi->print(std::cerr, &TM));
    }
  }

  return MadeChange;
}
