//===-- SparcV9CodeEmitter.cpp -  --------===//
//
//
//===----------------------------------------------------------------------===//

#include "llvm/PassManager.h"
#include "llvm/CodeGen/MachineCodeEmitter.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/Target/TargetMachine.h"
#include "SparcInternals.h"
#include "SparcV9CodeEmitter.h"

MachineCodeEmitter * SparcV9CodeEmitter::MCE = 0;
TargetMachine * SparcV9CodeEmitter::TM = 0;

bool UltraSparc::addPassesToEmitMachineCode(PassManager &PM,
                                            MachineCodeEmitter &MCE) {
  //PM.add(new SparcV9CodeEmitter(MCE));
  //MachineCodeEmitter *M = MachineCodeEmitter::createDebugMachineCodeEmitter();
  MachineCodeEmitter *M = 
    MachineCodeEmitter::createFilePrinterMachineCodeEmitter(MCE);
  PM.add(new SparcV9CodeEmitter(this, *M));
  PM.add(createMachineCodeDestructionPass()); // Free stuff no longer needed
  return false;
}

void SparcV9CodeEmitter::emitConstant(unsigned Val, unsigned Size) {
  // Output the constant in big endian byte order...
  unsigned byteVal;
  for (int i = Size-1; i >= 0; --i) {
    byteVal = Val >> 8*i;
    MCE->emitByte(byteVal & 255);
  }
}

unsigned getRealRegNum(unsigned fakeReg, unsigned regClass) {
  switch (regClass) {
  case UltraSparcRegInfo::IntRegType: {
    // Sparc manual, p31
    static const unsigned IntRegMap[] = {
      // "o0", "o1", "o2", "o3", "o4", "o5",       "o7",
      8, 9, 10, 11, 12, 13, 15,
      // "l0", "l1", "l2", "l3", "l4", "l5", "l6", "l7",
      16, 17, 18, 19, 20, 21, 22, 23,
      // "i0", "i1", "i2", "i3", "i4", "i5",  
      24, 25, 26, 27, 28, 29,
      // "i6", "i7",
      30, 31,
      // "g0", "g1", "g2", "g3", "g4", "g5",  "g6", "g7", 
      0, 1, 2, 3, 4, 5, 6, 7,
      // "o6"
      14
    }; 
 
    return IntRegMap[fakeReg];
    break;
  }
  case UltraSparcRegInfo::FPSingleRegType: {
    return fakeReg;
  }
  case UltraSparcRegInfo::FPDoubleRegType: {
    return fakeReg;
  }
  case UltraSparcRegInfo::FloatCCRegType: {
    return fakeReg;

  }
  case UltraSparcRegInfo::IntCCRegType: {
    return fakeReg;
  }
  default:
    assert(0 && "Invalid unified register number in getRegType");
    return fakeReg;
  }
}

int64_t SparcV9CodeEmitter::getMachineOpValue(MachineInstr &MI,
                                              MachineOperand &MO) {
  int64_t rv = 0; // Return value; defaults to 0 for unhandled cases
                  // or things that get fixed up later by the JIT.

  if (MO.isPhysicalRegister()) {
    // This is necessary because the Sparc doesn't actually lay out registers
    // in the real fashion -- it skips those that it chooses not to allocate,
    // i.e. those that are the SP, etc.
    unsigned fakeReg = MO.getReg(), realReg, regClass, regType;
    regType = TM->getRegInfo().getRegType(fakeReg);
    // At least map fakeReg into its class
    fakeReg = TM->getRegInfo().getClassRegNum(fakeReg, regClass);
    // Find the real register number for use in an instruction
    realReg = getRealRegNum(fakeReg, regClass);
    std::cerr << "Reg[" << fakeReg << "] = " << realReg << "\n";
    rv = realReg;
  } else if (MO.isImmediate()) {
    rv = MO.getImmedValue();
  } else if (MO.isPCRelativeDisp()) {
    std::cerr << "Saving reference to BB (PCRelDisp)\n";
    MCE->saveBBreference((BasicBlock*)MO.getVRegValue(), MI);
  } else if (MO.isMachineBasicBlock()) {
    std::cerr << "Saving reference to BB (MBB)\n";
    MCE->saveBBreference(MO.getMachineBasicBlock()->getBasicBlock(), MI);
  } else if (MO.isFrameIndex()) {
    std::cerr << "ERROR: Frame index unhandled.\n";
  } else if (MO.isConstantPoolIndex()) {
    std::cerr << "ERROR: Constant Pool index unhandled.\n";
  } else if (MO.isGlobalAddress()) {
    std::cerr << "ERROR: Global addr unhandled.\n";
  } else if (MO.isExternalSymbol()) {
    std::cerr << "ERROR: External symbol unhandled.\n";
  } else {
    std::cerr << "ERROR: Unknown type of MachineOperand: " << MO << "\n";
  }

  // Finally, deal with the various bitfield-extracting functions that
  // are used in SPARC assembly. (Some of these make no sense in combination
  // with some of the above; we'll trust that the instruction selector
  // will not produce nonsense, and not check for valid combinations here.)
  if (MO.opLoBits32()) {          // %lo(val)
    return rv & 0x03ff;
  } else if (MO.opHiBits32()) {   // %lm(val)
    return (rv >> 10) & 0x03fffff;
  } else if (MO.opLoBits64()) {   // %hm(val)
    return (rv >> 32) & 0x03ff;
  } else if (MO.opHiBits64()) {   // %hh(val)
    return rv >> 42;
  } else {                        // (unadorned) val
    return rv;
  }
}

unsigned SparcV9CodeEmitter::getValueBit(int64_t Val, unsigned bit) {
  Val >>= bit;
  return (Val & 1);
}


bool SparcV9CodeEmitter::runOnMachineFunction(MachineFunction &MF) {
  MCE->startFunction(MF);
  MCE->emitConstantPool(MF.getConstantPool());
  for (MachineFunction::iterator I = MF.begin(), E = MF.end(); I != E; ++I)
    emitBasicBlock(*I);
  MCE->finishFunction(MF);
  return false;
}

void SparcV9CodeEmitter::emitBasicBlock(MachineBasicBlock &MBB) {
  currBB = MBB.getBasicBlock();
  MCE->startBasicBlock(MBB);
  for (MachineBasicBlock::iterator I = MBB.begin(), E = MBB.end(); I != E; ++I)
    emitInstruction(**I);
}

void SparcV9CodeEmitter::emitInstruction(MachineInstr &MI) {
  emitConstant(getBinaryCodeForInstr(MI), 4);
}

#include "SparcV9CodeEmitter.inc"
