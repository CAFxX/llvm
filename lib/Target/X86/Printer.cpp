//===-- X86/Printer.cpp - Convert X86 LLVM code to Intel assembly ---------===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file contains a printer that converts from our internal representation
// of machine-dependent LLVM code to Intel-format assembly language. This
// printer is the output mechanism used by `llc' and `lli -print-machineinstrs'
// on X86.
//
//===----------------------------------------------------------------------===//

#include "X86.h"
#include "X86InstrInfo.h"
#include "X86TargetMachine.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Module.h"
#include "llvm/Assembly/Writer.h"
#include "llvm/CodeGen/MachineCodeEmitter.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/Mangler.h"
#include "Support/Statistic.h"
#include "Support/StringExtras.h"
#include "Support/CommandLine.h"
using namespace llvm;

namespace {
  Statistic<> EmittedInsts("asm-printer", "Number of machine instrs printed");

  // FIXME: This should be automatically picked up by autoconf from the C
  // frontend
  cl::opt<bool> EmitCygwin("enable-cygwin-compatible-output", cl::Hidden,
         cl::desc("Emit X86 assembly code suitable for consumption by cygwin"));

  struct GasBugWorkaroundEmitter : public MachineCodeEmitter {
      GasBugWorkaroundEmitter(std::ostream& o) 
          : O(o), OldFlags(O.flags()), firstByte(true) {
          O << std::hex;
      }

      ~GasBugWorkaroundEmitter() {
          O.flags(OldFlags);
          O << "\t# ";
      }

      virtual void emitByte(unsigned char B) {
          if (!firstByte) O << "\n\t";
          firstByte = false;
          O << ".byte 0x" << (unsigned) B;
      }

      // These should never be called
      virtual void emitWord(unsigned W) { assert(0); }
      virtual uint64_t getGlobalValueAddress(GlobalValue *V) { abort(); }
      virtual uint64_t getGlobalValueAddress(const std::string &Name) { abort(); }
      virtual uint64_t getConstantPoolEntryAddress(unsigned Index) { abort(); }
      virtual uint64_t getCurrentPCValue() { abort(); }
      virtual uint64_t forceCompilationOf(Function *F) { abort(); }

  private:
      std::ostream& O;
      std::ios::fmtflags OldFlags;
      bool firstByte;
  };

  struct Printer : public MachineFunctionPass {
    /// Output stream on which we're printing assembly code.
    ///
    std::ostream &O;

    /// Target machine description which we query for reg. names, data
    /// layout, etc.
    ///
    TargetMachine &TM;

    /// Name-mangler for global names.
    ///
    Mangler *Mang;

    Printer(std::ostream &o, TargetMachine &tm) : O(o), TM(tm) { }

    /// Cache of mangled name for current function. This is
    /// recalculated at the beginning of each call to
    /// runOnMachineFunction().
    ///
    std::string CurrentFnName;

    virtual const char *getPassName() const {
      return "X86 Assembly Printer";
    }

    void printImplUsesBefore(const TargetInstrDescriptor &Desc);
    bool printImplDefsBefore(const TargetInstrDescriptor &Desc);
    bool printImplUsesAfter(const TargetInstrDescriptor &Desc, const bool LC);
    bool printImplDefsAfter(const TargetInstrDescriptor &Desc, const bool LC);
    void printMachineInstruction(const MachineInstr *MI);
    void printOp(const MachineOperand &MO,
		 bool elideOffsetKeyword = false);
    void printMemReference(const MachineInstr *MI, unsigned Op);
    void printConstantPool(MachineConstantPool *MCP);
    bool runOnMachineFunction(MachineFunction &F);    
    bool doInitialization(Module &M);
    bool doFinalization(Module &M);
    void emitGlobalConstant(const Constant* CV);
    void emitConstantValueOnly(const Constant *CV);
  };
} // end of anonymous namespace

/// createX86CodePrinterPass - Returns a pass that prints the X86
/// assembly code for a MachineFunction to the given output stream,
/// using the given target machine description.  This should work
/// regardless of whether the function is in SSA form.
///
FunctionPass *llvm::createX86CodePrinterPass(std::ostream &o,TargetMachine &tm){
  return new Printer(o, tm);
}

/// toOctal - Convert the low order bits of X into an octal digit.
///
static inline char toOctal(int X) {
  return (X&7)+'0';
}

/// getAsCString - Return the specified array as a C compatible
/// string, only if the predicate isStringCompatible is true.
///
static void printAsCString(std::ostream &O, const ConstantArray *CVA) {
  assert(CVA->isString() && "Array is not string compatible!");

  O << "\"";
  for (unsigned i = 0; i != CVA->getNumOperands(); ++i) {
    unsigned char C = cast<ConstantInt>(CVA->getOperand(i))->getRawValue();

    if (C == '"') {
      O << "\\\"";
    } else if (C == '\\') {
      O << "\\\\";
    } else if (isprint(C)) {
      O << C;
    } else {
      switch(C) {
      case '\b': O << "\\b"; break;
      case '\f': O << "\\f"; break;
      case '\n': O << "\\n"; break;
      case '\r': O << "\\r"; break;
      case '\t': O << "\\t"; break;
      default:
        O << '\\';
        O << toOctal(C >> 6);
        O << toOctal(C >> 3);
        O << toOctal(C >> 0);
        break;
      }
    }
  }
  O << "\"";
}

