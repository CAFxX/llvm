//===-- PrologEpilogInserter.cpp - Insert Prolog/Epilog code in function --===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This pass is responsible for finalizing the functions frame layout, saving
// callee saved registers, and for emitting prolog & epilog code for the
// function.
//
// This pass must be run after register allocation.  After this pass is
// executed, it is illegal to construct MO_FrameIndex operands.
//
//===----------------------------------------------------------------------===//
//
// FIXME: The contents of this file should be merged with the target generic
// CodeGen/PrologEpilogInserter.cpp
//
//===----------------------------------------------------------------------===//

#include "PowerPC.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/MRegisterInfo.h"
#include "llvm/Target/TargetFrameInfo.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "Support/Debug.h"
using namespace llvm;

namespace {
  struct PPCPEI : public MachineFunctionPass {
    const char *getPassName() const {
      return "PowerPC Frame Finalization & Prolog/Epilog Insertion";
    }

    /// runOnMachineFunction - Insert prolog/epilog code and replace abstract
    /// frame indexes with appropriate references.
    ///
    bool runOnMachineFunction(MachineFunction &Fn) {
      RegsToSave.clear();
      StackSlots.clear();
      
      // Scan the function for modified caller saved registers and insert spill
      // code for any caller saved registers that are modified.  Also calculate
      // the MaxCallFrameSize and HasCalls variables for the function's frame
      // information and eliminates call frame pseudo instructions.
      calculateCallerSavedRegisters(Fn);

      // Calculate actual frame offsets for all of the abstract stack objects...
      calculateFrameObjectOffsets(Fn);

      // Add prolog and epilog code to the function.
      insertPrologEpilogCode(Fn);
      
      // Add register spills and fills before prolog and after epilog so that in
      // the event of a very large fixed size alloca, we don't have to do
      // anything weird.
      saveCallerSavedRegisters(Fn);

      // Replace all MO_FrameIndex operands with physical register references
      // and actual offsets.
      //
      replaceFrameIndices(Fn);
      return true;
    }

  private:
    std::vector<unsigned> RegsToSave;
    std::vector<int> StackSlots;

    void calculateCallerSavedRegisters(MachineFunction &Fn);
    void saveCallerSavedRegisters(MachineFunction &Fn);
    void calculateFrameObjectOffsets(MachineFunction &Fn);
    void replaceFrameIndices(MachineFunction &Fn);
    void insertPrologEpilogCode(MachineFunction &Fn);
  };
}


/// createPowerPCPEI - This function returns a pass that inserts
/// prolog and epilog code, and eliminates abstract frame references.
///
FunctionPass *llvm::createPowerPCPEI() { return new PPCPEI(); }


