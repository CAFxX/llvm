//===-- RegAllocSimple.cpp - A simple generic register allocator --- ------===//
//
// This file implements a simple register allocator. *Very* simple.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/Target/MachineInstrInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "Support/Statistic.h"
#include <iostream>
#include <set>

#if 0
/// PhysRegClassMap - Construct a mapping of physical register numbers to their
/// register classes.
///
/// NOTE: This class will eventually be pulled out to somewhere shared.
///
class PhysRegClassMap {
  std::map<unsigned, const TargetRegisterClass*> PhysReg2RegClassMap;
public:
  PhysRegClassMap(const MRegisterInfo *RI) {
    for (MRegisterInfo::const_iterator I = RI->regclass_begin(),
           E = RI->regclass_end(); I != E; ++I)
      for (unsigned i=0; i < (*I)->getNumRegs(); ++i)
        PhysReg2RegClassMap[(*I)->getRegister(i)] = *I;
  }

  const TargetRegisterClass *operator[](unsigned Reg) {
    assert(PhysReg2RegClassMap[Reg] && "Register is not a known physreg!");
    return PhysReg2RegClassMap[Reg];
  }

  const TargetRegisterClass *get(unsigned Reg) { return operator[](Reg); }
};
#endif


namespace {
  Statistic<> NumSpilled ("ra-simple", "Number of registers spilled");
  Statistic<> NumReloaded("ra-simple", "Number of registers reloaded");

  class RegAllocSimple : public FunctionPass {
    TargetMachine &TM;
    MachineFunction *MF;
    const MRegisterInfo *RegInfo;
    unsigned NumBytesAllocated;
    
    // Maps SSA Regs => offsets on the stack where these values are stored
    std::map<unsigned, unsigned> VirtReg2OffsetMap;

    // RegsUsed - Keep track of what registers are currently in use.
    std::set<unsigned> RegsUsed;

    // RegClassIdx - Maps RegClass => which index we can take a register
    // from. Since this is a simple register allocator, when we need a register
    // of a certain class, we just take the next available one.
    std::map<const TargetRegisterClass*, unsigned> RegClassIdx;

  public:

    RegAllocSimple(TargetMachine &tm)
      : TM(tm), RegInfo(tm.getRegisterInfo()) {
      RegsUsed.insert(RegInfo->getFramePointer());
      RegsUsed.insert(RegInfo->getStackPointer());

      cleanupAfterFunction();
    }

    bool runOnFunction(Function &Fn) {
      return runOnMachineFunction(MachineFunction::get(&Fn));
    }

    virtual const char *getPassName() const {
      return "Simple Register Allocator";
    }

  private:
    /// runOnMachineFunction - Register allocate the whole function
    bool runOnMachineFunction(MachineFunction &Fn);

    /// AllocateBasicBlock - Register allocate the specified basic block.
    void AllocateBasicBlock(MachineBasicBlock &MBB);

    /// EliminatePHINodes - Eliminate phi nodes by inserting copy instructions
    /// in predecessor basic blocks.
    void EliminatePHINodes(MachineBasicBlock &MBB);


    /// getStackSpaceFor - This returns the offset of the specified virtual
    /// register on the stack, allocating space if neccesary.
    unsigned getStackSpaceFor(unsigned VirtReg, 
                              const TargetRegisterClass *regClass);

    /// Given a virtual register, return a compatible physical register that is
    /// currently unused.
    ///
    /// Side effect: marks that register as being used until manually cleared
    ///
    unsigned getFreeReg(unsigned virtualReg);

    /// Returns all `borrowed' registers back to the free pool
    void clearAllRegs() {
      RegClassIdx.clear();
    }

    /// Invalidates any references, real or implicit, to physical registers
    ///
    void invalidatePhysRegs(const MachineInstr *MI) {
      unsigned Opcode = MI->getOpcode();
      const MachineInstrDescriptor &Desc = TM.getInstrInfo().get(Opcode);
      const unsigned *regs = Desc.ImplicitUses;
      while (*regs)
        RegsUsed.insert(*regs++);

      regs = Desc.ImplicitDefs;
      while (*regs)
        RegsUsed.insert(*regs++);
    }

    void cleanupAfterFunction() {
      VirtReg2OffsetMap.clear();
      NumBytesAllocated = 4;   // FIXME: This is X86 specific
    }

    /// Moves value from memory into that register
    MachineBasicBlock::iterator
    moveUseToReg(MachineBasicBlock &MBB, MachineBasicBlock::iterator I,
                 unsigned VirtReg, unsigned &PhysReg);

    /// Saves reg value on the stack (maps virtual register to stack value)
    MachineBasicBlock::iterator
    saveVirtRegToStack(MachineBasicBlock &MBB, MachineBasicBlock::iterator I,
                       unsigned VirtReg, unsigned PhysReg);
  };

}