// Print out the specified constant, without a storage class.  Only the
// constants valid in constant expressions can occur here.
void Printer::emitConstantValueOnly(const Constant *CV) {
  if (CV->isNullValue())
    O << "0";
  else if (const ConstantBool *CB = dyn_cast<ConstantBool>(CV)) {
    assert(CB == ConstantBool::True);
    O << "1";
  } else if (const ConstantSInt *CI = dyn_cast<ConstantSInt>(CV))
    if (((CI->getValue() << 32) >> 32) == CI->getValue())
      O << CI->getValue();
    else
      O << (unsigned long long)CI->getValue();
  else if (const ConstantUInt *CI = dyn_cast<ConstantUInt>(CV))
    O << CI->getValue();
  else if (const ConstantPointerRef *CPR = dyn_cast<ConstantPointerRef>(CV))
    // This is a constant address for a global variable or function.  Use the
    // name of the variable or function as the address value.
    O << Mang->getValueName(CPR->getValue());
  else if (const ConstantExpr *CE = dyn_cast<ConstantExpr>(CV)) {
    const TargetData &TD = TM.getTargetData();
    switch(CE->getOpcode()) {
    case Instruction::GetElementPtr: {
      // generate a symbolic expression for the byte address
      const Constant *ptrVal = CE->getOperand(0);
      std::vector<Value*> idxVec(CE->op_begin()+1, CE->op_end());
      if (unsigned Offset = TD.getIndexedOffset(ptrVal->getType(), idxVec)) {
        O << "(";
        emitConstantValueOnly(ptrVal);
        O << ") + " << Offset;
      } else {
        emitConstantValueOnly(ptrVal);
      }
      break;
    }
    case Instruction::Cast: {
      // Support only non-converting or widening casts for now, that is, ones
      // that do not involve a change in value.  This assertion is really gross,
      // and may not even be a complete check.
      Constant *Op = CE->getOperand(0);
      const Type *OpTy = Op->getType(), *Ty = CE->getType();

      // Remember, kids, pointers on x86 can be losslessly converted back and
      // forth into 32-bit or wider integers, regardless of signedness. :-P
      assert(((isa<PointerType>(OpTy)
               && (Ty == Type::LongTy || Ty == Type::ULongTy
                   || Ty == Type::IntTy || Ty == Type::UIntTy))
              || (isa<PointerType>(Ty)
                  && (OpTy == Type::LongTy || OpTy == Type::ULongTy
                      || OpTy == Type::IntTy || OpTy == Type::UIntTy))
              || (((TD.getTypeSize(Ty) >= TD.getTypeSize(OpTy))
                   && OpTy->isLosslesslyConvertibleTo(Ty))))
             && "FIXME: Don't yet support this kind of constant cast expr");
      O << "(";
      emitConstantValueOnly(Op);
      O << ")";
      break;
    }
    case Instruction::Add:
      O << "(";
      emitConstantValueOnly(CE->getOperand(0));
      O << ") + (";
      emitConstantValueOnly(CE->getOperand(1));
      O << ")";
      break;
    default:
      assert(0 && "Unsupported operator!");
    }
  } else {
    assert(0 && "Unknown constant value!");
  }
}

