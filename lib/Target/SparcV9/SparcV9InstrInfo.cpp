//***************************************************************************
// File:
//	SparcInstrInfo.cpp
// 
// Purpose:
//	
// History:
//	10/15/01	 -  Vikram Adve  -  Created
//**************************************************************************/


#include "SparcInternals.h"
#include "SparcInstrSelectionSupport.h"
#include "llvm/Target/Sparc.h"
#include "llvm/CodeGen/InstrSelection.h"
#include "llvm/CodeGen/InstrSelectionSupport.h"
#include "llvm/CodeGen/MachineCodeForMethod.h"
#include "llvm/CodeGen/MachineCodeForInstruction.h"
#include "llvm/Function.h"
#include "llvm/BasicBlock.h"
#include "llvm/Instruction.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
using std::vector;

//************************ Internal Functions ******************************/

static const uint32_t MAXLO   = (1 << 10) - 1; // set bits set by %lo(*)
static const uint32_t MAXSIMM = (1 << 12) - 1; // set bits in simm13 field of OR


// Set a 32-bit unsigned constant in the register `dest'.
// 
static inline void
CreateSETUWConst(const TargetMachine& target, uint32_t C,
                 Instruction* dest, std::vector<MachineInstr*>& mvec)
{
  MachineInstr *miSETHI = NULL, *miOR = NULL;
  
  // In order to get efficient code, we should not generate the SETHI if
  // all high bits are 1 (i.e., this is a small signed value that fits in
  // the simm13 field of OR).  So we check for and handle that case specially.
  // NOTE: The value C = 0x80000000 is bad: sC < 0 *and* -sC < 0.
  //       In fact, sC == -sC, so we have to check for this explicitly.
  int32_t sC = (int32_t) C;
  bool smallSignedValue = sC < 0 && sC != -sC && -sC < (int32_t) MAXSIMM;
  
  // Set the high 22 bits in dest if non-zero and simm13 field of OR not enough
  if (!smallSignedValue && (C & ~MAXLO) && C > MAXSIMM)
    {
      miSETHI = Create2OperandInstr_UImmed(SETHI, C, dest);
      miSETHI->setOperandHi32(0);
      mvec.push_back(miSETHI);
    }
  
  // Set the low 10 or 12 bits in dest.  This is necessary if no SETHI
  // was generated, or if the low 10 bits are non-zero.
  if (miSETHI==NULL || C & MAXLO)
    {
      if (miSETHI)
        { // unsigned value with high-order bits set using SETHI
          miOR = Create3OperandInstr_UImmed(OR, dest, C, dest);
          miOR->setOperandLo32(1);
        }
      else
        { // unsigned or small signed value that fits in simm13 field of OR
          assert(smallSignedValue || (C & ~MAXSIMM) == 0);
          miOR = new MachineInstr(OR);
          miOR->SetMachineOperandReg(0, target.getRegInfo().getZeroRegNum());
          miOR->SetMachineOperandConst(1, MachineOperand::MO_SignExtendedImmed,
                                       sC);
          miOR->SetMachineOperandVal(2,MachineOperand::MO_VirtualRegister,dest);
        }
      mvec.push_back(miOR);
    }
  
  assert((miSETHI || miOR) && "Oops, no code was generated!");
}

// Set a 32-bit constant (given by a symbolic label) in the register `dest'.
// Not needed for SPARC v9 but useful to make the two SETX functions similar
static inline void
CreateSETUWLabel(const TargetMachine& target, Value* val,
                 Instruction* dest, std::vector<MachineInstr*>& mvec)
{
  MachineInstr* MI;
  
  // Set the high 22 bits in dest
  MI = Create2OperandInstr(SETHI, val, dest);
  MI->setOperandHi32(0);
  mvec.push_back(MI);
  
  // Set the low 10 bits in dest
  MI = Create3OperandInstr(OR, dest, val, dest);
  MI->setOperandLo32(1);
  mvec.push_back(MI);
}


// Set a 32-bit signed constant in the register `dest', 
// with sign-extension to 64 bits.
static inline void
CreateSETSWConst(const TargetMachine& target, int32_t C,
                 Instruction* dest, std::vector<MachineInstr*>& mvec)
{
  MachineInstr* MI;
  
  // Set the low 32 bits of dest
  CreateSETUWConst(target, (uint32_t) C,  dest, mvec);
  
  // Sign-extend to the high 32 bits if needed
  if (C < 0 && (-C) > (int32_t) MAXSIMM)
    {
      MI = Create3OperandInstr_UImmed(SRA, dest, 0, dest);
      mvec.push_back(MI);
    }
}


