//===-- SparcInstrInfo.cpp ------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "SparcInternals.h"
#include "SparcInstrSelectionSupport.h"
#include "llvm/CodeGen/InstrSelection.h"
#include "llvm/CodeGen/InstrSelectionSupport.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionInfo.h"
#include "llvm/CodeGen/MachineCodeForInstruction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/Function.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "Config/stdlib.h"

static const uint32_t MAXLO   = (1 << 10) - 1; // set bits set by %lo(*)
static const uint32_t MAXSIMM = (1 << 12) - 1; // set bits in simm13 field of OR


//---------------------------------------------------------------------------
// Function GetConstantValueAsUnsignedInt
// Function GetConstantValueAsSignedInt
// 
// Convenience functions to get the value of an integral constant, for an
// appropriate integer or non-integer type that can be held in a signed
// or unsigned integer respectively.  The type of the argument must be
// the following:
//      Signed or unsigned integer
//      Boolean
//      Pointer
// 
// isValidConstant is set to true if a valid constant was found.
//---------------------------------------------------------------------------

static uint64_t
GetConstantValueAsUnsignedInt(const Value *V,
                              bool &isValidConstant)
{
  isValidConstant = true;

  if (isa<Constant>(V))
    if (const ConstantBool *CB = dyn_cast<ConstantBool>(V))
      return (int64_t)CB->getValue();
    else if (const ConstantSInt *CS = dyn_cast<ConstantSInt>(V))
      return (uint64_t)CS->getValue();
    else if (const ConstantUInt *CU = dyn_cast<ConstantUInt>(V))
      return CU->getValue();

  isValidConstant = false;
  return 0;
}

int64_t
GetConstantValueAsSignedInt(const Value *V, bool &isValidConstant)
{
  uint64_t C = GetConstantValueAsUnsignedInt(V, isValidConstant);
  if (isValidConstant) {
    if (V->getType()->isSigned() || C < INT64_MAX) // safe to cast to signed
      return (int64_t) C;
    else
      isValidConstant = false;
  }
  return 0;
}


//----------------------------------------------------------------------------
// Function: CreateSETUWConst
// 
// Set a 32-bit unsigned constant in the register `dest', using
// SETHI, OR in the worst case.  This function correctly emulates
// the SETUW pseudo-op for SPARC v9 (if argument isSigned == false).
//
// The isSigned=true case is used to implement SETSW without duplicating code.
// 
// Optimize some common cases:
// (1) Small value that fits in simm13 field of OR: don't need SETHI.
// (2) isSigned = true and C is a small negative signed value, i.e.,
//     high bits are 1, and the remaining bits fit in simm13(OR).
//----------------------------------------------------------------------------

static inline void
CreateSETUWConst(const TargetMachine& target, uint32_t C,
                 Instruction* dest, std::vector<MachineInstr*>& mvec,
                 bool isSigned = false)
{
  MachineInstr *miSETHI = NULL, *miOR = NULL;

  // In order to get efficient code, we should not generate the SETHI if
  // all high bits are 1 (i.e., this is a small signed value that fits in
  // the simm13 field of OR).  So we check for and handle that case specially.
  // NOTE: The value C = 0x80000000 is bad: sC < 0 *and* -sC < 0.
  //       In fact, sC == -sC, so we have to check for this explicitly.
  int32_t sC = (int32_t) C;
  bool smallNegValue =isSigned && sC < 0 && sC != -sC && -sC < (int32_t)MAXSIMM;

  // Set the high 22 bits in dest if non-zero and simm13 field of OR not enough
  if (!smallNegValue && (C & ~MAXLO) && C > MAXSIMM) {
    miSETHI = BuildMI(V9::SETHI, 2).addZImm(C).addRegDef(dest);
    miSETHI->setOperandHi32(0);
    mvec.push_back(miSETHI);
  }
  
  // Set the low 10 or 12 bits in dest.  This is necessary if no SETHI
  // was generated, or if the low 10 bits are non-zero.
  if (miSETHI==NULL || C & MAXLO) {
    if (miSETHI) {
      // unsigned value with high-order bits set using SETHI
      miOR = BuildMI(V9::ORi,3).addReg(dest).addZImm(C).addRegDef(dest);
      miOR->setOperandLo32(1);
    } else {
      // unsigned or small signed value that fits in simm13 field of OR
      assert(smallNegValue || (C & ~MAXSIMM) == 0);
      miOR = BuildMI(V9::ORi, 3).addMReg(target.getRegInfo()
                                        .getZeroRegNum())
        .addSImm(sC).addRegDef(dest);
    }
    mvec.push_back(miOR);
  }
  
  assert((miSETHI || miOR) && "Oops, no code was generated!");
}