// Print a constant value or values, with the appropriate storage class as a
// prefix.
void Printer::emitGlobalConstant(const Constant *CV) {  
  const TargetData &TD = TM.getTargetData();

  if (CV->isNullValue()) {
    O << "\t.zero\t " << TD.getTypeSize(CV->getType()) << "\n";      
    return;
  } else if (const ConstantArray *CVA = dyn_cast<ConstantArray>(CV)) {
    if (CVA->isString()) {
      O << "\t.ascii\t";
      printAsCString(O, CVA);
      O << "\n";
    } else { // Not a string.  Print the values in successive locations
      const std::vector<Use> &constValues = CVA->getValues();
      for (unsigned i=0; i < constValues.size(); i++)
        emitGlobalConstant(cast<Constant>(constValues[i].get()));
    }
    return;
  } else if (const ConstantStruct *CVS = dyn_cast<ConstantStruct>(CV)) {
    // Print the fields in successive locations. Pad to align if needed!
    const StructLayout *cvsLayout = TD.getStructLayout(CVS->getType());
    const std::vector<Use>& constValues = CVS->getValues();
    unsigned sizeSoFar = 0;
    for (unsigned i=0, N = constValues.size(); i < N; i++) {
      const Constant* field = cast<Constant>(constValues[i].get());

      // Check if padding is needed and insert one or more 0s.
      unsigned fieldSize = TD.getTypeSize(field->getType());
      unsigned padSize = ((i == N-1? cvsLayout->StructSize
                           : cvsLayout->MemberOffsets[i+1])
                          - cvsLayout->MemberOffsets[i]) - fieldSize;
      sizeSoFar += fieldSize + padSize;

      // Now print the actual field value
      emitGlobalConstant(field);

      // Insert the field padding unless it's zero bytes...
      if (padSize)
        O << "\t.zero\t " << padSize << "\n";      
    }
    assert(sizeSoFar == cvsLayout->StructSize &&
           "Layout of constant struct may be incorrect!");
    return;
  } else if (const ConstantFP *CFP = dyn_cast<ConstantFP>(CV)) {
    // FP Constants are printed as integer constants to avoid losing
    // precision...
    double Val = CFP->getValue();
    switch (CFP->getType()->getPrimitiveID()) {
    default: assert(0 && "Unknown floating point type!");
    case Type::FloatTyID: {
      union FU {                            // Abide by C TBAA rules
        float FVal;
        unsigned UVal;
      } U;
      U.FVal = Val;
      O << ".long\t" << U.UVal << "\t# float " << Val << "\n";
      return;
    }
    case Type::DoubleTyID: {
      union DU {                            // Abide by C TBAA rules
        double FVal;
        uint64_t UVal;
      } U;
      U.FVal = Val;
      O << ".quad\t" << U.UVal << "\t# double " << Val << "\n";
      return;
    }
    }
  }

  const Type *type = CV->getType();
  O << "\t";
  switch (type->getPrimitiveID()) {
  case Type::BoolTyID: case Type::UByteTyID: case Type::SByteTyID:
    O << ".byte";
    break;
  case Type::UShortTyID: case Type::ShortTyID:
    O << ".word";
    break;
  case Type::FloatTyID: case Type::PointerTyID:
  case Type::UIntTyID: case Type::IntTyID:
    O << ".long";
    break;
  case Type::DoubleTyID:
  case Type::ULongTyID: case Type::LongTyID:
    O << ".quad";
    break;
  default:
    assert (0 && "Can't handle printing this type of thing");
    break;
  }
  O << "\t";
  emitConstantValueOnly(CV);
  O << "\n";
}

/// printConstantPool - Print to the current output stream assembly
/// representations of the constants in the constant pool MCP. This is
/// used to print out constants which have been "spilled to memory" by
/// the code generator.
///
void Printer::printConstantPool(MachineConstantPool *MCP) {
  const std::vector<Constant*> &CP = MCP->getConstants();
  const TargetData &TD = TM.getTargetData();
 
  if (CP.empty()) return;

  for (unsigned i = 0, e = CP.size(); i != e; ++i) {
    O << "\t.section .rodata\n";
    O << "\t.align " << (unsigned)TD.getTypeAlignment(CP[i]->getType())
      << "\n";
    O << ".CPI" << CurrentFnName << "_" << i << ":\t\t\t\t\t#"
      << *CP[i] << "\n";
    emitGlobalConstant(CP[i]);
  }
}

/// runOnMachineFunction - This uses the printMachineInstruction()
/// method to print assembly for each instruction.
///
bool Printer::runOnMachineFunction(MachineFunction &MF) {
  O << "\n\n";
  // What's my mangled name?
  CurrentFnName = Mang->getValueName(MF.getFunction());

  // Print out constants referenced by the function
  printConstantPool(MF.getConstantPool());

  // Print out labels for the function.
  O << "\t.text\n";
  O << "\t.align 16\n";
  O << "\t.globl\t" << CurrentFnName << "\n";
  if (!EmitCygwin)
    O << "\t.type\t" << CurrentFnName << ", @function\n";
  O << CurrentFnName << ":\n";

  // Print out code for the function.
  for (MachineFunction::const_iterator I = MF.begin(), E = MF.end();
       I != E; ++I) {
    // Print a label for the basic block.
    O << ".LBB" << CurrentFnName << "_" << I->getNumber() << ":\t# "
      << I->getBasicBlock()->getName() << "\n";
    for (MachineBasicBlock::const_iterator II = I->begin(), E = I->end();
	 II != E; ++II) {
      // Print the assembly for the instruction.
      O << "\t";
      printMachineInstruction(II);
    }
  }

  // We didn't modify anything.
  return false;
}

static bool isScale(const MachineOperand &MO) {
  return MO.isImmediate() &&
    (MO.getImmedValue() == 1 || MO.getImmedValue() == 2 ||
     MO.getImmedValue() == 4 || MO.getImmedValue() == 8);
}