// Set a 64-bit signed or unsigned constant in the register `dest'.
static inline void
CreateSETXConst(const TargetMachine& target, uint64_t C,
                Instruction* tmpReg, Instruction* dest,
                std::vector<MachineInstr*>& mvec)
{
  assert(C > (unsigned int) ~0 && "Use SETUW/SETSW for 32-bit values!");
  
  MachineInstr* MI;
  
  // Code to set the upper 32 bits of the value in register `tmpReg'
  CreateSETUWConst(target, (C >> 32), tmpReg, mvec);
  
  // Shift tmpReg left by 32 bits
  MI = Create3OperandInstr_UImmed(SLLX, tmpReg, 32, tmpReg);
  mvec.push_back(MI);
  
  // Code to set the low 32 bits of the value in register `dest'
  CreateSETUWConst(target, C, dest, mvec);
  
  // dest = OR(tmpReg, dest)
  MI = Create3OperandInstr(OR, dest, tmpReg, dest);
  mvec.push_back(MI);
}


// Set a 64-bit constant (given by a symbolic label) in the register `dest'.
static inline void
CreateSETXLabel(const TargetMachine& target,
                Value* val, Instruction* tmpReg, Instruction* dest,
                std::vector<MachineInstr*>& mvec)
{
  assert(isa<Constant>(val) || isa<GlobalValue>(val) &&
         "I only know about constant values and global addresses");
  
  MachineInstr* MI;
  
  MI = Create2OperandInstr_Addr(SETHI, val, tmpReg);
  MI->setOperandHi64(0);
  mvec.push_back(MI);
  
  MI = Create3OperandInstr_Addr(OR, tmpReg, val, tmpReg);
  MI->setOperandLo64(1);
  mvec.push_back(MI);
  
  MI = Create3OperandInstr_UImmed(SLLX, tmpReg, 32, tmpReg);
  mvec.push_back(MI);
  
  MI = Create2OperandInstr_Addr(SETHI, val, dest);
  MI->setOperandHi32(0);
  mvec.push_back(MI);
  
  MI = Create3OperandInstr(OR, dest, tmpReg, dest);
  mvec.push_back(MI);
  
  MI = Create3OperandInstr_Addr(OR, dest, val, dest);
  MI->setOperandLo32(1);
  mvec.push_back(MI);
}


static inline void
CreateIntSetInstruction(const TargetMachine& target,
                        int64_t C, Instruction* dest,
                        std::vector<MachineInstr*>& mvec,
                        MachineCodeForInstruction& mcfi)
{
  assert(dest->getType()->isSigned() && "Use CreateUIntSetInstruction()");
  
  uint64_t absC = (C >= 0)? C : -C;
  if (absC > (unsigned int) ~0)
    { // C does not fit in 32 bits
      TmpInstruction* tmpReg = new TmpInstruction(Type::IntTy);
      mcfi.addTemp(tmpReg);
      CreateSETXConst(target, (uint64_t) C, tmpReg, dest, mvec);
    }
  else
    CreateSETSWConst(target, (int32_t) C, dest, mvec);
}