//----------------------------------------------------------------------------
// Function: CreateSETSWConst
// 
// Set a 32-bit signed constant in the register `dest', with sign-extension
// to 64 bits.  This uses SETHI, OR, SRA in the worst case.
// This function correctly emulates the SETSW pseudo-op for SPARC v9.
//
// Optimize the same cases as SETUWConst, plus:
// (1) SRA is not needed for positive or small negative values.
//----------------------------------------------------------------------------

static inline void
CreateSETSWConst(const TargetMachine& target, int32_t C,
                 Instruction* dest, std::vector<MachineInstr*>& mvec)
{
  // Set the low 32 bits of dest
  CreateSETUWConst(target, (uint32_t) C,  dest, mvec, /*isSigned*/true);

  // Sign-extend to the high 32 bits if needed.
  // NOTE: The value C = 0x80000000 is bad: -C == C and so -C is < MAXSIMM
  if (C < 0 && (C == -C || -C > (int32_t) MAXSIMM))
    mvec.push_back(BuildMI(V9::SRAi5,3).addReg(dest).addZImm(0).addRegDef(dest));
}


//----------------------------------------------------------------------------
// Function: CreateSETXConst
// 
// Set a 64-bit signed or unsigned constant in the register `dest'.
// Use SETUWConst for each 32 bit word, plus a left-shift-by-32 in between.
// This function correctly emulates the SETX pseudo-op for SPARC v9.
//
// Optimize the same cases as SETUWConst for each 32 bit word.
//----------------------------------------------------------------------------

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
  mvec.push_back(BuildMI(V9::SLLXi6, 3).addReg(tmpReg).addZImm(32)
                 .addRegDef(tmpReg));
  
  // Code to set the low 32 bits of the value in register `dest'
  CreateSETUWConst(target, C, dest, mvec);
  
  // dest = OR(tmpReg, dest)
  mvec.push_back(BuildMI(V9::ORr,3).addReg(dest).addReg(tmpReg).addRegDef(dest));
}


//----------------------------------------------------------------------------
// Function: CreateSETUWLabel
// 
// Set a 32-bit constant (given by a symbolic label) in the register `dest'.
//----------------------------------------------------------------------------

static inline void
CreateSETUWLabel(const TargetMachine& target, Value* val,
                 Instruction* dest, std::vector<MachineInstr*>& mvec)
{
  MachineInstr* MI;
  
  // Set the high 22 bits in dest
  MI = BuildMI(V9::SETHI, 2).addReg(val).addRegDef(dest);
  MI->setOperandHi32(0);
  mvec.push_back(MI);
  
  // Set the low 10 bits in dest
  MI = BuildMI(V9::ORr, 3).addReg(dest).addReg(val).addRegDef(dest);
  MI->setOperandLo32(1);
  mvec.push_back(MI);
}


//----------------------------------------------------------------------------
// Function: CreateSETXLabel
// 
// Set a 64-bit constant (given by a symbolic label) in the register `dest'.
//----------------------------------------------------------------------------