/// calculateCallerSavedRegisters - Scan the function for modified caller saved
/// registers.  Also calculate the MaxCallFrameSize and HasCalls variables for
/// the function's frame information and eliminates call frame pseudo
/// instructions.
///
void PPCPEI::calculateCallerSavedRegisters(MachineFunction &Fn) {
  const MRegisterInfo *RegInfo = Fn.getTarget().getRegisterInfo();
  const TargetFrameInfo &FrameInfo = *Fn.getTarget().getFrameInfo();

  // Get the callee saved register list...
  const unsigned *CSRegs = RegInfo->getCalleeSaveRegs();

  // Get the function call frame set-up and tear-down instruction opcode
  int FrameSetupOpcode   = RegInfo->getCallFrameSetupOpcode();
  int FrameDestroyOpcode = RegInfo->getCallFrameDestroyOpcode();

  // Early exit for targets which have no callee saved registers and no call
  // frame setup/destroy pseudo instructions.
  if ((CSRegs == 0 || CSRegs[0] == 0) &&
      FrameSetupOpcode == -1 && FrameDestroyOpcode == -1)
    return;

  // This bitset contains an entry for each physical register for the target...
  std::vector<bool> ModifiedRegs(RegInfo->getNumRegs());
  unsigned MaxCallFrameSize = 0;
  bool HasCalls = false;

  for (MachineFunction::iterator BB = Fn.begin(), E = Fn.end(); BB != E; ++BB)
    for (MachineBasicBlock::iterator I = BB->begin(); I != BB->end(); )
      if (I->getOpcode() == FrameSetupOpcode ||
          I->getOpcode() == FrameDestroyOpcode) {
	assert(I->getNumOperands() == 1 && "Call Frame Setup/Destroy Pseudo"
	       " instructions should have a single immediate argument!");
	unsigned Size = I->getOperand(0).getImmedValue();
	if (Size > MaxCallFrameSize) MaxCallFrameSize = Size;
	HasCalls = true;
	RegInfo->eliminateCallFramePseudoInstr(Fn, *BB, I++);
      } else {
	for (unsigned i = 0, e = I->getNumOperands(); i != e; ++i) {
	  MachineOperand &MO = I->getOperand(i);
	  if (MO.isRegister() && MO.isDef()) {
            assert(MRegisterInfo::isPhysicalRegister(MO.getReg()) &&
                   "Register allocation must be performed!");
	    ModifiedRegs[MO.getReg()] = true;         // Register is modified
          }
        }
	++I;
      }

  MachineFrameInfo *FFI = Fn.getFrameInfo();
  FFI->setHasCalls(HasCalls);
  FFI->setMaxCallFrameSize(MaxCallFrameSize);

  // Now figure out which *callee saved* registers are modified by the current
  // function, thus needing to be saved and restored in the prolog/epilog.
  //
  for (unsigned i = 0; CSRegs[i]; ++i) {
    unsigned Reg = CSRegs[i];
    if (ModifiedRegs[Reg]) {
      RegsToSave.push_back(Reg);  // If modified register...
    } else {
      for (const unsigned *AliasSet = RegInfo->getAliasSet(Reg);
           *AliasSet; ++AliasSet) {  // Check alias registers too...
        if (ModifiedRegs[*AliasSet]) {
          RegsToSave.push_back(Reg);
          break;
        }
      }
    }
  }
  
  // FIXME: should we sort the regs to save so that we always get the regs in
  // the correct order?

  // Now that we know which registers need to be saved and restored, allocate
  // stack slots for them.
  int Offset = 0;
  for (unsigned i = 0, e = RegsToSave.size(); i != e; ++i) {
    unsigned RegSize = RegInfo->getRegClass(RegsToSave[i])->getSize();
    int FrameIdx;
    
    if (RegsToSave[i] == PPC::LR) {
      FrameIdx = FFI->CreateFixedObject(RegSize, 8); // LR lives at +8
    } else {
      Offset -= RegSize;
      FrameIdx = FFI->CreateFixedObject(RegSize, Offset);
    }
    StackSlots.push_back(FrameIdx);
  }
}


/// saveCallerSavedRegisters -  Insert spill code for any caller saved registers
/// that are modified in the function.
///
void PPCPEI::saveCallerSavedRegisters(MachineFunction &Fn) {
  // Early exit if no caller saved registers are modified!
  if (RegsToSave.empty())
    return;   

  const MRegisterInfo *RegInfo = Fn.getTarget().getRegisterInfo();

  // Now that we have a stack slot for each register to be saved, insert spill
  // code into the entry block...
  MachineBasicBlock *MBB = Fn.begin();
  MachineBasicBlock::iterator I = MBB->begin();
  for (unsigned i = 0, e = RegsToSave.size(); i != e; ++i) {
    const TargetRegisterClass *RC = RegInfo->getRegClass(RegsToSave[i]);
    // Insert the spill to the stack frame...
    RegInfo->storeRegToStackSlot(*MBB, I, RegsToSave[i], StackSlots[i], RC);
  }

  // Add code to restore the callee-save registers in each exiting block.
  const TargetInstrInfo &TII = *Fn.getTarget().getInstrInfo();
  for (MachineFunction::iterator FI = Fn.begin(), E = Fn.end(); FI != E; ++FI) {
    // If last instruction is a return instruction, add an epilogue
    if (!FI->empty() && TII.isReturn(FI->back().getOpcode())) {
      MBB = FI;
      I = MBB->end(); --I;

      for (unsigned i = 0, e = RegsToSave.size(); i != e; ++i) {
	const TargetRegisterClass *RC = RegInfo->getRegClass(RegsToSave[i]);
	RegInfo->loadRegFromStackSlot(*MBB, I, RegsToSave[i],StackSlots[i], RC);
	--I;  // Insert in reverse order
      }
    }
  }
}