static inline void
CreateUIntSetInstruction(const TargetMachine& target,
                         uint64_t C, Instruction* dest,
                         std::vector<MachineInstr*>& mvec,
                         MachineCodeForInstruction& mcfi)
{
  assert(! dest->getType()->isSigned() && "Use CreateIntSetInstruction()");
  MachineInstr* M;
  
  if (C > (unsigned int) ~0)
    { // C does not fit in 32 bits
      assert(dest->getType() == Type::ULongTy && "Sign extension problems");
      TmpInstruction *tmpReg = new TmpInstruction(Type::IntTy);
      mcfi.addTemp(tmpReg);
      CreateSETXConst(target, C, tmpReg, dest, mvec);
    }
  else
    {
#undef SIGN_EXTEND_FOR_UNSIGNED_DEST
#ifdef SIGN_EXTEND_FOR_UNSIGNED_DEST
      // If dest is smaller than the standard integer reg. size
      // and the high-order bit of dest will be 1, then we have to
      // extend the sign-bit into upper bits of the dest register.
      // 
      unsigned destSize = target.DataLayout.getTypeSize(dest->getType());
      if (destSize < target.DataLayout.getIntegerRegize())
        {
          assert(destSize <= 4 && "Unexpected type size of 5-7 bytes");
          uint32_t signBit = C & (1 << (8*destSize-1));
          if (signBit)
            { // Sign-bit is 1 so convert C to a sign-extended 64-bit value
              // and use CreateSETSWConst.  CreateSETSWConst will correctly
              // generate efficient code for small signed values.
              int32_t simmC = C | ~(signBit-1);
              CreateSETSWConst(target, simmC, dest, mvec);
              return;
            }
        }
#endif /*SIGN_EXTEND_FOR_UNSIGNED_DEST*/
      
      CreateSETUWConst(target, C, dest, mvec);
    }
}


//************************* External Classes *******************************/

//---------------------------------------------------------------------------
// class UltraSparcInstrInfo 
// 
// Purpose:
//   Information about individual instructions.
//   Most information is stored in the SparcMachineInstrDesc array above.
//   Other information is computed on demand, and most such functions
//   default to member functions in base class MachineInstrInfo. 
//---------------------------------------------------------------------------

/*ctor*/
UltraSparcInstrInfo::UltraSparcInstrInfo(const TargetMachine& tgt)
  : MachineInstrInfo(tgt, SparcMachineInstrDesc,
		     /*descSize = */ NUM_TOTAL_OPCODES,
		     /*numRealOpCodes = */ NUM_REAL_OPCODES)
{
}

// 
// Create an instruction sequence to put the constant `val' into
// the virtual register `dest'.  `val' may be a Constant or a
// GlobalValue, viz., the constant address of a global variable or function.
// The generated instructions are returned in `mvec'.
// Any temp. registers (TmpInstruction) created are recorded in mcfi.
// Any stack space required is allocated via MachineCodeForMethod.
// 
void
UltraSparcInstrInfo::CreateCodeToLoadConst(const TargetMachine& target,
                                           Function* F,
                                           Value* val,
                                           Instruction* dest,
                                           std::vector<MachineInstr*>& mvec,
                                       MachineCodeForInstruction& mcfi) const
{
  assert(isa<Constant>(val) || isa<GlobalValue>(val) &&
         "I only know about constant values and global addresses");
  
  // Use a "set" instruction for known constants or symbolic constants (labels)
  // that can go in an integer reg.
  // We have to use a "load" instruction for all other constants,
  // in particular, floating point constants.
  // 
  const Type* valType = val->getType();
  
  if (isa<GlobalValue>(val) || valType->isIntegral() || valType == Type::BoolTy)
    {
      if (isa<GlobalValue>(val))
        {
          TmpInstruction* tmpReg =
            new TmpInstruction(PointerType::get(val->getType()), val);
          mcfi.addTemp(tmpReg);
          CreateSETXLabel(target, val, tmpReg, dest, mvec);
        }
      else if (! val->getType()->isSigned())
        {
          uint64_t C = cast<ConstantUInt>(val)->getValue();
          CreateUIntSetInstruction(target, C, dest, mvec, mcfi);
        }
      else
        {
          bool isValidConstant;
          int64_t C = GetConstantValueAsSignedInt(val, isValidConstant);
          assert(isValidConstant && "Unrecognized constant");
          CreateIntSetInstruction(target, C, dest, mvec, mcfi);
        }
    }
  else
    {
      // Make an instruction sequence to load the constant, viz:
      //            SETX <addr-of-constant>, tmpReg, addrReg
      //            LOAD  /*addr*/ addrReg, /*offset*/ 0, dest
      
      // First, create a tmp register to be used by the SETX sequence.
      TmpInstruction* tmpReg =
        new TmpInstruction(PointerType::get(val->getType()), val);
      mcfi.addTemp(tmpReg);
      
      // Create another TmpInstruction for the address register
      TmpInstruction* addrReg =
            new TmpInstruction(PointerType::get(val->getType()), val);
      mcfi.addTemp(addrReg);
      
      // Put the address (a symbolic name) into a register
      CreateSETXLabel(target, val, tmpReg, addrReg, mvec);
      
      // Generate the load instruction
      int64_t zeroOffset = 0;           // to avoid ambiguity with (Value*) 0
      MachineInstr* MI =
        Create3OperandInstr_SImmed(ChooseLoadInstruction(val->getType()),
                                   addrReg, zeroOffset, dest);
      mvec.push_back(MI);
      
      // Make sure constant is emitted to constant pool in assembly code.
      MachineCodeForMethod::get(F).addToConstantPool(cast<Constant>(val));
    }
}