static inline void
CreateSETXLabel(const TargetMachine& target,
                Value* val, Instruction* tmpReg, Instruction* dest,
                std::vector<MachineInstr*>& mvec)
{
  assert(isa<Constant>(val) || isa<GlobalValue>(val) &&
         "I only know about constant values and global addresses");
  
  MachineInstr* MI;
  
  MI = BuildMI(V9::SETHI, 2).addPCDisp(val).addRegDef(tmpReg);
  MI->setOperandHi64(0);
  mvec.push_back(MI);
  
  MI = BuildMI(V9::ORi, 3).addReg(tmpReg).addPCDisp(val).addRegDef(tmpReg);
  MI->setOperandLo64(1);
  mvec.push_back(MI);
  
  mvec.push_back(BuildMI(V9::SLLXi6, 3).addReg(tmpReg).addZImm(32)
                 .addRegDef(tmpReg));
  MI = BuildMI(V9::SETHI, 2).addPCDisp(val).addRegDef(dest);
  MI->setOperandHi32(0);
  mvec.push_back(MI);
  
  MI = BuildMI(V9::ORr, 3).addReg(dest).addReg(tmpReg).addRegDef(dest);
  mvec.push_back(MI);
  
  MI = BuildMI(V9::ORi, 3).addReg(dest).addPCDisp(val).addRegDef(dest);
  MI->setOperandLo32(1);
  mvec.push_back(MI);
}


//----------------------------------------------------------------------------
// Function: CreateUIntSetInstruction
// 
// Create code to Set an unsigned constant in the register `dest'.
// Uses CreateSETUWConst, CreateSETSWConst or CreateSETXConst as needed.
// CreateSETSWConst is an optimization for the case that the unsigned value
// has all ones in the 33 high bits (so that sign-extension sets them all).
//----------------------------------------------------------------------------

static inline void
CreateUIntSetInstruction(const TargetMachine& target,
                         uint64_t C, Instruction* dest,
                         std::vector<MachineInstr*>& mvec,
                         MachineCodeForInstruction& mcfi)
{
  static const uint64_t lo32 = (uint32_t) ~0;
  if (C <= lo32)                        // High 32 bits are 0.  Set low 32 bits.
    CreateSETUWConst(target, (uint32_t) C, dest, mvec);
  else if ((C & ~lo32) == ~lo32 && (C & (1U << 31))) {
    // All high 33 (not 32) bits are 1s: sign-extension will take care
    // of high 32 bits, so use the sequence for signed int
    CreateSETSWConst(target, (int32_t) C, dest, mvec);
  } else if (C > lo32) {
    // C does not fit in 32 bits
    TmpInstruction* tmpReg = new TmpInstruction(mcfi, Type::IntTy);
    CreateSETXConst(target, C, tmpReg, dest, mvec);
  }
}


//----------------------------------------------------------------------------
// Function: CreateIntSetInstruction
// 
// Create code to Set a signed constant in the register `dest'.
// Really the same as CreateUIntSetInstruction.
//----------------------------------------------------------------------------

static inline void
CreateIntSetInstruction(const TargetMachine& target,
                        int64_t C, Instruction* dest,
                        std::vector<MachineInstr*>& mvec,
                        MachineCodeForInstruction& mcfi)
{
  CreateUIntSetInstruction(target, (uint64_t) C, dest, mvec, mcfi);
}


//---------------------------------------------------------------------------
// Create a table of LLVM opcode -> max. immediate constant likely to
// be usable for that operation.
//---------------------------------------------------------------------------

// Entry == 0 ==> no immediate constant field exists at all.
// Entry >  0 ==> abs(immediate constant) <= Entry
// 
std::vector<int> MaxConstantsTable(Instruction::OtherOpsEnd);

static int
MaxConstantForInstr(unsigned llvmOpCode)
{
  int modelOpCode = -1;

  if (llvmOpCode >= Instruction::BinaryOpsBegin &&
      llvmOpCode <  Instruction::BinaryOpsEnd)
    modelOpCode = V9::ADDi;
  else
    switch(llvmOpCode) {
    case Instruction::Ret:   modelOpCode = V9::JMPLCALLi; break;

    case Instruction::Malloc:         
    case Instruction::Alloca:         
    case Instruction::GetElementPtr:  
    case Instruction::PHINode:       
    case Instruction::Cast:
    case Instruction::Call:  modelOpCode = V9::ADDi; break;

    case Instruction::Shl:
    case Instruction::Shr:   modelOpCode = V9::SLLXi6; break;

    default: break;
    };

  return (modelOpCode < 0)? 0: SparcMachineInstrDesc[modelOpCode].maxImmedConst;
}