static bool isMem(const MachineInstr *MI, unsigned Op) {
  if (MI->getOperand(Op).isFrameIndex()) return true;
  if (MI->getOperand(Op).isConstantPoolIndex()) return true;
  return Op+4 <= MI->getNumOperands() &&
    MI->getOperand(Op  ).isRegister() &&isScale(MI->getOperand(Op+1)) &&
    MI->getOperand(Op+2).isRegister() &&MI->getOperand(Op+3).isImmediate();
}



void Printer::printOp(const MachineOperand &MO,
		      bool elideOffsetKeyword /* = false */) {
  const MRegisterInfo &RI = *TM.getRegisterInfo();
  switch (MO.getType()) {
  case MachineOperand::MO_VirtualRegister:
    if (Value *V = MO.getVRegValueOrNull()) {
      O << "<" << V->getName() << ">";
      return;
    }
    // FALLTHROUGH
  case MachineOperand::MO_MachineRegister:
    if (MRegisterInfo::isPhysicalRegister(MO.getReg()))
      // Bug Workaround: See note in Printer::doInitialization about %.
      O << "%" << RI.get(MO.getReg()).Name;
    else
      O << "%reg" << MO.getReg();
    return;

  case MachineOperand::MO_SignExtendedImmed:
  case MachineOperand::MO_UnextendedImmed:
    O << (int)MO.getImmedValue();
    return;
  case MachineOperand::MO_MachineBasicBlock: {
    MachineBasicBlock *MBBOp = MO.getMachineBasicBlock();
    O << ".LBB" << Mang->getValueName(MBBOp->getParent()->getFunction())
      << "_" << MBBOp->getNumber () << "\t# "
      << MBBOp->getBasicBlock ()->getName ();
    return;
  }
  case MachineOperand::MO_PCRelativeDisp:
    std::cerr << "Shouldn't use addPCDisp() when building X86 MachineInstrs";
    abort ();
    return;
  case MachineOperand::MO_GlobalAddress:
    if (!elideOffsetKeyword)
      O << "OFFSET ";
    O << Mang->getValueName(MO.getGlobal());
    return;
  case MachineOperand::MO_ExternalSymbol:
    O << MO.getSymbolName();
    return;
  default:
    O << "<unknown operand type>"; return;    
  }
}

static const char* const sizePtr(const TargetInstrDescriptor &Desc) {
  switch (Desc.TSFlags & X86II::MemMask) {
  default: assert(0 && "Unknown arg size!");
  case X86II::Mem8:   return "BYTE PTR"; 
  case X86II::Mem16:  return "WORD PTR"; 
  case X86II::Mem32:  return "DWORD PTR"; 
  case X86II::Mem64:  return "QWORD PTR"; 
  case X86II::Mem80:  return "XWORD PTR"; 
  }
}

void Printer::printMemReference(const MachineInstr *MI, unsigned Op) {
  assert(isMem(MI, Op) && "Invalid memory reference!");

  if (MI->getOperand(Op).isFrameIndex()) {
    O << "[frame slot #" << MI->getOperand(Op).getFrameIndex();
    if (MI->getOperand(Op+3).getImmedValue())
      O << " + " << MI->getOperand(Op+3).getImmedValue();
    O << "]";
    return;
  } else if (MI->getOperand(Op).isConstantPoolIndex()) {
    O << "[.CPI" << CurrentFnName << "_"
      << MI->getOperand(Op).getConstantPoolIndex();
    if (MI->getOperand(Op+3).getImmedValue())
      O << " + " << MI->getOperand(Op+3).getImmedValue();
    O << "]";
    return;
  }

  const MachineOperand &BaseReg  = MI->getOperand(Op);
  int ScaleVal                   = MI->getOperand(Op+1).getImmedValue();
  const MachineOperand &IndexReg = MI->getOperand(Op+2);
  int DispVal                    = MI->getOperand(Op+3).getImmedValue();

  O << "[";
  bool NeedPlus = false;
  if (BaseReg.getReg()) {
    printOp(BaseReg);
    NeedPlus = true;
  }

  if (IndexReg.getReg()) {
    if (NeedPlus) O << " + ";
    if (ScaleVal != 1)
      O << ScaleVal << "*";
    printOp(IndexReg);
    NeedPlus = true;
  }

  if (DispVal) {
    if (NeedPlus)
      if (DispVal > 0)
	O << " + ";
      else {
	O << " - ";
	DispVal = -DispVal;
      }
    O << DispVal;
  }
  O << "]";
}


/// printImplUsesBefore - Emit the implicit-use registers for the instruction
/// described by DESC, if its PrintImplUsesBefore flag is set.
///
void Printer::printImplUsesBefore(const TargetInstrDescriptor &Desc) {
  const MRegisterInfo &RI = *TM.getRegisterInfo();
  if (Desc.TSFlags & X86II::PrintImplUsesBefore) {
    for (const unsigned *p = Desc.ImplicitUses; *p; ++p) {
      // Bug Workaround: See note in Printer::doInitialization about %.
      O << "%" << RI.get(*p).Name << ", ";
    }
  }
}