// Create an instruction sequence to copy an integer value `val'
// to a floating point value `dest' by copying to memory and back.
// val must be an integral type.  dest must be a Float or Double.
// The generated instructions are returned in `mvec'.
// Any temp. registers (TmpInstruction) created are recorded in mcfi.
// Any stack space required is allocated via MachineCodeForMethod.
// 
void
UltraSparcInstrInfo::CreateCodeToCopyIntToFloat(const TargetMachine& target,
                                        Function* F,
                                        Value* val,
                                        Instruction* dest,
                                        std::vector<MachineInstr*>& mvec,
                                        MachineCodeForInstruction& mcfi) const
{
  assert((val->getType()->isIntegral() || isa<PointerType>(val->getType()))
         && "Source type must be integral");
  assert(dest->getType()->isFloatingPoint()
         && "Dest type must be float/double");
  
  int offset = MachineCodeForMethod::get(F).allocateLocalVar(target, val); 
  
  // Store instruction stores `val' to [%fp+offset].
  // The store and load opCodes are based on the value being copied, and
  // they use integer and float types that accomodate the
  // larger of the source type and the destination type:
  // On SparcV9: int for float, long for double.
  // 
  Type* tmpType = (dest->getType() == Type::FloatTy)? Type::IntTy
                                                    : Type::LongTy;
  MachineInstr* store = new MachineInstr(ChooseStoreInstruction(tmpType));
  store->SetMachineOperandVal(0, MachineOperand::MO_VirtualRegister, val);
  store->SetMachineOperandReg(1, target.getRegInfo().getFramePointer());
  store->SetMachineOperandConst(2,MachineOperand::MO_SignExtendedImmed,offset);
  mvec.push_back(store);

  // Load instruction loads [%fp+offset] to `dest'.
  // 
  MachineInstr* load =new MachineInstr(ChooseLoadInstruction(dest->getType()));
  load->SetMachineOperandReg(0, target.getRegInfo().getFramePointer());
  load->SetMachineOperandConst(1, MachineOperand::MO_SignExtendedImmed,offset);
  load->SetMachineOperandVal(2, MachineOperand::MO_VirtualRegister, dest);
  mvec.push_back(load);
}


// Similarly, create an instruction sequence to copy an FP value
// `val' to an integer value `dest' by copying to memory and back.
// The generated instructions are returned in `mvec'.
// Any temp. registers (TmpInstruction) created are recorded in mcfi.
// Any stack space required is allocated via MachineCodeForMethod.
// 
void
UltraSparcInstrInfo::CreateCodeToCopyFloatToInt(const TargetMachine& target,
                                        Function* F,
                                        Value* val,
                                        Instruction* dest,
                                        std::vector<MachineInstr*>& mvec,
                                        MachineCodeForInstruction& mcfi) const
{
  assert(val->getType()->isFloatingPoint()
         && "Source type must be float/double");
  assert((dest->getType()->isIntegral() || isa<PointerType>(dest->getType()))
         && "Dest type must be integral");
  
  int offset = MachineCodeForMethod::get(F).allocateLocalVar(target, val); 
  
  // Store instruction stores `val' to [%fp+offset].
  // The store and load opCodes are based on the value being copied, and
  // they use the integer type that matches the source type in size:
  // On SparcV9: int for float, long for double.
  // 
  Type* tmpType = (val->getType() == Type::FloatTy)? Type::IntTy
                                                   : Type::LongTy;
  MachineInstr* store=new MachineInstr(ChooseStoreInstruction(val->getType()));
  store->SetMachineOperandVal(0, MachineOperand::MO_VirtualRegister, val);
  store->SetMachineOperandReg(1, target.getRegInfo().getFramePointer());
  store->SetMachineOperandConst(2,MachineOperand::MO_SignExtendedImmed,offset);
  mvec.push_back(store);
  
  // Load instruction loads [%fp+offset] to `dest'.
  // 
  MachineInstr* load = new MachineInstr(ChooseLoadInstruction(tmpType));
  load->SetMachineOperandReg(0, target.getRegInfo().getFramePointer());
  load->SetMachineOperandConst(1, MachineOperand::MO_SignExtendedImmed,offset);
  load->SetMachineOperandVal(2, MachineOperand::MO_VirtualRegister, dest);
  mvec.push_back(load);
}