static void
InitializeMaxConstantsTable()
{
  unsigned op;
  assert(MaxConstantsTable.size() == Instruction::OtherOpsEnd &&
         "assignments below will be illegal!");
  for (op = Instruction::TermOpsBegin; op < Instruction::TermOpsEnd; ++op)
    MaxConstantsTable[op] = MaxConstantForInstr(op);
  for (op = Instruction::BinaryOpsBegin; op < Instruction::BinaryOpsEnd; ++op)
    MaxConstantsTable[op] = MaxConstantForInstr(op);
  for (op = Instruction::MemoryOpsBegin; op < Instruction::MemoryOpsEnd; ++op)
    MaxConstantsTable[op] = MaxConstantForInstr(op);
  for (op = Instruction::OtherOpsBegin; op < Instruction::OtherOpsEnd; ++op)
    MaxConstantsTable[op] = MaxConstantForInstr(op);
}


//---------------------------------------------------------------------------
// class UltraSparcInstrInfo 
// 
// Purpose:
//   Information about individual instructions.
//   Most information is stored in the SparcMachineInstrDesc array above.
//   Other information is computed on demand, and most such functions
//   default to member functions in base class TargetInstrInfo. 
//---------------------------------------------------------------------------

/*ctor*/
UltraSparcInstrInfo::UltraSparcInstrInfo()
  : TargetInstrInfo(SparcMachineInstrDesc,
                    /*descSize = */ V9::NUM_TOTAL_OPCODES,
                    /*numRealOpCodes = */ V9::NUM_REAL_OPCODES)
{
  InitializeMaxConstantsTable();
}

bool
UltraSparcInstrInfo::ConstantMayNotFitInImmedField(const Constant* CV,
                                                   const Instruction* I) const
{
  if (I->getOpcode() >= MaxConstantsTable.size()) // user-defined op (or bug!)
    return true;

  if (isa<ConstantPointerNull>(CV))               // can always use %g0
    return false;

  if (const ConstantUInt* U = dyn_cast<ConstantUInt>(CV))
    /* Large unsigned longs may really just be small negative signed longs */
    return (labs((int64_t) U->getValue()) > MaxConstantsTable[I->getOpcode()]);

  if (const ConstantSInt* S = dyn_cast<ConstantSInt>(CV))
    return (labs(S->getValue()) > MaxConstantsTable[I->getOpcode()]);

  if (isa<ConstantBool>(CV))
    return (1 > MaxConstantsTable[I->getOpcode()]);

  return true;
}