/// getStackSpaceFor - This allocates space for the specified virtual
/// register to be held on the stack.
unsigned RegAllocSimple::getStackSpaceFor(unsigned VirtReg,
                                          const TargetRegisterClass *regClass) {
  // Find the location VirtReg would belong...
  std::map<unsigned, unsigned>::iterator I =
    VirtReg2OffsetMap.lower_bound(VirtReg);

  if (I != VirtReg2OffsetMap.end() && I->first == VirtReg)
    return I->second;          // Already has space allocated?

  unsigned RegSize = regClass->getDataSize();

  // Align NumBytesAllocated.  We should be using TargetData alignment stuff
  // to determine this, but we don't know the LLVM type associated with the
  // virtual register.  Instead, just align to a multiple of the size for now.
  NumBytesAllocated += RegSize-1;
  NumBytesAllocated = NumBytesAllocated/RegSize*RegSize;
  
  // Assign the slot...
  VirtReg2OffsetMap.insert(I, std::make_pair(VirtReg, NumBytesAllocated));
  
  // Reserve the space!
  NumBytesAllocated += RegSize;
  return NumBytesAllocated-RegSize;
}

unsigned RegAllocSimple::getFreeReg(unsigned virtualReg) {
  const TargetRegisterClass* regClass = MF->getRegClass(virtualReg);
  
  unsigned regIdx = RegClassIdx[regClass]++;
  assert(regIdx < regClass->getNumRegs() && "Not enough registers!");
  unsigned physReg = regClass->getRegister(regIdx);

  if (RegsUsed.find(physReg) == RegsUsed.end())
    return physReg;
  else
    return getFreeReg(virtualReg);
}

MachineBasicBlock::iterator
RegAllocSimple::moveUseToReg (MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator I,
                              unsigned VirtReg, unsigned &PhysReg)
{
  const TargetRegisterClass* regClass = MF->getRegClass(VirtReg);
  unsigned stackOffset = getStackSpaceFor(VirtReg, regClass);
  PhysReg = getFreeReg(VirtReg);

  // Add move instruction(s)
  ++NumReloaded;
  return RegInfo->loadRegOffset2Reg(MBB, I, PhysReg,
                                    RegInfo->getFramePointer(),
                                    -stackOffset, regClass->getDataSize());
}

MachineBasicBlock::iterator
RegAllocSimple::saveVirtRegToStack (MachineBasicBlock &MBB,
                                    MachineBasicBlock::iterator I,
                                    unsigned VirtReg, unsigned PhysReg)
{
  const TargetRegisterClass* regClass = MF->getRegClass(VirtReg);
  unsigned stackOffset = getStackSpaceFor(VirtReg, regClass);

  // Add move instruction(s)
  ++NumSpilled;
  return RegInfo->storeReg2RegOffset(MBB, I, PhysReg,
                                     RegInfo->getFramePointer(),
                                     -stackOffset, regClass->getDataSize());
}


/// EliminatePHINodes - Eliminate phi nodes by inserting copy instructions in
/// predecessor basic blocks.
void RegAllocSimple::EliminatePHINodes(MachineBasicBlock &MBB) {
  const MachineInstrInfo &MII = TM.getInstrInfo();

  while (MBB.front()->getOpcode() == MachineInstrInfo::PHI) {
    MachineInstr *MI = MBB.front();
    // Unlink the PHI node from the basic block... but don't delete the PHI yet
    MBB.erase(MBB.begin());
    
    DEBUG(std::cerr << "num invalid regs: " << RegsUsed.size() << "\n");
    DEBUG(std::cerr << "num ops: " << MI->getNumOperands() << "\n");
    assert(MI->getOperand(0).isVirtualRegister() &&
           "PHI node doesn't write virt reg?");

    // a preliminary pass that will invalidate any registers that
    // are used by the instruction (including implicit uses)
    invalidatePhysRegs(MI);
    
    // Allocate a physical reg to hold this temporary.
    //
    unsigned virtualReg = MI->getOperand(0).getAllocatedRegNum();
    unsigned physReg = getFreeReg(virtualReg);
    
    // Find the register class of the target register: should be the
    // same as the values we're trying to store there
    const TargetRegisterClass* regClass = MF->getRegClass(virtualReg);
    assert(regClass && "Target register class not found!");
    unsigned dataSize = regClass->getDataSize();

    for (int i = MI->getNumOperands() - 1; i >= 2; i-=2) {
      MachineOperand &opVal = MI->getOperand(i-1);
      
      // Get the MachineBasicBlock equivalent of the BasicBlock that is the
      // source path the phi
      MachineBasicBlock &opBlock = *MI->getOperand(i).getMachineBasicBlock();

      // Check to make sure we haven't already emitted the copy for this block.
      // This can happen because PHI nodes may have multiple entries for the
      // same basic block.  It doesn't matter which entry we use though, because
      // all incoming values are guaranteed to be the same for a particular bb.
      //
      // Note that this is N^2 in the number of phi node entries, but since the
      // # of entries is tiny, this is not a problem.
      //
      bool HaveNotEmitted = true;
      for (int op = MI->getNumOperands() - 1; op != i; op -= 2)
        if (&opBlock == MI->getOperand(op).getMachineBasicBlock()) {
          HaveNotEmitted = false;
          break;
        }

      if (HaveNotEmitted) {
        MachineBasicBlock::iterator opI = opBlock.end();
        MachineInstr *opMI = *--opI;
        
        // must backtrack over ALL the branches in the previous block
        while (MII.isBranch(opMI->getOpcode()) && opI != opBlock.begin())
          opMI = *--opI;
        
        // move back to the first branch instruction so new instructions
        // are inserted right in front of it and not in front of a non-branch
        if (!MII.isBranch(opMI->getOpcode()))
          ++opI;
        
        // Retrieve the constant value from this op, move it to target
        // register of the phi
        if (opVal.isImmediate()) {
          opI = RegInfo->moveImm2Reg(opBlock, opI, physReg,
                                     (unsigned) opVal.getImmedValue(),
                                     dataSize);
        } else {
          // Allocate a physical register and add a move in the BB
          unsigned opVirtualReg = opVal.getAllocatedRegNum();
          unsigned opPhysReg;
          opI = moveUseToReg(opBlock, opI, opVirtualReg, physReg);
          
        }

        // Save that register value to the stack of the TARGET REG
        saveVirtRegToStack(opBlock, opI, virtualReg, physReg);
      }

      // make regs available to other instructions
      clearAllRegs();
    }
    
    // really delete the PHI instruction now!
    delete MI;
  }
}