/// printImplDefsBefore - Emit the implicit-def registers for the instruction
/// described by DESC, if its PrintImplUsesBefore flag is set.  Return true if
/// we printed any registers.
///
bool Printer::printImplDefsBefore(const TargetInstrDescriptor &Desc) {
  bool Printed = false;
  const MRegisterInfo &RI = *TM.getRegisterInfo();
  if (Desc.TSFlags & X86II::PrintImplDefsBefore) {
    const unsigned *p = Desc.ImplicitDefs;
    if (*p) {
      O << (Printed ? ", %" : "%") << RI.get (*p).Name;
      Printed = true;
      ++p;
    }
    while (*p) {
      // Bug Workaround: See note in Printer::doInitialization about %.
      O << ", %" << RI.get(*p).Name;
      ++p;
    }
  }
  return Printed;
}


/// printImplUsesAfter - Emit the implicit-use registers for the instruction
/// described by DESC, if its PrintImplUsesAfter flag is set.
///
/// Inputs:
///   Comma - List of registers will need a leading comma.
///   Desc  - Description of the Instruction.
///
/// Return value:
///   true  - Emitted one or more registers.
///   false - Emitted no registers.
///
bool Printer::printImplUsesAfter(const TargetInstrDescriptor &Desc,
                                 const bool Comma = true) {
  const MRegisterInfo &RI = *TM.getRegisterInfo();
  if (Desc.TSFlags & X86II::PrintImplUsesAfter) {
    bool emitted = false;
    const unsigned *p = Desc.ImplicitUses;
    if (*p) {
      O << (Comma ? ", %" : "%") << RI.get (*p).Name;
      emitted = true;
      ++p;
    }
    while (*p) {
      // Bug Workaround: See note in Printer::doInitialization about %.
      O << ", %" << RI.get(*p).Name;
      ++p;
    }
    return emitted;
  }
  return false;
}

/// printImplDefsAfter - Emit the implicit-definition registers for the
/// instruction described by DESC, if its PrintImplDefsAfter flag is set.
///
/// Inputs:
///   Comma - List of registers will need a leading comma.
///   Desc  - Description of the Instruction
///
/// Return value:
///   true  - Emitted one or more registers.
///   false - Emitted no registers.
///
bool Printer::printImplDefsAfter(const TargetInstrDescriptor &Desc,
                                 const bool Comma = true) {
  const MRegisterInfo &RI = *TM.getRegisterInfo();
  if (Desc.TSFlags & X86II::PrintImplDefsAfter) {
    bool emitted = false;
    const unsigned *p = Desc.ImplicitDefs;
    if (*p) {
      O << (Comma ? ", %" : "%") << RI.get (*p).Name;
      emitted = true;
      ++p;
    }
    while (*p) {
      // Bug Workaround: See note in Printer::doInitialization about %.
      O << ", %" << RI.get(*p).Name;
      ++p;
    }
    return emitted;
  }
  return false;
}