/// calculateFrameObjectOffsets - Calculate actual frame offsets for all of the
/// abstract stack objects...
///
void PPCPEI::calculateFrameObjectOffsets(MachineFunction &Fn) {
  const TargetFrameInfo &TFI = *Fn.getTarget().getFrameInfo();
  
  bool StackGrowsDown =
    TFI.getStackGrowthDirection() == TargetFrameInfo::StackGrowsDown;
 
  // Loop over all of the stack objects, assigning sequential addresses...
  MachineFrameInfo *FFI = Fn.getFrameInfo();

  unsigned StackAlignment = TFI.getStackAlignment();

  // Start at the beginning of the local area.
  // The Offset is the distance from the stack top in the direction
  // of stack growth -- so it's always positive.
  int Offset = TFI.getOffsetOfLocalArea();
  if (StackGrowsDown)
    Offset = -Offset;
  assert(Offset >= 0 
         && "Local area offset should be in direction of stack growth");

  // If there are fixed sized objects that are preallocated in the local area,
  // non-fixed objects can't be allocated right at the start of local area.
  // We currently don't support filling in holes in between fixed sized objects, 
  // so we adjust 'Offset' to point to the end of last fixed sized
  // preallocated object.
  for (int i = FFI->getObjectIndexBegin(); i != 0; ++i) {
    int FixedOff;
    if (StackGrowsDown) {
      // The maximum distance from the stack pointer is at lower address of
      // the object -- which is given by offset. For down growing stack
      // the offset is negative, so we negate the offset to get the distance.
      FixedOff = -FFI->getObjectOffset(i);
    } else {
      // The maximum distance from the start pointer is at the upper 
      // address of the object.
      FixedOff = FFI->getObjectOffset(i) + FFI->getObjectSize(i);
    }    
    if (FixedOff > Offset) Offset = FixedOff;            
  }

  for (unsigned i = 0, e = FFI->getObjectIndexEnd(); i != e; ++i) {
    // If stack grows down, we need to add size of find the lowest
    // address of the object.
    if (StackGrowsDown)
      Offset += FFI->getObjectSize(i);

    unsigned Align = FFI->getObjectAlignment(i);
    assert(Align <= StackAlignment && "Cannot align stack object to higher "
           "alignment boundary than the stack itself!");
    Offset = (Offset+Align-1)/Align*Align;   // Adjust to Alignment boundary...
    
    if (StackGrowsDown) {
      FFI->setObjectOffset(i, -Offset);        // Set the computed offset
    } else {
      FFI->setObjectOffset(i, Offset); 
      Offset += FFI->getObjectSize(i);
    }
  }

  // Set the final value of the stack pointer...
  FFI->setStackSize(Offset);
}


/// insertPrologEpilogCode - Scan the function for modified caller saved
/// registers, insert spill code for these caller saved registers, then add
/// prolog and epilog code to the function.
///
void PPCPEI::insertPrologEpilogCode(MachineFunction &Fn) {
  // Add prologue to the function...
  Fn.getTarget().getRegisterInfo()->emitPrologue(Fn);

  // Add epilogue to restore the callee-save registers in each exiting block
  const TargetInstrInfo &TII = *Fn.getTarget().getInstrInfo();
  for (MachineFunction::iterator I = Fn.begin(), E = Fn.end(); I != E; ++I) {
    // If last instruction is a return instruction, add an epilogue
    if (!I->empty() && TII.isReturn(I->back().getOpcode()))
      Fn.getTarget().getRegisterInfo()->emitEpilogue(Fn, *I);
  }
}


/// replaceFrameIndices - Replace all MO_FrameIndex operands with physical
/// register references and actual offsets.
///
void PPCPEI::replaceFrameIndices(MachineFunction &Fn) {
  if (!Fn.getFrameInfo()->hasStackObjects()) return; // Nothing to do?

  const TargetMachine &TM = Fn.getTarget();
  assert(TM.getRegisterInfo() && "TM::getRegisterInfo() must be implemented!");
  const MRegisterInfo &MRI = *TM.getRegisterInfo();

  for (MachineFunction::iterator BB = Fn.begin(), E = Fn.end(); BB != E; ++BB)
    for (MachineBasicBlock::iterator I = BB->begin(); I != BB->end(); ++I)
      for (unsigned i = 0, e = I->getNumOperands(); i != e; ++i)
	if (I->getOperand(i).isFrameIndex()) {
	  // If this instruction has a FrameIndex operand, we need to use that
	  // target machine register info object to eliminate it.
	  MRI.eliminateFrameIndex(Fn, I);
	  break;
	}
}