// Create instruction(s) to copy src to dest, for arbitrary types
// The generated instructions are returned in `mvec'.
// Any temp. registers (TmpInstruction) created are recorded in mcfi.
// Any stack space required is allocated via MachineCodeForMethod.
// 
void
UltraSparcInstrInfo::CreateCopyInstructionsByType(const TargetMachine& target,
                                                  Function *F,
                                                  Value* src,
                                                  Instruction* dest,
                                                  vector<MachineInstr*>& mvec,
                                          MachineCodeForInstruction& mcfi) const
{
  bool loadConstantToReg = false;
  
  const Type* resultType = dest->getType();
  
  MachineOpCode opCode = ChooseAddInstructionByType(resultType);
  if (opCode == INVALID_OPCODE)
    {
      assert(0 && "Unsupported result type in CreateCopyInstructionsByType()");
      return;
    }
  
  // if `src' is a constant that doesn't fit in the immed field or if it is
  // a global variable (i.e., a constant address), generate a load
  // instruction instead of an add
  // 
  if (isa<Constant>(src))
    {
      unsigned int machineRegNum;
      int64_t immedValue;
      MachineOperand::MachineOperandType opType =
        ChooseRegOrImmed(src, opCode, target, /*canUseImmed*/ true,
                         machineRegNum, immedValue);
      
      if (opType == MachineOperand::MO_VirtualRegister)
        loadConstantToReg = true;
    }
  else if (isa<GlobalValue>(src))
    loadConstantToReg = true;
  
  if (loadConstantToReg)
    { // `src' is constant and cannot fit in immed field for the ADD
      // Insert instructions to "load" the constant into a register
      target.getInstrInfo().CreateCodeToLoadConst(target, F, src, dest,
                                                  mvec, mcfi);
    }
  else
    { // Create an add-with-0 instruction of the appropriate type.
      // Make `src' the second operand, in case it is a constant
      // Use (unsigned long) 0 for a NULL pointer value.
      // 
      const Type* zeroValueType =
        isa<PointerType>(resultType) ? Type::ULongTy : resultType;
      MachineInstr* minstr =
        Create3OperandInstr(opCode, Constant::getNullValue(zeroValueType),
                            src, dest);
      mvec.push_back(minstr);
    }
}


// Create instruction sequence to produce a sign-extended register value
// from an arbitrary sized value (sized in bits, not bytes).
// For SPARC v9, we sign-extend the given unsigned operand using SLL; SRA.
// The generated instructions are returned in `mvec'.
// Any temp. registers (TmpInstruction) created are recorded in mcfi.
// Any stack space required is allocated via MachineCodeForMethod.
// 
void
UltraSparcInstrInfo::CreateSignExtensionInstructions(
                                        const TargetMachine& target,
                                        Function* F,
                                        Value* unsignedSrcVal,
                                        unsigned int srcSizeInBits,
                                        Value* dest,
                                        vector<MachineInstr*>& mvec,
                                        MachineCodeForInstruction& mcfi) const
{
  MachineInstr* M;
  
  assert(srcSizeInBits > 0 && srcSizeInBits <= 32
     && "Hmmm... srcSizeInBits > 32 unexpected but could be handled here.");
  
  if (srcSizeInBits < 32)
    { // SLL is needed since operand size is < 32 bits.
      TmpInstruction *tmpI = new TmpInstruction(dest->getType(),
                                                unsignedSrcVal, dest,"make32");
      mcfi.addTemp(tmpI);
      M = Create3OperandInstr_UImmed(SLL,unsignedSrcVal,32-srcSizeInBits,tmpI);
      mvec.push_back(M);
      unsignedSrcVal = tmpI;
    }
  
  M = Create3OperandInstr_UImmed(SRA, unsignedSrcVal, 32-srcSizeInBits, dest);
  mvec.push_back(M);
}