// 
// Create an instruction sequence to put the constant `val' into
// the virtual register `dest'.  `val' may be a Constant or a
// GlobalValue, viz., the constant address of a global variable or function.
// The generated instructions are returned in `mvec'.
// Any temp. registers (TmpInstruction) created are recorded in mcfi.
// Any stack space required is allocated via MachineFunction.
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
  
  // Unfortunate special case: a ConstantPointerRef is just a
  // reference to GlobalValue.
  if (isa<ConstantPointerRef>(val))
    val = cast<ConstantPointerRef>(val)->getValue();

  if (isa<GlobalValue>(val)) {
      TmpInstruction* tmpReg =
        new TmpInstruction(mcfi, PointerType::get(val->getType()), val);
      CreateSETXLabel(target, val, tmpReg, dest, mvec);
  } else if (valType->isIntegral()) {
    bool isValidConstant;
    unsigned opSize = target.getTargetData().getTypeSize(val->getType());
    unsigned destSize = target.getTargetData().getTypeSize(dest->getType());
      
    if (! dest->getType()->isSigned()) {
      uint64_t C = GetConstantValueAsUnsignedInt(val, isValidConstant);
      assert(isValidConstant && "Unrecognized constant");

      if (opSize > destSize || (val->getType()->isSigned() && destSize < 8)) {
        // operand is larger than dest,
        //    OR both are equal but smaller than the full register size
        //       AND operand is signed, so it may have extra sign bits:
        // mask high bits
        C = C & ((1U << 8*destSize) - 1);
      }
      CreateUIntSetInstruction(target, C, dest, mvec, mcfi);
    } else {
      int64_t C = GetConstantValueAsSignedInt(val, isValidConstant);
      assert(isValidConstant && "Unrecognized constant");

      if (opSize > destSize)
        // operand is larger than dest: mask high bits
        C = C & ((1U << 8*destSize) - 1);

      if (opSize > destSize ||
          (opSize == destSize && !val->getType()->isSigned()))
        // sign-extend from destSize to 64 bits
        C = ((C & (1U << (8*destSize - 1)))
             ? C | ~((1U << 8*destSize) - 1)
             : C);
          
      CreateIntSetInstruction(target, C, dest, mvec, mcfi);
    }
  } else {
    // Make an instruction sequence to load the constant, viz:
    //            SETX <addr-of-constant>, tmpReg, addrReg
    //            LOAD  /*addr*/ addrReg, /*offset*/ 0, dest
      
    // First, create a tmp register to be used by the SETX sequence.
    TmpInstruction* tmpReg =
      new TmpInstruction(mcfi, PointerType::get(val->getType()), val);
      
    // Create another TmpInstruction for the address register
    TmpInstruction* addrReg =
      new TmpInstruction(mcfi, PointerType::get(val->getType()), val);
      
    // Put the address (a symbolic name) into a register
    CreateSETXLabel(target, val, tmpReg, addrReg, mvec);
      
    // Generate the load instruction
    int64_t zeroOffset = 0;           // to avoid ambiguity with (Value*) 0
    unsigned Opcode = ChooseLoadInstruction(val->getType());
    Opcode = convertOpcodeFromRegToImm(Opcode);
    mvec.push_back(BuildMI(Opcode, 3).addReg(addrReg).
                   addSImm(zeroOffset).addRegDef(dest));
      
    // Make sure constant is emitted to constant pool in assembly code.
    MachineFunction::get(F).getInfo()->addToConstantPool(cast<Constant>(val));
  }
}


// Create an instruction sequence to copy an integer register `val'
// to a floating point register `dest' by copying to memory and back.
// val must be an integral type.  dest must be a Float or Double.
// The generated instructions are returned in `mvec'.
// Any temp. registers (TmpInstruction) created are recorded in mcfi.
// Any stack space required is allocated via MachineFunction.
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
         && "Source type must be integral (integer or bool) or pointer");
  assert(dest->getType()->isFloatingPoint()
         && "Dest type must be float/double");

  // Get a stack slot to use for the copy
  int offset = MachineFunction::get(F).getInfo()->allocateLocalVar(val);

  // Get the size of the source value being copied. 
  size_t srcSize = target.getTargetData().getTypeSize(val->getType());

  // Store instruction stores `val' to [%fp+offset].
  // The store and load opCodes are based on the size of the source value.
  // If the value is smaller than 32 bits, we must sign- or zero-extend it
  // to 32 bits since the load-float will load 32 bits.
  // Note that the store instruction is the same for signed and unsigned ints.
  const Type* storeType = (srcSize <= 4)? Type::IntTy : Type::LongTy;
  Value* storeVal = val;
  if (srcSize < target.getTargetData().getTypeSize(Type::FloatTy)) {
    // sign- or zero-extend respectively
    storeVal = new TmpInstruction(mcfi, storeType, val);
    if (val->getType()->isSigned())
      CreateSignExtensionInstructions(target, F, val, storeVal, 8*srcSize,
                                      mvec, mcfi);
    else
      CreateZeroExtensionInstructions(target, F, val, storeVal, 8*srcSize,
                                      mvec, mcfi);
  }

  unsigned FPReg = target.getRegInfo().getFramePointer();
  unsigned StoreOpcode = ChooseStoreInstruction(storeType);
  StoreOpcode = convertOpcodeFromRegToImm(StoreOpcode);
  mvec.push_back(BuildMI(StoreOpcode, 3)
                 .addReg(storeVal).addMReg(FPReg).addSImm(offset));

  // Load instruction loads [%fp+offset] to `dest'.
  // The type of the load opCode is the floating point type that matches the
  // stored type in size:
  // On SparcV9: float for int or smaller, double for long.
  // 
  const Type* loadType = (srcSize <= 4)? Type::FloatTy : Type::DoubleTy;
  unsigned LoadOpcode = ChooseLoadInstruction(loadType);
  LoadOpcode = convertOpcodeFromRegToImm(LoadOpcode);
  mvec.push_back(BuildMI(LoadOpcode, 3)
                 .addMReg(FPReg).addSImm(offset).addRegDef(dest));
}