/// printMachineInstruction -- Print out a single X86 LLVM instruction
/// MI in Intel syntax to the current output stream.
///
void Printer::printMachineInstruction(const MachineInstr *MI) {
  unsigned Opcode = MI->getOpcode();
  const TargetInstrInfo &TII = TM.getInstrInfo();
  const TargetInstrDescriptor &Desc = TII.get(Opcode);

  ++EmittedInsts;
  switch (Desc.TSFlags & X86II::FormMask) {
  case X86II::Pseudo:
    // Print pseudo-instructions as comments; either they should have been
    // turned into real instructions by now, or they don't need to be
    // seen by the assembler (e.g., IMPLICIT_USEs.)
    O << "# ";
    if (Opcode == X86::PHI) {
      printOp(MI->getOperand(0));
      O << " = phi ";
      for (unsigned i = 1, e = MI->getNumOperands(); i != e; i+=2) {
        if (i != 1) O << ", ";
        O << "[";
        printOp(MI->getOperand(i));
        O << ", ";
        printOp(MI->getOperand(i+1));
        O << "]";
      }
    } else {
      unsigned i = 0;
      if (MI->getNumOperands() && MI->getOperand(0).isDef()) {
        printOp(MI->getOperand(0));
        O << " = ";
        ++i;
      }
      O << TII.getName(MI->getOpcode());

      for (unsigned e = MI->getNumOperands(); i != e; ++i) {
        O << " ";
        if (MI->getOperand(i).isDef()) O << "*";
        printOp(MI->getOperand(i));
        if (MI->getOperand(i).isDef()) O << "*";
      }
    }
    O << "\n";
    return;

  case X86II::RawFrm:
  {
    // The accepted forms of Raw instructions are:
    //   1. nop     - No operand required
    //   2. jmp foo - MachineBasicBlock operand
    //   3. call bar - GlobalAddress Operand or External Symbol Operand
    //   4. in AL, imm - Immediate operand
    //
    assert(MI->getNumOperands() == 0 ||
           (MI->getNumOperands() == 1 &&
	    (MI->getOperand(0).isMachineBasicBlock() ||
	     MI->getOperand(0).isGlobalAddress() ||
	     MI->getOperand(0).isExternalSymbol() ||
             MI->getOperand(0).isImmediate())) &&
           "Illegal raw instruction!");
    O << TII.getName(MI->getOpcode()) << " ";

    bool LeadingComma = printImplDefsBefore(Desc);

    if (MI->getNumOperands() == 1) {
      if (LeadingComma) O << ", ";
      printOp(MI->getOperand(0), true); // Don't print "OFFSET"...
      LeadingComma = true;
    }
    LeadingComma = printImplDefsAfter(Desc, LeadingComma) || LeadingComma;
    printImplUsesAfter(Desc, LeadingComma);
    O << "\n";
    return;
  }

  case X86II::AddRegFrm: {
    // There are currently two forms of acceptable AddRegFrm instructions.
    // Either the instruction JUST takes a single register (like inc, dec, etc),
    // or it takes a register and an immediate of the same size as the register
    // (move immediate f.e.).  Note that this immediate value might be stored as
    // an LLVM value, to represent, for example, loading the address of a global
    // into a register.  The initial register might be duplicated if this is a
    // M_2_ADDR_REG instruction
    //
    assert(MI->getOperand(0).isRegister() &&
           (MI->getNumOperands() == 1 || 
            (MI->getNumOperands() == 2 &&
             (MI->getOperand(1).getVRegValueOrNull() ||
              MI->getOperand(1).isImmediate() ||
	      MI->getOperand(1).isRegister() ||
	      MI->getOperand(1).isGlobalAddress() ||
	      MI->getOperand(1).isExternalSymbol()))) &&
           "Illegal form for AddRegFrm instruction!");

    unsigned Reg = MI->getOperand(0).getReg();
    
    O << TII.getName(MI->getOpcode()) << " ";

    printImplUsesBefore(Desc);   // fcmov*

    printOp(MI->getOperand(0));
    if (MI->getNumOperands() == 2 &&
	(!MI->getOperand(1).isRegister() ||
	 MI->getOperand(1).getVRegValueOrNull() ||
	 MI->getOperand(1).isGlobalAddress() ||
	 MI->getOperand(1).isExternalSymbol())) {
      O << ", ";
      printOp(MI->getOperand(1));
    }
    printImplUsesAfter(Desc);
    O << "\n";
    return;
  }
  case X86II::MRMDestReg: {
    // There are three forms of MRMDestReg instructions, those with 2
    // or 3 operands:
    //
    // 2 Operands: this is for things like mov that do not read a
    // second input.
    //
    // 2 Operands: two address instructions which def&use the first
    // argument and use the second as input.
    //
    // 3 Operands: in this form, two address instructions are the same
    // as in 2 but have a constant argument as well.
    //
    bool isTwoAddr = TII.isTwoAddrInstr(Opcode);
    assert(MI->getOperand(0).isRegister() &&
           (MI->getNumOperands() == 2 ||
            (MI->getNumOperands() == 3 && MI->getOperand(2).isImmediate()))
           && "Bad format for MRMDestReg!");

    O << TII.getName(MI->getOpcode()) << " ";
    printOp(MI->getOperand(0));
    O << ", ";
    printOp(MI->getOperand(1));
    if (MI->getNumOperands() == 3) {
      O << ", ";
      printOp(MI->getOperand(2));
    }
    printImplUsesAfter(Desc);
    O << "\n";
    return;
  }

  case X86II::MRMDestMem: {
    // These instructions are the same as MRMDestReg, but instead of having a
    // register reference for the mod/rm field, it's a memory reference.
    //
    assert(isMem(MI, 0) && 
           (MI->getNumOperands() == 4+1 ||
            (MI->getNumOperands() == 4+2 && MI->getOperand(5).isImmediate()))
           && "Bad format for MRMDestMem!");

    O << TII.getName(MI->getOpcode()) << " " << sizePtr(Desc) << " ";
    printMemReference(MI, 0);
    O << ", ";
    printOp(MI->getOperand(4));
    if (MI->getNumOperands() == 4+2) {
      O << ", ";
      printOp(MI->getOperand(5));
    }
    printImplUsesAfter(Desc);
    O << "\n";
    return;
  }

  case X86II::MRMSrcReg: {
    // There are three forms that are acceptable for MRMSrcReg
    // instructions, those with 2 or 3 operands:
    //
    // 2 Operands: this is for things like mov that do not read a
    // second input.
    //
    // 2 Operands: in this form, the last register is the ModR/M
    // input.  The first operand is a def&use.  This is for things
    // like: add r32, r/m32
    //
    // 3 Operands: in this form, we can have 'INST R1, R2, imm', which is used
    // for instructions like the IMULrri instructions.
    //
    //
    assert(MI->getOperand(0).isRegister() &&
           MI->getOperand(1).isRegister() &&
           (MI->getNumOperands() == 2 ||
            (MI->getNumOperands() == 3 &&
             (MI->getOperand(2).isImmediate())))
           && "Bad format for MRMSrcReg!");

    O << TII.getName(MI->getOpcode()) << " ";
    printOp(MI->getOperand(0));
    O << ", ";
    printOp(MI->getOperand(1));
    if (MI->getNumOperands() == 3) {
        O << ", ";
        printOp(MI->getOperand(2));
    }
    O << "\n";
    return;
  }

  case X86II::MRMSrcMem: {
    // These instructions are the same as MRMSrcReg, but instead of having a
    // register reference for the mod/rm field, it's a memory reference.
    //
    assert(MI->getOperand(0).isRegister() &&
           (MI->getNumOperands() == 1+4 && isMem(MI, 1)) || 
(MI->getNumOperands() == 2+4 && MI->getOperand(5).isImmediate() && isMem(MI, 1))
           && "Bad format for MRMSrcMem!");
    O << TII.getName(MI->getOpcode()) << " ";
    printOp(MI->getOperand(0));
    O << ", " << sizePtr(Desc) << " ";
    printMemReference(MI, 1);
    if (MI->getNumOperands() == 2+4) {
      O << ", ";
      printOp(MI->getOperand(5));
    }
    O << "\n";
    return;
  }

  case X86II::MRM0r: case X86II::MRM1r:
  case X86II::MRM2r: case X86II::MRM3r:
  case X86II::MRM4r: case X86II::MRM5r:
  case X86II::MRM6r: case X86II::MRM7r: {
    // In this form, the following are valid formats:
    //  1. sete r
    //  2. cmp reg, immediate
    //  2. shl rdest, rinput  <implicit CL or 1>
    //  3. sbb rdest, rinput, immediate   [rdest = rinput]
    //    
    assert(MI->getNumOperands() > 0 && MI->getNumOperands() < 4 &&
           MI->getOperand(0).isRegister() && "Bad MRMSxR format!");
    assert((MI->getNumOperands() != 2 ||
            MI->getOperand(1).isRegister() || MI->getOperand(1).isImmediate())&&
           "Bad MRMSxR format!");
    assert((MI->getNumOperands() < 3 ||
	    (MI->getOperand(1).isRegister() && MI->getOperand(2).isImmediate())) &&
           "Bad MRMSxR format!");

    if (MI->getNumOperands() > 1 && MI->getOperand(1).isRegister() && 
        MI->getOperand(0).getReg() != MI->getOperand(1).getReg())
      O << "**";

    O << TII.getName(MI->getOpcode()) << " ";
    printOp(MI->getOperand(0));
    if (MI->getOperand(MI->getNumOperands()-1).isImmediate()) {
      O << ", ";
      printOp(MI->getOperand(MI->getNumOperands()-1));
    }
    printImplUsesAfter(Desc);
    O << "\n";

    return;
  }

  case X86II::MRM0m: case X86II::MRM1m:
  case X86II::MRM2m: case X86II::MRM3m:
  case X86II::MRM4m: case X86II::MRM5m:
  case X86II::MRM6m: case X86II::MRM7m: {
    // In this form, the following are valid formats:
    //  1. sete [m]
    //  2. cmp [m], immediate
    //  2. shl [m], rinput  <implicit CL or 1>
    //  3. sbb [m], immediate
    //    
    assert(MI->getNumOperands() >= 4 && MI->getNumOperands() <= 5 &&
           isMem(MI, 0) && "Bad MRMSxM format!");
    assert((MI->getNumOperands() != 5 ||
            (MI->getOperand(4).isImmediate() ||
             MI->getOperand(4).isGlobalAddress())) &&
           "Bad MRMSxM format!");

    const MachineOperand &Op3 = MI->getOperand(3);

    // gas bugs:
    //
    // The 80-bit FP store-pop instruction "fstp XWORD PTR [...]"
    // is misassembled by gas in intel_syntax mode as its 32-bit
    // equivalent "fstp DWORD PTR [...]". Workaround: Output the raw
    // opcode bytes instead of the instruction.
    //
    // The 80-bit FP load instruction "fld XWORD PTR [...]" is
    // misassembled by gas in intel_syntax mode as its 32-bit
    // equivalent "fld DWORD PTR [...]". Workaround: Output the raw
    // opcode bytes instead of the instruction.
    //
    // gas intel_syntax mode treats "fild QWORD PTR [...]" as an
    // invalid opcode, saying "64 bit operations are only supported in
    // 64 bit modes." libopcodes disassembles it as "fild DWORD PTR
    // [...]", which is wrong. Workaround: Output the raw opcode bytes
    // instead of the instruction.
    //
    // gas intel_syntax mode treats "fistp QWORD PTR [...]" as an
    // invalid opcode, saying "64 bit operations are only supported in
    // 64 bit modes." libopcodes disassembles it as "fistpll DWORD PTR
    // [...]", which is wrong. Workaround: Output the raw opcode bytes
    // instead of the instruction.
    if (MI->getOpcode() == X86::FSTP80m ||
        MI->getOpcode() == X86::FLD80m ||
        MI->getOpcode() == X86::FILD64m ||
        MI->getOpcode() == X86::FISTP64m) {
        GasBugWorkaroundEmitter gwe(O);
        X86::emitInstruction(gwe, (X86InstrInfo&)TM.getInstrInfo(), *MI);
    }

    O << TII.getName(MI->getOpcode()) << " ";
    O << sizePtr(Desc) << " ";
    printMemReference(MI, 0);
    if (MI->getNumOperands() == 5) {
      O << ", ";
      printOp(MI->getOperand(4));
    }
    printImplUsesAfter(Desc);
    O << "\n";
    return;
  }
  default:
    O << "\tUNKNOWN FORM:\t\t-"; MI->print(O, TM); break;
  }
}