void RegAllocSimple::AllocateBasicBlock(MachineBasicBlock &MBB) {
  // Handle PHI instructions specially: add moves to each pred block
  EliminatePHINodes(MBB);
  
  // loop over each instruction
  for (MachineBasicBlock::iterator I = MBB.begin(); I != MBB.end(); ++I) {
    // Made to combat the incorrect allocation of r2 = add r1, r1
    std::map<unsigned, unsigned> Virt2PhysRegMap;

    MachineInstr *MI = *I;
    
    // a preliminary pass that will invalidate any registers that
    // are used by the instruction (including implicit uses)
    invalidatePhysRegs(MI);
    
    // Loop over uses, move from memory into registers
    for (int i = MI->getNumOperands() - 1; i >= 0; --i) {
      MachineOperand &op = MI->getOperand(i);
      
      if (op.isVirtualRegister()) {
        unsigned virtualReg = (unsigned) op.getAllocatedRegNum();
        DEBUG(std::cerr << "op: " << op << "\n");
        DEBUG(std::cerr << "\t inst[" << i << "]: ";
              MI->print(std::cerr, TM));
        
        // make sure the same virtual register maps to the same physical
        // register in any given instruction
        unsigned physReg = Virt2PhysRegMap[virtualReg];
        if (physReg == 0) {
          if (op.opIsDef()) {
            if (TM.getInstrInfo().isTwoAddrInstr(MI->getOpcode()) && i == 0) {
              // must be same register number as the first operand
              // This maps a = b + c into b += c, and saves b into a's spot
              assert(MI->getOperand(1).isRegister()  &&
                     MI->getOperand(1).getAllocatedRegNum() &&
                     MI->getOperand(1).opIsUse() &&
                     "Two address instruction invalid!");

              physReg = MI->getOperand(1).getAllocatedRegNum();
            } else {
              physReg = getFreeReg(virtualReg);
            }
            I = --saveVirtRegToStack(MBB, ++I, virtualReg, physReg);
          } else {
            I = moveUseToReg(MBB, I, virtualReg, physReg);
          }
          Virt2PhysRegMap[virtualReg] = physReg;
        }
        MI->SetMachineOperandReg(i, physReg);
        DEBUG(std::cerr << "virt: " << virtualReg << 
              ", phys: " << op.getAllocatedRegNum() << "\n");
      }
    }
    clearAllRegs();
  }
}

/// runOnMachineFunction - Register allocate the whole function
///
bool RegAllocSimple::runOnMachineFunction(MachineFunction &Fn) {
  DEBUG(std::cerr << "Machine Function " << "\n");
  MF = &Fn;

  // Loop over all of the basic blocks, eliminating virtual register references
  for (MachineFunction::iterator MBB = Fn.begin(), MBBe = Fn.end();
       MBB != MBBe; ++MBB)
    AllocateBasicBlock(*MBB);

  // Add prologue to the function...
  RegInfo->emitPrologue(Fn, NumBytesAllocated);

  const MachineInstrInfo &MII = TM.getInstrInfo();

  // Add epilogue to restore the callee-save registers in each exiting block
  for (MachineFunction::iterator MBB = Fn.begin(), MBBe = Fn.end();
       MBB != MBBe; ++MBB) {
    // If last instruction is a return instruction, add an epilogue
    if (MII.isReturn(MBB->back()->getOpcode()))
      RegInfo->emitEpilogue(*MBB, NumBytesAllocated);
  }

  cleanupAfterFunction();
  return true;
}

Pass *createSimpleX86RegisterAllocator(TargetMachine &TM) {
  return new RegAllocSimple(TM);
}