// Similarly, create an instruction sequence to copy an FP register
// `val' to an integer register `dest' by copying to memory and back.
// The generated instructions are returned in `mvec'.
// Any temp. virtual registers (TmpInstruction) created are recorded in mcfi.
// Temporary stack space required is allocated via MachineFunction.
// 
void
UltraSparcInstrInfo::CreateCodeToCopyFloatToInt(const TargetMachine& target,
                                        Function* F,
                                        Value* val,
                                        Instruction* dest,
                                        std::vector<MachineInstr*>& mvec,
                                        MachineCodeForInstruction& mcfi) const
{
  const Type* opTy   = val->getType();
  const Type* destTy = dest->getType();

  assert(opTy->isFloatingPoint() && "Source type must be float/double");
  assert((destTy->isIntegral() || isa<PointerType>(destTy))
         && "Dest type must be integer, bool or pointer");

  // FIXME: For now, we allocate permanent space because the stack frame
  // manager does not allow locals to be allocated (e.g., for alloca) after
  // a temp is allocated!
  // 
  int offset = MachineFunction::get(F).getInfo()->allocateLocalVar(val); 

  unsigned FPReg = target.getRegInfo().getFramePointer();

  // Store instruction stores `val' to [%fp+offset].
  // The store opCode is based only the source value being copied.
  // 
  unsigned StoreOpcode = ChooseStoreInstruction(opTy);
  StoreOpcode = convertOpcodeFromRegToImm(StoreOpcode);  
  mvec.push_back(BuildMI(StoreOpcode, 3)
                 .addReg(val).addMReg(FPReg).addSImm(offset));

  // Load instruction loads [%fp+offset] to `dest'.
  // The type of the load opCode is the integer type that matches the
  // source type in size:
  // On SparcV9: int for float, long for double.
  // Note that we *must* use signed loads even for unsigned dest types, to
  // ensure correct sign-extension for UByte, UShort or UInt:
  // 
  const Type* loadTy = (opTy == Type::FloatTy)? Type::IntTy : Type::LongTy;
  unsigned LoadOpcode = ChooseLoadInstruction(loadTy);
  LoadOpcode = convertOpcodeFromRegToImm(LoadOpcode);
  mvec.push_back(BuildMI(LoadOpcode, 3).addMReg(FPReg)
                 .addSImm(offset).addRegDef(dest));
}