bool Printer::doInitialization(Module &M) {
  // Tell gas we are outputting Intel syntax (not AT&T syntax) assembly.
  //
  // Bug: gas in `intel_syntax noprefix' mode interprets the symbol `Sp' in an
  // instruction as a reference to the register named sp, and if you try to
  // reference a symbol `Sp' (e.g. `mov ECX, OFFSET Sp') then it gets lowercased
  // before being looked up in the symbol table. This creates spurious
  // `undefined symbol' errors when linking. Workaround: Do not use `noprefix'
  // mode, and decorate all register names with percent signs.
  O << "\t.intel_syntax\n";
  Mang = new Mangler(M, EmitCygwin);
  return false; // success
}

// SwitchSection - Switch to the specified section of the executable if we are
// not already in it!
//
static void SwitchSection(std::ostream &OS, std::string &CurSection,
                          const char *NewSection) {
  if (CurSection != NewSection) {
    CurSection = NewSection;
    if (!CurSection.empty())
      OS << "\t" << NewSection << "\n";
  }
}

bool Printer::doFinalization(Module &M) {
  const TargetData &TD = TM.getTargetData();
  std::string CurSection;

  // Print out module-level global variables here.
  for (Module::const_giterator I = M.gbegin(), E = M.gend(); I != E; ++I)
    if (I->hasInitializer()) {   // External global require no code
      O << "\n\n";
      std::string name = Mang->getValueName(I);
      Constant *C = I->getInitializer();
      unsigned Size = TD.getTypeSize(C->getType());
      unsigned Align = TD.getTypeAlignment(C->getType());

      if (C->isNullValue() && 
          (I->hasLinkOnceLinkage() || I->hasInternalLinkage() ||
           I->hasWeakLinkage() /* FIXME: Verify correct */)) {
        SwitchSection(O, CurSection, ".data");
        if (I->hasInternalLinkage())
          O << "\t.local " << name << "\n";
        
        O << "\t.comm " << name << "," << TD.getTypeSize(C->getType())
          << "," << (unsigned)TD.getTypeAlignment(C->getType());
        O << "\t\t# ";
        WriteAsOperand(O, I, true, true, &M);
        O << "\n";
      } else {
        switch (I->getLinkage()) {
        case GlobalValue::LinkOnceLinkage:
        case GlobalValue::WeakLinkage:   // FIXME: Verify correct for weak.
          // Nonnull linkonce -> weak
          O << "\t.weak " << name << "\n";
          SwitchSection(O, CurSection, "");
          O << "\t.section\t.llvm.linkonce.d." << name << ",\"aw\",@progbits\n";
          break;
        
        case GlobalValue::AppendingLinkage:
          // FIXME: appending linkage variables should go into a section of
          // their name or something.  For now, just emit them as external.
        case GlobalValue::ExternalLinkage:
          // If external or appending, declare as a global symbol
          O << "\t.globl " << name << "\n";
          // FALL THROUGH
        case GlobalValue::InternalLinkage:
          if (C->isNullValue())
            SwitchSection(O, CurSection, ".bss");
          else
            SwitchSection(O, CurSection, ".data");
          break;
        }

        O << "\t.align " << Align << "\n";
        O << "\t.type " << name << ",@object\n";
        O << "\t.size " << name << "," << Size << "\n";
        O << name << ":\t\t\t\t# ";
        WriteAsOperand(O, I, true, true, &M);
        O << " = ";
        WriteAsOperand(O, C, false, false, &M);
        O << "\n";
        emitGlobalConstant(C);
      }
    }

  delete Mang;
  return false; // success
}