// Create instruction(s) to copy src to dest, for arbitrary types
// The generated instructions are returned in `mvec'.
// Any temp. registers (TmpInstruction) created are recorded in mcfi.
// Any stack space required is allocated via MachineFunction.
// 
void
UltraSparcInstrInfo::CreateCopyInstructionsByType(const TargetMachine& target,
                                                  Function *F,
                                                  Value* src,
                                                  Instruction* dest,
                                               std::vector<MachineInstr*>& mvec,
                                          MachineCodeForInstruction& mcfi) const
{
  bool loadConstantToReg = false;
  
  const Type* resultType = dest->getType();
  
  MachineOpCode opCode = ChooseAddInstructionByType(resultType);
  if (opCode == V9::INVALID_OPCODE) {
    assert(0 && "Unsupported result type in CreateCopyInstructionsByType()");
    return;
  }
  
  // if `src' is a constant that doesn't fit in the immed field or if it is
  // a global variable (i.e., a constant address), generate a load
  // instruction instead of an add
  // 
  if (isa<Constant>(src)) {
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
  
  if (loadConstantToReg) { 
    // `src' is constant and cannot fit in immed field for the ADD
    // Insert instructions to "load" the constant into a register
    target.getInstrInfo().CreateCodeToLoadConst(target, F, src, dest,
                                                mvec, mcfi);
  } else { 
    // Create a reg-to-reg copy instruction for the given type:
    // -- For FP values, create a FMOVS or FMOVD instruction
    // -- For non-FP values, create an add-with-0 instruction (opCode as above)
    // Make `src' the second operand, in case it is a small constant!
    // 
    MachineInstr* MI;
    if (resultType->isFloatingPoint())
      MI = (BuildMI(resultType == Type::FloatTy? V9::FMOVS : V9::FMOVD, 2)
            .addReg(src).addRegDef(dest));
    else {
        const Type* Ty =isa<PointerType>(resultType)? Type::ULongTy :resultType;
        MI = (BuildMI(opCode, 3)
              .addSImm((int64_t) 0).addReg(src).addRegDef(dest));
    }
    mvec.push_back(MI);
  }
}


// Helper function for sign-extension and zero-extension.
// For SPARC v9, we sign-extend the given operand using SLL; SRA/SRL.
inline void
CreateBitExtensionInstructions(bool signExtend,
                               const TargetMachine& target,
                               Function* F,
                               Value* srcVal,
                               Value* destVal,
                               unsigned int numLowBits,
                               std::vector<MachineInstr*>& mvec,
                               MachineCodeForInstruction& mcfi)
{
  MachineInstr* M;

  assert(numLowBits <= 32 && "Otherwise, nothing should be done here!");

  if (numLowBits < 32) {
    // SLL is needed since operand size is < 32 bits.
    TmpInstruction *tmpI = new TmpInstruction(mcfi, destVal->getType(),
                                              srcVal, destVal, "make32");
    mvec.push_back(BuildMI(V9::SLLXi6, 3).addReg(srcVal)
                   .addZImm(32-numLowBits).addRegDef(tmpI));
    srcVal = tmpI;
  }

  mvec.push_back(BuildMI(signExtend? V9::SRAi5 : V9::SRLi5, 3)
                 .addReg(srcVal).addZImm(32-numLowBits).addRegDef(destVal));
}


// Create instruction sequence to produce a sign-extended register value
// from an arbitrary-sized integer value (sized in bits, not bytes).
// The generated instructions are returned in `mvec'.
// Any temp. registers (TmpInstruction) created are recorded in mcfi.
// Any stack space required is allocated via MachineFunction.
// 
void
UltraSparcInstrInfo::CreateSignExtensionInstructions(
                                        const TargetMachine& target,
                                        Function* F,
                                        Value* srcVal,
                                        Value* destVal,
                                        unsigned int numLowBits,
                                        std::vector<MachineInstr*>& mvec,
                                        MachineCodeForInstruction& mcfi) const
{
  CreateBitExtensionInstructions(/*signExtend*/ true, target, F, srcVal,
                                 destVal, numLowBits, mvec, mcfi);
}


// Create instruction sequence to produce a zero-extended register value
// from an arbitrary-sized integer value (sized in bits, not bytes).
// For SPARC v9, we sign-extend the given operand using SLL; SRL.
// The generated instructions are returned in `mvec'.
// Any temp. registers (TmpInstruction) created are recorded in mcfi.
// Any stack space required is allocated via MachineFunction.
// 
void
UltraSparcInstrInfo::CreateZeroExtensionInstructions(
                                        const TargetMachine& target,
                                        Function* F,
                                        Value* srcVal,
                                        Value* destVal,
                                        unsigned int numLowBits,
                                        std::vector<MachineInstr*>& mvec,
                                        MachineCodeForInstruction& mcfi) const
{
  CreateBitExtensionInstructions(/*signExtend*/ false, target, F, srcVal,
                                 destVal, numLowBits, mvec, mcfi);
}
