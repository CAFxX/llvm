//===-- SparcInstrSelection.cpp -------------------------------------------===//
//
//  BURS instruction selection for SPARC V9 architecture.      
//
//===----------------------------------------------------------------------===//

#include "SparcInternals.h"
#include "SparcInstrSelectionSupport.h"
#include "SparcRegClassInfo.h"
#include "llvm/CodeGen/InstrSelectionSupport.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrAnnot.h"
#include "llvm/CodeGen/InstrForest.h"
#include "llvm/CodeGen/InstrSelection.h"
#include "llvm/CodeGen/MachineCodeForMethod.h"
#include "llvm/CodeGen/MachineCodeForInstruction.h"
#include "llvm/DerivedTypes.h"
#include "llvm/iTerminators.h"
#include "llvm/iMemory.h"
#include "llvm/iOther.h"
#include "llvm/Function.h"
#include "llvm/Constants.h"
#include "Support/MathExtras.h"
#include <math.h>
using std::vector;

//************************ Internal Functions ******************************/


static inline MachineOpCode 
ChooseBprInstruction(const InstructionNode* instrNode)
{
  MachineOpCode opCode;
  
  Instruction* setCCInstr =
    ((InstructionNode*) instrNode->leftChild())->getInstruction();
  
  switch(setCCInstr->getOpcode())
    {
    case Instruction::SetEQ: opCode = BRZ;   break;
    case Instruction::SetNE: opCode = BRNZ;  break;
    case Instruction::SetLE: opCode = BRLEZ; break;
    case Instruction::SetGE: opCode = BRGEZ; break;
    case Instruction::SetLT: opCode = BRLZ;  break;
    case Instruction::SetGT: opCode = BRGZ;  break;
    default:
      assert(0 && "Unrecognized VM instruction!");
      opCode = INVALID_OPCODE;
      break; 
    }
  
  return opCode;
}


static inline MachineOpCode 
ChooseBpccInstruction(const InstructionNode* instrNode,
                      const BinaryOperator* setCCInstr)
{
  MachineOpCode opCode = INVALID_OPCODE;
  
  bool isSigned = setCCInstr->getOperand(0)->getType()->isSigned();
  
  if (isSigned)
    {
      switch(setCCInstr->getOpcode())
        {
        case Instruction::SetEQ: opCode = BE;  break;
        case Instruction::SetNE: opCode = BNE; break;
        case Instruction::SetLE: opCode = BLE; break;
        case Instruction::SetGE: opCode = BGE; break;
        case Instruction::SetLT: opCode = BL;  break;
        case Instruction::SetGT: opCode = BG;  break;
        default:
          assert(0 && "Unrecognized VM instruction!");
          break; 
        }
    }
  else
    {
      switch(setCCInstr->getOpcode())
        {
        case Instruction::SetEQ: opCode = BE;   break;
        case Instruction::SetNE: opCode = BNE;  break;
        case Instruction::SetLE: opCode = BLEU; break;
        case Instruction::SetGE: opCode = BCC;  break;
        case Instruction::SetLT: opCode = BCS;  break;
        case Instruction::SetGT: opCode = BGU;  break;
        default:
          assert(0 && "Unrecognized VM instruction!");
          break; 
        }
    }
  
  return opCode;
}

static inline MachineOpCode 
ChooseBFpccInstruction(const InstructionNode* instrNode,
                       const BinaryOperator* setCCInstr)
{
  MachineOpCode opCode = INVALID_OPCODE;
  
  switch(setCCInstr->getOpcode())
    {
    case Instruction::SetEQ: opCode = FBE;  break;
    case Instruction::SetNE: opCode = FBNE; break;
    case Instruction::SetLE: opCode = FBLE; break;
    case Instruction::SetGE: opCode = FBGE; break;
    case Instruction::SetLT: opCode = FBL;  break;
    case Instruction::SetGT: opCode = FBG;  break;
    default:
      assert(0 && "Unrecognized VM instruction!");
      break; 
    }
  
  return opCode;
}


// Create a unique TmpInstruction for a boolean value,
// representing the CC register used by a branch on that value.
// For now, hack this using a little static cache of TmpInstructions.
// Eventually the entire BURG instruction selection should be put
// into a separate class that can hold such information.
// The static cache is not too bad because the memory for these
// TmpInstructions will be freed along with the rest of the Function anyway.
// 
static TmpInstruction*
GetTmpForCC(Value* boolVal, const Function *F, const Type* ccType)
{
  typedef hash_map<const Value*, TmpInstruction*> BoolTmpCache;
  static BoolTmpCache boolToTmpCache;     // Map boolVal -> TmpInstruction*
  static const Function *lastFunction = 0;// Use to flush cache between funcs
  
  assert(boolVal->getType() == Type::BoolTy && "Weird but ok! Delete assert");
  
  if (lastFunction != F)
    {
      lastFunction = F;
      boolToTmpCache.clear();
    }
  
  // Look for tmpI and create a new one otherwise.  The new value is
  // directly written to map using the ref returned by operator[].
  TmpInstruction*& tmpI = boolToTmpCache[boolVal];
  if (tmpI == NULL)
    tmpI = new TmpInstruction(ccType, boolVal);
  
  return tmpI;
}


static inline MachineOpCode 
ChooseBccInstruction(const InstructionNode* instrNode,
                     bool& isFPBranch)
{
  InstructionNode* setCCNode = (InstructionNode*) instrNode->leftChild();
  assert(setCCNode->getOpLabel() == SetCCOp);
  BinaryOperator* setCCInstr =cast<BinaryOperator>(setCCNode->getInstruction());
  const Type* setCCType = setCCInstr->getOperand(0)->getType();
  
  isFPBranch = setCCType->isFloatingPoint(); // Return value: don't delete!
  
  if (isFPBranch)
    return ChooseBFpccInstruction(instrNode, setCCInstr);
  else
    return ChooseBpccInstruction(instrNode, setCCInstr);
}


static inline MachineOpCode 
ChooseMovFpccInstruction(const InstructionNode* instrNode)
{
  MachineOpCode opCode = INVALID_OPCODE;
  
  switch(instrNode->getInstruction()->getOpcode())
    {
    case Instruction::SetEQ: opCode = MOVFE;  break;
    case Instruction::SetNE: opCode = MOVFNE; break;
    case Instruction::SetLE: opCode = MOVFLE; break;
    case Instruction::SetGE: opCode = MOVFGE; break;
    case Instruction::SetLT: opCode = MOVFL;  break;
    case Instruction::SetGT: opCode = MOVFG;  break;
    default:
      assert(0 && "Unrecognized VM instruction!");
      break; 
    }
  
  return opCode;
}


// Assumes that SUBcc v1, v2 -> v3 has been executed.
// In most cases, we want to clear v3 and then follow it by instruction
// MOVcc 1 -> v3.
// Set mustClearReg=false if v3 need not be cleared before conditional move.
// Set valueToMove=0 if we want to conditionally move 0 instead of 1
//                      (i.e., we want to test inverse of a condition)
// (The latter two cases do not seem to arise because SetNE needs nothing.)
// 
static MachineOpCode
ChooseMovpccAfterSub(const InstructionNode* instrNode,
                     bool& mustClearReg,
                     int& valueToMove)
{
  MachineOpCode opCode = INVALID_OPCODE;
  mustClearReg = true;
  valueToMove = 1;
  
  switch(instrNode->getInstruction()->getOpcode())
    {
    case Instruction::SetEQ: opCode = MOVE;  break;
    case Instruction::SetLE: opCode = MOVLE; break;
    case Instruction::SetGE: opCode = MOVGE; break;
    case Instruction::SetLT: opCode = MOVL;  break;
    case Instruction::SetGT: opCode = MOVG;  break;
    case Instruction::SetNE: assert(0 && "No move required!"); break;
    default:		     assert(0 && "Unrecognized VM instr!"); break; 
    }
  
  return opCode;
}

static inline MachineOpCode
ChooseConvertToFloatInstr(OpLabel vopCode, const Type* opType)
{
  MachineOpCode opCode = INVALID_OPCODE;
  
  switch(vopCode)
    {
    case ToFloatTy: 
      if (opType == Type::SByteTy || opType == Type::ShortTy || opType == Type::IntTy)
        opCode = FITOS;
      else if (opType == Type::LongTy)
        opCode = FXTOS;
      else if (opType == Type::DoubleTy)
        opCode = FDTOS;
      else if (opType == Type::FloatTy)
        ;
      else
        assert(0 && "Cannot convert this type to FLOAT on SPARC");
      break;
      
    case ToDoubleTy: 
      // This is usually used in conjunction with CreateCodeToCopyIntToFloat().
      // Both functions should treat the integer as a 32-bit value for types
      // of 4 bytes or less, and as a 64-bit value otherwise.
      if (opType == Type::SByteTy || opType == Type::UByteTy ||
          opType == Type::ShortTy || opType == Type::UShortTy ||
          opType == Type::IntTy   || opType == Type::UIntTy)
        opCode = FITOD;
      else if (opType == Type::LongTy || opType == Type::ULongTy)
        opCode = FXTOD;
      else if (opType == Type::FloatTy)
        opCode = FSTOD;
      else if (opType == Type::DoubleTy)
        ;
      else
        assert(0 && "Cannot convert this type to DOUBLE on SPARC");
      break;
      
    default:
      break;
    }
  
  return opCode;
}

static inline MachineOpCode 
ChooseConvertToIntInstr(Type::PrimitiveID tid, const Type* opType)
{
  MachineOpCode opCode = INVALID_OPCODE;;
  
  if (tid==Type::SByteTyID || tid==Type::ShortTyID  || tid==Type::IntTyID ||
      tid==Type::UByteTyID || tid==Type::UShortTyID || tid==Type::UIntTyID)
    {
      switch (opType->getPrimitiveID())
        {
        case Type::FloatTyID:   opCode = FSTOI; break;
        case Type::DoubleTyID:  opCode = FDTOI; break;
        default:
          assert(0 && "Non-numeric non-bool type cannot be converted to Int");
          break;
        }
    }
  else if (tid==Type::LongTyID || tid==Type::ULongTyID)
    {
      switch (opType->getPrimitiveID())
        {
        case Type::FloatTyID:   opCode = FSTOX; break;
        case Type::DoubleTyID:  opCode = FDTOX; break;
        default:
          assert(0 && "Non-numeric non-bool type cannot be converted to Long");
          break;
        }
    }
  else
      assert(0 && "Should not get here, Mo!");
  
  return opCode;
}

MachineInstr*
CreateConvertToIntInstr(Type::PrimitiveID destTID, Value* srcVal,Value* destVal)
{
  MachineOpCode opCode = ChooseConvertToIntInstr(destTID, srcVal->getType());
  assert(opCode != INVALID_OPCODE && "Expected to need conversion!");
  
  MachineInstr* M = new MachineInstr(opCode);
  M->SetMachineOperandVal(0, MachineOperand::MO_VirtualRegister, srcVal);
  M->SetMachineOperandVal(1, MachineOperand::MO_VirtualRegister, destVal);
  return M;
}

// CreateCodeToConvertFloatToInt: Convert FP value to signed or unsigned integer
// The FP value must be converted to the dest type in an FP register,
// and the result is then copied from FP to int register via memory.
//
// Since fdtoi converts to signed integers, any FP value V between MAXINT+1
// and MAXUNSIGNED (i.e., 2^31 <= V <= 2^32-1) would be converted incorrectly
// *only* when converting to an unsigned int.  (Unsigned byte, short or long
// don't have this problem.)
// For unsigned int, we therefore have to generate the code sequence:
// 
//      if (V > (float) MAXINT) {
//        unsigned result = (unsigned) (V  - (float) MAXINT);
//        result = result + (unsigned) MAXINT;
//      }
//      else
//        result = (unsigned int) V;
// 
static void
CreateCodeToConvertFloatToInt(const TargetMachine& target,
                              Value* opVal,
                              Instruction* destI,
                              std::vector<MachineInstr*>& mvec,
                              MachineCodeForInstruction& mcfi)
{
  // Create a temporary to represent the FP register into which the
  // int value will placed after conversion.  The type of this temporary
  // depends on the type of FP register to use: single-prec for a 32-bit
  // int or smaller; double-prec for a 64-bit int.
  // 
  size_t destSize = target.DataLayout.getTypeSize(destI->getType());
  const Type* destTypeToUse = (destSize > 4)? Type::DoubleTy : Type::FloatTy;
  TmpInstruction* destForCast = new TmpInstruction(destTypeToUse, opVal);
  mcfi.addTemp(destForCast);

  // Create the fp-to-int conversion code
  MachineInstr* M = CreateConvertToIntInstr(destI->getType()->getPrimitiveID(),
                                            opVal, destForCast);
  mvec.push_back(M);

  // Create the fpreg-to-intreg copy code
  target.getInstrInfo().
    CreateCodeToCopyFloatToInt(target, destI->getParent()->getParent(),
                               destForCast, destI, mvec, mcfi);
}


static inline MachineOpCode 
ChooseAddInstruction(const InstructionNode* instrNode)
{
  return ChooseAddInstructionByType(instrNode->getInstruction()->getType());
}


static inline MachineInstr* 
CreateMovFloatInstruction(const InstructionNode* instrNode,
                          const Type* resultType)
{
  MachineInstr* minstr = new MachineInstr((resultType == Type::FloatTy)
                                          ? FMOVS : FMOVD);
  minstr->SetMachineOperandVal(0, MachineOperand::MO_VirtualRegister,
                               instrNode->leftChild()->getValue());
  minstr->SetMachineOperandVal(1, MachineOperand::MO_VirtualRegister,
                               instrNode->getValue());
  return minstr;
}

static inline MachineInstr* 
CreateAddConstInstruction(const InstructionNode* instrNode)
{
  MachineInstr* minstr = NULL;
  
  Value* constOp = ((InstrTreeNode*) instrNode->rightChild())->getValue();
  assert(isa<Constant>(constOp));
  
  // Cases worth optimizing are:
  // (1) Add with 0 for float or double: use an FMOV of appropriate type,
  //	 instead of an FADD (1 vs 3 cycles).  There is no integer MOV.
  // 
  if (ConstantFP *FPC = dyn_cast<ConstantFP>(constOp)) {
      double dval = FPC->getValue();
      if (dval == 0.0)
        minstr = CreateMovFloatInstruction(instrNode,
                                   instrNode->getInstruction()->getType());
    }
  
  return minstr;
}


static inline MachineOpCode 
ChooseSubInstructionByType(const Type* resultType)
{
  MachineOpCode opCode = INVALID_OPCODE;
  
  if (resultType->isInteger() || isa<PointerType>(resultType))
    {
      opCode = SUB;
    }
  else
    switch(resultType->getPrimitiveID())
      {
      case Type::FloatTyID:  opCode = FSUBS; break;
      case Type::DoubleTyID: opCode = FSUBD; break;
      default: assert(0 && "Invalid type for SUB instruction"); break; 
      }
  
  return opCode;
}


static inline MachineInstr* 
CreateSubConstInstruction(const InstructionNode* instrNode)
{
  MachineInstr* minstr = NULL;
  
  Value* constOp = ((InstrTreeNode*) instrNode->rightChild())->getValue();
  assert(isa<Constant>(constOp));
  
  // Cases worth optimizing are:
  // (1) Sub with 0 for float or double: use an FMOV of appropriate type,
  //	 instead of an FSUB (1 vs 3 cycles).  There is no integer MOV.
  // 
  if (ConstantFP *FPC = dyn_cast<ConstantFP>(constOp)) {
    double dval = FPC->getValue();
    if (dval == 0.0)
      minstr = CreateMovFloatInstruction(instrNode,
                                        instrNode->getInstruction()->getType());
  }
  
  return minstr;
}


static inline MachineOpCode 
ChooseFcmpInstruction(const InstructionNode* instrNode)
{
  MachineOpCode opCode = INVALID_OPCODE;
  
  Value* operand = ((InstrTreeNode*) instrNode->leftChild())->getValue();
  switch(operand->getType()->getPrimitiveID()) {
  case Type::FloatTyID:  opCode = FCMPS; break;
  case Type::DoubleTyID: opCode = FCMPD; break;
  default: assert(0 && "Invalid type for FCMP instruction"); break; 
  }
  
  return opCode;
}


// Assumes that leftArg and rightArg are both cast instructions.
//
static inline bool
BothFloatToDouble(const InstructionNode* instrNode)
{
  InstrTreeNode* leftArg = instrNode->leftChild();
  InstrTreeNode* rightArg = instrNode->rightChild();
  InstrTreeNode* leftArgArg = leftArg->leftChild();
  InstrTreeNode* rightArgArg = rightArg->leftChild();
  assert(leftArg->getValue()->getType() == rightArg->getValue()->getType());
  
  // Check if both arguments are floats cast to double
  return (leftArg->getValue()->getType() == Type::DoubleTy &&
          leftArgArg->getValue()->getType() == Type::FloatTy &&
          rightArgArg->getValue()->getType() == Type::FloatTy);
}


static inline MachineOpCode 
ChooseMulInstructionByType(const Type* resultType)
{
  MachineOpCode opCode = INVALID_OPCODE;
  
  if (resultType->isInteger())
    opCode = MULX;
  else
    switch(resultType->getPrimitiveID())
      {
      case Type::FloatTyID:  opCode = FMULS; break;
      case Type::DoubleTyID: opCode = FMULD; break;
      default: assert(0 && "Invalid type for MUL instruction"); break; 
      }
  
  return opCode;
}



static inline MachineInstr*
CreateIntNegInstruction(const TargetMachine& target,
                        Value* vreg)
{
  MachineInstr* minstr = new MachineInstr(SUB);
  minstr->SetMachineOperandReg(0, target.getRegInfo().getZeroRegNum());
  minstr->SetMachineOperandVal(1, MachineOperand::MO_VirtualRegister, vreg);
  minstr->SetMachineOperandVal(2, MachineOperand::MO_VirtualRegister, vreg);
  return minstr;
}


// Create instruction sequence for any shift operation.
// SLL or SLLX on an operand smaller than the integer reg. size (64bits)
// requires a second instruction for explicit sign-extension.
// Note that we only have to worry about a sign-bit appearing in the
// most significant bit of the operand after shifting (e.g., bit 32 of
// Int or bit 16 of Short), so we do not have to worry about results
// that are as large as a normal integer register.
// 
static inline void
CreateShiftInstructions(const TargetMachine& target,
                        Function* F,
                        MachineOpCode shiftOpCode,
                        Value* argVal1,
                        Value* optArgVal2, /* Use optArgVal2 if not NULL */
                        unsigned int optShiftNum, /* else use optShiftNum */
                        Instruction* destVal,
                        vector<MachineInstr*>& mvec,
                        MachineCodeForInstruction& mcfi)
{
  assert((optArgVal2 != NULL || optShiftNum <= 64) &&
         "Large shift sizes unexpected, but can be handled below: "
         "You need to check whether or not it fits in immed field below");
  
  // If this is a logical left shift of a type smaller than the standard
  // integer reg. size, we have to extend the sign-bit into upper bits
  // of dest, so we need to put the result of the SLL into a temporary.
  // 
  Value* shiftDest = destVal;
  const Type* opType = argVal1->getType();
  unsigned opSize = target.DataLayout.getTypeSize(argVal1->getType());
  if ((shiftOpCode == SLL || shiftOpCode == SLLX)
      && opSize < target.DataLayout.getIntegerRegize())
    { // put SLL result into a temporary
      shiftDest = new TmpInstruction(argVal1, optArgVal2, "sllTmp");
      mcfi.addTemp(shiftDest);
    }
  
  MachineInstr* M = (optArgVal2 != NULL)
    ? Create3OperandInstr(shiftOpCode, argVal1, optArgVal2, shiftDest)
    : Create3OperandInstr_UImmed(shiftOpCode, argVal1, optShiftNum, shiftDest);
  mvec.push_back(M);
  
  if (shiftDest != destVal)
    { // extend the sign-bit of the result into all upper bits of dest
      assert(8*opSize <= 32 && "Unexpected type size > 4 and < IntRegSize?");
      target.getInstrInfo().
        CreateSignExtensionInstructions(target, F, shiftDest, 8*opSize,
                                        destVal, mvec, mcfi);
    }
}


// Does not create any instructions if we cannot exploit constant to
// create a cheaper instruction.
// This returns the approximate cost of the instructions generated,
// which is used to pick the cheapest when both operands are constant.
static inline unsigned int
CreateMulConstInstruction(const TargetMachine &target, Function* F,
                          Value* lval, Value* rval, Instruction* destVal,
                          vector<MachineInstr*>& mvec,
                          MachineCodeForInstruction& mcfi)
{
  /* Use max. multiply cost, viz., cost of MULX */
  unsigned int cost = target.getInstrInfo().minLatency(MULX);
  unsigned int firstNewInstr = mvec.size();
  
  Value* constOp = rval;
  if (! isa<Constant>(constOp))
    return cost;
  
  // Cases worth optimizing are:
  // (1) Multiply by 0 or 1 for any type: replace with copy (ADD or FMOV)
  // (2) Multiply by 2^x for integer types: replace with Shift
  // 
  const Type* resultType = destVal->getType();
  
  if (resultType->isInteger() || isa<PointerType>(resultType))
    {
      bool isValidConst;
      int64_t C = GetConstantValueAsSignedInt(constOp, isValidConst);
      if (isValidConst)
        {
          unsigned pow;
          bool needNeg = false;
          if (C < 0)
            {
              needNeg = true;
              C = -C;
            }
          
          if (C == 0 || C == 1)
            {
              cost = target.getInstrInfo().minLatency(ADD);
              MachineInstr* M = (C == 0)
                ? Create3OperandInstr_Reg(ADD,
                                          target.getRegInfo().getZeroRegNum(),
                                          target.getRegInfo().getZeroRegNum(),
                                          destVal)
                : Create3OperandInstr_Reg(ADD, lval,
                                          target.getRegInfo().getZeroRegNum(),
                                          destVal);
              mvec.push_back(M);
            }
          else if (isPowerOf2(C, pow))
            {
              unsigned int opSize = target.DataLayout.getTypeSize(resultType);
              MachineOpCode opCode = (opSize <= 32)? SLL : SLLX;
              CreateShiftInstructions(target, F, opCode, lval, NULL, pow,
                                      destVal, mvec, mcfi); 
            }
          
          if (mvec.size() > 0 && needNeg)
            { // insert <reg = SUB 0, reg> after the instr to flip the sign
              MachineInstr* M = CreateIntNegInstruction(target, destVal);
              mvec.push_back(M);
            }
        }
    }
  else
    {
      if (ConstantFP *FPC = dyn_cast<ConstantFP>(constOp))
        {
          double dval = FPC->getValue();
          if (fabs(dval) == 1)
            {
              MachineOpCode opCode =  (dval < 0)
                ? (resultType == Type::FloatTy? FNEGS : FNEGD)
                : (resultType == Type::FloatTy? FMOVS : FMOVD);
              MachineInstr* M = Create2OperandInstr(opCode, lval, destVal);
              mvec.push_back(M);
            } 
        }
    }
  
  if (firstNewInstr < mvec.size())
    {
      cost = 0;
      for (unsigned int i=firstNewInstr; i < mvec.size(); ++i)
        cost += target.getInstrInfo().minLatency(mvec[i]->getOpCode());
    }
  
  return cost;
}


// Does not create any instructions if we cannot exploit constant to
// create a cheaper instruction.
// 
static inline void
CreateCheapestMulConstInstruction(const TargetMachine &target,
                                  Function* F,
                                  Value* lval, Value* rval,
                                  Instruction* destVal,
                                  vector<MachineInstr*>& mvec,
                                  MachineCodeForInstruction& mcfi)
{
  Value* constOp;
  if (isa<Constant>(lval) && isa<Constant>(rval))
    { // both operands are constant: try both orders!
      vector<MachineInstr*> mvec1, mvec2;
      unsigned int lcost = CreateMulConstInstruction(target, F, lval, rval,
                                                     destVal, mvec1, mcfi);
      unsigned int rcost = CreateMulConstInstruction(target, F, rval, lval,
                                                     destVal, mvec2, mcfi);
      vector<MachineInstr*>& mincostMvec =  (lcost <= rcost)? mvec1 : mvec2;
      vector<MachineInstr*>& maxcostMvec =  (lcost <= rcost)? mvec2 : mvec1;
      mvec.insert(mvec.end(), mincostMvec.begin(), mincostMvec.end()); 

      for (unsigned int i=0; i < maxcostMvec.size(); ++i)
        delete maxcostMvec[i];
    }
  else if (isa<Constant>(rval))         // rval is constant, but not lval
    CreateMulConstInstruction(target, F, lval, rval, destVal, mvec, mcfi);
  else if (isa<Constant>(lval))         // lval is constant, but not rval
    CreateMulConstInstruction(target, F, lval, rval, destVal, mvec, mcfi);
  
  // else neither is constant
  return;
}

// Return NULL if we cannot exploit constant to create a cheaper instruction
static inline void
CreateMulInstruction(const TargetMachine &target, Function* F,
                     Value* lval, Value* rval, Instruction* destVal,
                     vector<MachineInstr*>& mvec,
                     MachineCodeForInstruction& mcfi,
                     MachineOpCode forceMulOp = INVALID_MACHINE_OPCODE)
{
  unsigned int L = mvec.size();
  CreateCheapestMulConstInstruction(target,F, lval, rval, destVal, mvec, mcfi);
  if (mvec.size() == L)
    { // no instructions were added so create MUL reg, reg, reg.
      // Use FSMULD if both operands are actually floats cast to doubles.
      // Otherwise, use the default opcode for the appropriate type.
      MachineOpCode mulOp = ((forceMulOp != INVALID_MACHINE_OPCODE)
                             ? forceMulOp 
                             : ChooseMulInstructionByType(destVal->getType()));
      MachineInstr* M = new MachineInstr(mulOp);
      M->SetMachineOperandVal(0, MachineOperand::MO_VirtualRegister, lval);
      M->SetMachineOperandVal(1, MachineOperand::MO_VirtualRegister, rval);
      M->SetMachineOperandVal(2, MachineOperand::MO_VirtualRegister, destVal);
      mvec.push_back(M);
    }
}


// Generate a divide instruction for Div or Rem.
// For Rem, this assumes that the operand type will be signed if the result
// type is signed.  This is correct because they must have the same sign.
// 
static inline MachineOpCode 
ChooseDivInstruction(TargetMachine &target,
                     const InstructionNode* instrNode)
{
  MachineOpCode opCode = INVALID_OPCODE;
  
  const Type* resultType = instrNode->getInstruction()->getType();
  
  if (resultType->isInteger())
    opCode = resultType->isSigned()? SDIVX : UDIVX;
  else
    switch(resultType->getPrimitiveID())
      {
      case Type::FloatTyID:  opCode = FDIVS; break;
      case Type::DoubleTyID: opCode = FDIVD; break;
      default: assert(0 && "Invalid type for DIV instruction"); break; 
      }
  
  return opCode;
}


// Return NULL if we cannot exploit constant to create a cheaper instruction
static inline void
CreateDivConstInstruction(TargetMachine &target,
                          const InstructionNode* instrNode,
                          vector<MachineInstr*>& mvec)
{
  MachineInstr* minstr1 = NULL;
  MachineInstr* minstr2 = NULL;
  
  Value* constOp = ((InstrTreeNode*) instrNode->rightChild())->getValue();
  if (! isa<Constant>(constOp))
    return;
  
  // Cases worth optimizing are:
  // (1) Divide by 1 for any type: replace with copy (ADD or FMOV)
  // (2) Divide by 2^x for integer types: replace with SR[L or A]{X}
  // 
  const Type* resultType = instrNode->getInstruction()->getType();
  
  if (resultType->isInteger())
    {
      unsigned pow;
      bool isValidConst;
      int64_t C = GetConstantValueAsSignedInt(constOp, isValidConst);
      if (isValidConst)
        {
          bool needNeg = false;
          if (C < 0)
            {
              needNeg = true;
              C = -C;
            }
          
          if (C == 1)
            {
              minstr1 = new MachineInstr(ADD);
              minstr1->SetMachineOperandVal(0,
                                           MachineOperand::MO_VirtualRegister,
                                           instrNode->leftChild()->getValue());
              minstr1->SetMachineOperandReg(1,
                                        target.getRegInfo().getZeroRegNum());
            }
          else if (isPowerOf2(C, pow))
            {
              MachineOpCode opCode= ((resultType->isSigned())
                                     ? (resultType==Type::LongTy)? SRAX : SRA
                                     : (resultType==Type::LongTy)? SRLX : SRL);
              minstr1 = new MachineInstr(opCode);
              minstr1->SetMachineOperandVal(0,
                                           MachineOperand::MO_VirtualRegister,
                                           instrNode->leftChild()->getValue());
              minstr1->SetMachineOperandConst(1,
                                          MachineOperand::MO_UnextendedImmed,
                                          pow);
            }
          
          if (minstr1 && needNeg)
            { // insert <reg = SUB 0, reg> after the instr to flip the sign
              minstr2 = CreateIntNegInstruction(target,
                                                   instrNode->getValue());
            }
        }
    }
  else
    {
      if (ConstantFP *FPC = dyn_cast<ConstantFP>(constOp))
        {
          double dval = FPC->getValue();
          if (fabs(dval) == 1)
            {
              bool needNeg = (dval < 0);
              
              MachineOpCode opCode = needNeg
                ? (resultType == Type::FloatTy? FNEGS : FNEGD)
                : (resultType == Type::FloatTy? FMOVS : FMOVD);
              
              minstr1 = new MachineInstr(opCode);
              minstr1->SetMachineOperandVal(0,
                                           MachineOperand::MO_VirtualRegister,
                                           instrNode->leftChild()->getValue());
            } 
        }
    }
  
  if (minstr1 != NULL)
    minstr1->SetMachineOperandVal(2, MachineOperand::MO_VirtualRegister,
                                 instrNode->getValue());   
  
  if (minstr1)
    mvec.push_back(minstr1);
  if (minstr2)
    mvec.push_back(minstr2);
}


static void
CreateCodeForVariableSizeAlloca(const TargetMachine& target,
                                Instruction* result,
                                unsigned int tsize,
                                Value* numElementsVal,
                                vector<MachineInstr*>& getMvec)
{
  MachineInstr* M;
  
  // Create a Value to hold the (constant) element size
  Value* tsizeVal = ConstantSInt::get(Type::IntTy, tsize);

  // Get the constant offset from SP for dynamically allocated storage
  // and create a temporary Value to hold it.
  assert(result && result->getParent() && "Result value is not part of a fn?");
  Function *F = result->getParent()->getParent();
  MachineCodeForMethod& mcInfo = MachineCodeForMethod::get(F);
  bool growUp;
  ConstantSInt* dynamicAreaOffset =
    ConstantSInt::get(Type::IntTy,
                      target.getFrameInfo().getDynamicAreaOffset(mcInfo,growUp));
  assert(! growUp && "Has SPARC v9 stack frame convention changed?");

  // Create a temporary value to hold the result of MUL
  TmpInstruction* tmpProd = new TmpInstruction(numElementsVal, tsizeVal);
  MachineCodeForInstruction::get(result).addTemp(tmpProd);
  
  // Instruction 1: mul numElements, typeSize -> tmpProd
  M = new MachineInstr(MULX);
  M->SetMachineOperandVal(0, MachineOperand::MO_VirtualRegister, numElementsVal);
  M->SetMachineOperandVal(1, MachineOperand::MO_VirtualRegister, tsizeVal);
  M->SetMachineOperandVal(2, MachineOperand::MO_VirtualRegister, tmpProd);
  getMvec.push_back(M);
        
  // Instruction 2: sub %sp, tmpProd -> %sp
  M = new MachineInstr(SUB);
  M->SetMachineOperandReg(0, target.getRegInfo().getStackPointer());
  M->SetMachineOperandVal(1, MachineOperand::MO_VirtualRegister, tmpProd);
  M->SetMachineOperandReg(2, target.getRegInfo().getStackPointer());
  getMvec.push_back(M);
  
  // Instruction 3: add %sp, frameSizeBelowDynamicArea -> result
  M = new MachineInstr(ADD);
  M->SetMachineOperandReg(0, target.getRegInfo().getStackPointer());
  M->SetMachineOperandVal(1, MachineOperand::MO_VirtualRegister, dynamicAreaOffset);
  M->SetMachineOperandVal(2, MachineOperand::MO_VirtualRegister, result);
  getMvec.push_back(M);
}        


static void
CreateCodeForFixedSizeAlloca(const TargetMachine& target,
                             Instruction* result,
                             unsigned int tsize,
                             unsigned int numElements,
                             vector<MachineInstr*>& getMvec)
{
  assert(result && result->getParent() &&
         "Result value is not part of a function?");
  Function *F = result->getParent()->getParent();
  MachineCodeForMethod &mcInfo = MachineCodeForMethod::get(F);

  // Check if the offset would small enough to use as an immediate in
  // load/stores (check LDX because all load/stores have the same-size immediate
  // field).  If not, put the variable in the dynamically sized area of the
  // frame.
  unsigned int paddedSizeIgnored;
  int offsetFromFP = mcInfo.computeOffsetforLocalVar(target, result,
                                                     paddedSizeIgnored,
                                                     tsize * numElements);
  if (! target.getInstrInfo().constantFitsInImmedField(LDX, offsetFromFP))
    {
      CreateCodeForVariableSizeAlloca(target, result, tsize, 
                                      ConstantSInt::get(Type::IntTy,numElements),
                                      getMvec);
      return;
    }
  
  // else offset fits in immediate field so go ahead and allocate it.
  offsetFromFP = mcInfo.allocateLocalVar(target, result, tsize * numElements);
  
  // Create a temporary Value to hold the constant offset.
  // This is needed because it may not fit in the immediate field.
  ConstantSInt* offsetVal = ConstantSInt::get(Type::IntTy, offsetFromFP);
  
  // Instruction 1: add %fp, offsetFromFP -> result
  MachineInstr* M = new MachineInstr(ADD);
  M->SetMachineOperandReg(0, target.getRegInfo().getFramePointer());
  M->SetMachineOperandVal(1, MachineOperand::MO_VirtualRegister, offsetVal); 
  M->SetMachineOperandVal(2, MachineOperand::MO_VirtualRegister, result);
  
  getMvec.push_back(M);
}


//------------------------------------------------------------------------ 
// Function SetOperandsForMemInstr
//
// Choose addressing mode for the given load or store instruction.
// Use [reg+reg] if it is an indexed reference, and the index offset is
//		 not a constant or if it cannot fit in the offset field.
// Use [reg+offset] in all other cases.
// 
// This assumes that all array refs are "lowered" to one of these forms:
//	%x = load (subarray*) ptr, constant	; single constant offset
//	%x = load (subarray*) ptr, offsetVal	; single non-constant offset
// Generally, this should happen via strength reduction + LICM.
// Also, strength reduction should take care of using the same register for
// the loop index variable and an array index, when that is profitable.
//------------------------------------------------------------------------ 

static void
SetOperandsForMemInstr(vector<MachineInstr*>& mvec,
                       const InstructionNode* vmInstrNode,
                       const TargetMachine& target)
{
  Instruction* memInst = vmInstrNode->getInstruction();
  vector<MachineInstr*>::iterator mvecI = mvec.end() - 1;

  // Index vector, ptr value, and flag if all indices are const.
  vector<Value*> idxVec;
  bool allConstantIndices;
  Value* ptrVal = GetMemInstArgs(vmInstrNode, idxVec, allConstantIndices);

  // Now create the appropriate operands for the machine instruction.
  // First, initialize so we default to storing the offset in a register.
  int64_t smallConstOffset = 0;
  Value* valueForRegOffset = NULL;
  MachineOperand::MachineOperandType offsetOpType =
    MachineOperand::MO_VirtualRegister;

  // Check if there is an index vector and if so, compute the
  // right offset for structures and for arrays 
  // 
  if (!idxVec.empty())
    {
      const PointerType* ptrType = cast<PointerType>(ptrVal->getType());
      
      // If all indices are constant, compute the combined offset directly.
      if (allConstantIndices)
        {
          // Compute the offset value using the index vector. Create a
          // virtual reg. for it since it may not fit in the immed field.
          uint64_t offset = target.DataLayout.getIndexedOffset(ptrType,idxVec);
          valueForRegOffset = ConstantSInt::get(Type::LongTy, offset);
        }
      else
        {
          // There is at least one non-constant offset.  Therefore, this must
          // be an array ref, and must have been lowered to a single non-zero
          // offset.  (An extra leading zero offset, if any, can be ignored.)
          // Generate code sequence to compute address from index.
          // 
          bool firstIdxIsZero =
            (idxVec[0] == Constant::getNullValue(idxVec[0]->getType()));
          assert(idxVec.size() == 1U + firstIdxIsZero 
                 && "Array refs must be lowered before Instruction Selection");

          Value* idxVal = idxVec[firstIdxIsZero];

          vector<MachineInstr*> mulVec;
          Instruction* addr = new TmpInstruction(Type::UIntTy, memInst);
          MachineCodeForInstruction::get(memInst).addTemp(addr);

          // Get the array type indexed by idxVal, and compute its element size.
          // The call to getTypeSize() will fail if size is not constant.
          const Type* vecType = (firstIdxIsZero
                                 ? GetElementPtrInst::getIndexedType(ptrType,
                                           std::vector<Value*>(1U, idxVec[0]),
                                           /*AllowCompositeLeaf*/ true)
                                 : ptrType);
          const Type* eltType = cast<SequentialType>(vecType)->getElementType();
          ConstantUInt* eltSizeVal = ConstantUInt::get(Type::ULongTy,
                                       target.DataLayout.getTypeSize(eltType));

          // CreateMulInstruction() folds constants intelligently enough.
          CreateMulInstruction(target,
                               memInst->getParent()->getParent(),
                               idxVal,         /* lval, not likely to be const*/
                               eltSizeVal,     /* rval, likely to be constant */
                               addr,           /* result */
                               mulVec,
                               MachineCodeForInstruction::get(memInst),
                               INVALID_MACHINE_OPCODE);

          // Sign-extend the result of MUL  from 32 to 64 bits.
          target.getInstrInfo().CreateSignExtensionInstructions(target, memInst->getParent()->getParent(), addr, /*srcSizeInBits*/32, addr, mulVec, MachineCodeForInstruction::get(memInst));

          // Insert mulVec[] before *mvecI in mvec[] and update mvecI
          // to point to the same instruction it pointed to before.
          assert(mulVec.size() > 0 && "No multiply code created?");
          vector<MachineInstr*>::iterator oldMvecI = mvecI;
          for (unsigned i=0, N=mulVec.size(); i < N; ++i)
            mvecI = mvec.insert(mvecI, mulVec[i]) + 1;  // pts to mem instr

          valueForRegOffset = addr;
        }
    }
  else
    {
      offsetOpType = MachineOperand::MO_SignExtendedImmed;
      smallConstOffset = 0;
    }

  // For STORE:
  //   Operand 0 is value, operand 1 is ptr, operand 2 is offset
  // For LOAD or GET_ELEMENT_PTR,
  //   Operand 0 is ptr, operand 1 is offset, operand 2 is result.
  // 
  unsigned offsetOpNum, ptrOpNum;
  if (memInst->getOpcode() == Instruction::Store)
    {
      (*mvecI)->SetMachineOperandVal(0, MachineOperand::MO_VirtualRegister,
                                     vmInstrNode->leftChild()->getValue());
      ptrOpNum = 1;
      offsetOpNum = 2;
    }
  else
    {
      ptrOpNum = 0;
      offsetOpNum = 1;
      (*mvecI)->SetMachineOperandVal(2, MachineOperand::MO_VirtualRegister,
                                     memInst);
    }
  
  (*mvecI)->SetMachineOperandVal(ptrOpNum, MachineOperand::MO_VirtualRegister,
                                 ptrVal);
  
  if (offsetOpType == MachineOperand::MO_VirtualRegister)
    {
      assert(valueForRegOffset != NULL);
      (*mvecI)->SetMachineOperandVal(offsetOpNum, offsetOpType,
                                     valueForRegOffset); 
    }
  else
    (*mvecI)->SetMachineOperandConst(offsetOpNum, offsetOpType,
                                     smallConstOffset);
}


// 
// Substitute operand `operandNum' of the instruction in node `treeNode'
// in place of the use(s) of that instruction in node `parent'.
// Check both explicit and implicit operands!
// Also make sure to skip over a parent who:
// (1) is a list node in the Burg tree, or
// (2) itself had its results forwarded to its parent
// 
static void
ForwardOperand(InstructionNode* treeNode,
               InstrTreeNode*   parent,
               int operandNum)
{
  assert(treeNode && parent && "Invalid invocation of ForwardOperand");
  
  Instruction* unusedOp = treeNode->getInstruction();
  Value* fwdOp = unusedOp->getOperand(operandNum);

  // The parent itself may be a list node, so find the real parent instruction
  while (parent->getNodeType() != InstrTreeNode::NTInstructionNode)
    {
      parent = parent->parent();
      assert(parent && "ERROR: Non-instruction node has no parent in tree.");
    }
  InstructionNode* parentInstrNode = (InstructionNode*) parent;
  
  Instruction* userInstr = parentInstrNode->getInstruction();
  MachineCodeForInstruction &mvec = MachineCodeForInstruction::get(userInstr);

  // The parent's mvec would be empty if it was itself forwarded.
  // Recursively call ForwardOperand in that case...
  //
  if (mvec.size() == 0)
    {
      assert(parent->parent() != NULL &&
             "Parent could not have been forwarded, yet has no instructions?");
      ForwardOperand(treeNode, parent->parent(), operandNum);
    }
  else
    {
      for (unsigned i=0, N=mvec.size(); i < N; i++)
        {
          MachineInstr* minstr = mvec[i];
          for (unsigned i=0, numOps=minstr->getNumOperands(); i < numOps; ++i)
            {
              const MachineOperand& mop = minstr->getOperand(i);
              if (mop.getOperandType() == MachineOperand::MO_VirtualRegister &&
                  mop.getVRegValue() == unusedOp)
                minstr->SetMachineOperandVal(i,
                                MachineOperand::MO_VirtualRegister, fwdOp);
            }
          
          for (unsigned i=0,numOps=minstr->getNumImplicitRefs(); i<numOps; ++i)
            if (minstr->getImplicitRef(i) == unusedOp)
              minstr->setImplicitRef(i, fwdOp,
                                     minstr->implicitRefIsDefined(i),
                                     minstr->implicitRefIsDefinedAndUsed(i));
        }
    }
}


inline bool
AllUsesAreBranches(const Instruction* setccI)
{
  for (Value::use_const_iterator UI=setccI->use_begin(), UE=setccI->use_end();
       UI != UE; ++UI)
    if (! isa<TmpInstruction>(*UI)     // ignore tmp instructions here
        && cast<Instruction>(*UI)->getOpcode() != Instruction::Br)
      return false;
  return true;
}

//******************* Externally Visible Functions *************************/

//------------------------------------------------------------------------ 
// External Function: ThisIsAChainRule
//
// Purpose:
//   Check if a given BURG rule is a chain rule.
//------------------------------------------------------------------------ 

extern bool
ThisIsAChainRule(int eruleno)
{
  switch(eruleno)
    {
    case 111:	// stmt:  reg
    case 123:
    case 124:
    case 125:
    case 126:
    case 127:
    case 128:
    case 129:
    case 130:
    case 131:
    case 132:
    case 133:
    case 155:
    case 221:
    case 222:
    case 241:
    case 242:
    case 243:
    case 244:
    case 245:
    case 321:
      return true; break;

    default:
      return false; break;
    }
}


//------------------------------------------------------------------------ 
// External Function: GetInstructionsByRule
//
// Purpose:
//   Choose machine instructions for the SPARC according to the
//   patterns chosen by the BURG-generated parser.
//------------------------------------------------------------------------ 

void
GetInstructionsByRule(InstructionNode* subtreeRoot,
                      int ruleForNode,
                      short* nts,
                      TargetMachine &target,
                      vector<MachineInstr*>& mvec)
{
  bool checkCast = false;		// initialize here to use fall-through
  bool maskUnsignedResult = false;
  int nextRule;
  int forwardOperandNum = -1;
  unsigned int allocaSize = 0;
  MachineInstr* M, *M2;
  unsigned int L;

  mvec.clear(); 
  
  // If the code for this instruction was folded into the parent (user),
  // then do nothing!
  if (subtreeRoot->isFoldedIntoParent())
    return;
  
  // 
  // Let's check for chain rules outside the switch so that we don't have
  // to duplicate the list of chain rule production numbers here again
  // 
  if (ThisIsAChainRule(ruleForNode))
    {
      // Chain rules have a single nonterminal on the RHS.
      // Get the rule that matches the RHS non-terminal and use that instead.
      // 
      assert(nts[0] && ! nts[1]
             && "A chain rule should have only one RHS non-terminal!");
      nextRule = burm_rule(subtreeRoot->state, nts[0]);
      nts = burm_nts[nextRule];
      GetInstructionsByRule(subtreeRoot, nextRule, nts, target, mvec);
    }
  else
    {
      switch(ruleForNode) {
      case 1:	// stmt:   Ret
      case 2:	// stmt:   RetValue(reg)
      {         // NOTE: Prepass of register allocation is responsible
                //	 for moving return value to appropriate register.
                // Mark the return-address register as a hidden virtual reg.
                // Mark the return value   register as an implicit ref of
                // the machine instruction.
         	// Finally put a NOP in the delay slot.
        ReturnInst *returnInstr =
          cast<ReturnInst>(subtreeRoot->getInstruction());
        assert(returnInstr->getOpcode() == Instruction::Ret);
        
        Instruction* returnReg = new TmpInstruction(returnInstr);
        MachineCodeForInstruction::get(returnInstr).addTemp(returnReg);
        
        M = new MachineInstr(JMPLRET);
        M->SetMachineOperandReg(0, MachineOperand::MO_VirtualRegister,
                                      returnReg);
        M->SetMachineOperandConst(1,MachineOperand::MO_SignExtendedImmed,
                                   (int64_t)8);
        M->SetMachineOperandReg(2, target.getRegInfo().getZeroRegNum());
        
        if (returnInstr->getReturnValue() != NULL)
          M->addImplicitRef(returnInstr->getReturnValue());
        
        mvec.push_back(M);
        mvec.push_back(new MachineInstr(NOP));
        
        break;
      }  
        
      case 3:	// stmt:   Store(reg,reg)
      case 4:	// stmt:   Store(reg,ptrreg)
        mvec.push_back(new MachineInstr(
                         ChooseStoreInstruction(
                            subtreeRoot->leftChild()->getValue()->getType())));
        SetOperandsForMemInstr(mvec, subtreeRoot, target);
        break;

      case 5:	// stmt:   BrUncond
        M = new MachineInstr(BA);
        M->SetMachineOperandVal(0, MachineOperand::MO_PCRelativeDisp,
             cast<BranchInst>(subtreeRoot->getInstruction())->getSuccessor(0));
        mvec.push_back(M);
        
        // delay slot
        mvec.push_back(new MachineInstr(NOP));
        break;

      case 206:	// stmt:   BrCond(setCCconst)
      { // setCCconst => boolean was computed with `%b = setCC type reg1 const'
        // If the constant is ZERO, we can use the branch-on-integer-register
        // instructions and avoid the SUBcc instruction entirely.
        // Otherwise this is just the same as case 5, so just fall through.
        // 
        InstrTreeNode* constNode = subtreeRoot->leftChild()->rightChild();
        assert(constNode &&
               constNode->getNodeType() ==InstrTreeNode::NTConstNode);
        Constant *constVal = cast<Constant>(constNode->getValue());
        bool isValidConst;
        
        if ((constVal->getType()->isInteger()
             || isa<PointerType>(constVal->getType()))
            && GetConstantValueAsSignedInt(constVal, isValidConst) == 0
            && isValidConst)
          {
            // That constant is a zero after all...
            // Use the left child of setCC as the first argument!
            // Mark the setCC node so that no code is generated for it.
            InstructionNode* setCCNode = (InstructionNode*)
                                         subtreeRoot->leftChild();
            assert(setCCNode->getOpLabel() == SetCCOp);
            setCCNode->markFoldedIntoParent();
            
            BranchInst* brInst=cast<BranchInst>(subtreeRoot->getInstruction());
            
            M = new MachineInstr(ChooseBprInstruction(subtreeRoot));
            M->SetMachineOperandVal(0, MachineOperand::MO_VirtualRegister,
                                    setCCNode->leftChild()->getValue());
            M->SetMachineOperandVal(1, MachineOperand::MO_PCRelativeDisp,
                                    brInst->getSuccessor(0));
            mvec.push_back(M);
            
            // delay slot
            mvec.push_back(new MachineInstr(NOP));

            // false branch
            M = new MachineInstr(BA);
            M->SetMachineOperandVal(0, MachineOperand::MO_PCRelativeDisp,
                                    brInst->getSuccessor(1));
            mvec.push_back(M);
            
            // delay slot
            mvec.push_back(new MachineInstr(NOP));
            
            break;
          }
        // ELSE FALL THROUGH
      }

      case 6:	// stmt:   BrCond(setCC)
      { // bool => boolean was computed with SetCC.
        // The branch to use depends on whether it is FP, signed, or unsigned.
        // If it is an integer CC, we also need to find the unique
        // TmpInstruction representing that CC.
        // 
        BranchInst* brInst = cast<BranchInst>(subtreeRoot->getInstruction());
        bool isFPBranch;
        M = new MachineInstr(ChooseBccInstruction(subtreeRoot, isFPBranch));

        Value* ccValue = GetTmpForCC(subtreeRoot->leftChild()->getValue(),
                                     brInst->getParent()->getParent(),
                                     isFPBranch? Type::FloatTy : Type::IntTy);
        
        M->SetMachineOperandVal(0, MachineOperand::MO_CCRegister, ccValue);
        M->SetMachineOperandVal(1, MachineOperand::MO_PCRelativeDisp,
                                   brInst->getSuccessor(0));
        mvec.push_back(M);

        // delay slot
        mvec.push_back(new MachineInstr(NOP));

        // false branch
        M = new MachineInstr(BA);
        M->SetMachineOperandVal(0, MachineOperand::MO_PCRelativeDisp,
                                   brInst->getSuccessor(1));
        mvec.push_back(M);

        // delay slot
        mvec.push_back(new MachineInstr(NOP));
        break;
      }
        
      case 208:	// stmt:   BrCond(boolconst)
      {
        // boolconst => boolean is a constant; use BA to first or second label
        Constant* constVal = 
          cast<Constant>(subtreeRoot->leftChild()->getValue());
        unsigned dest = cast<ConstantBool>(constVal)->getValue()? 0 : 1;
        
        M = new MachineInstr(BA);
        M->SetMachineOperandVal(0, MachineOperand::MO_PCRelativeDisp,
          cast<BranchInst>(subtreeRoot->getInstruction())->getSuccessor(dest));
        mvec.push_back(M);
        
        // delay slot
        mvec.push_back(new MachineInstr(NOP));
        break;
      }
        
      case   8:	// stmt:   BrCond(boolreg)
      { // boolreg   => boolean is stored in an existing register.
        // Just use the branch-on-integer-register instruction!
        // 
        M = new MachineInstr(BRNZ);
        M->SetMachineOperandVal(0, MachineOperand::MO_VirtualRegister,
                                      subtreeRoot->leftChild()->getValue());
        M->SetMachineOperandVal(1, MachineOperand::MO_PCRelativeDisp,
              cast<BranchInst>(subtreeRoot->getInstruction())->getSuccessor(0));
        mvec.push_back(M);

        // delay slot
        mvec.push_back(new MachineInstr(NOP));

        // false branch
        M = new MachineInstr(BA);
        M->SetMachineOperandVal(0, MachineOperand::MO_PCRelativeDisp,
              cast<BranchInst>(subtreeRoot->getInstruction())->getSuccessor(1));
        mvec.push_back(M);
        
        // delay slot
        mvec.push_back(new MachineInstr(NOP));
        break;
      }  
      
      case 9:	// stmt:   Switch(reg)
        assert(0 && "*** SWITCH instruction is not implemented yet.");
        break;

      case 10:	// reg:   VRegList(reg, reg)
        assert(0 && "VRegList should never be the topmost non-chain rule");
        break;

      case 21:	// bool:  Not(bool,reg): Both these are implemented as:
      case 421:	// reg:   BNot(reg,reg):	reg = reg XOR-NOT 0
      { // First find the unary operand. It may be left or right, usually right.
        Value* notArg = BinaryOperator::getNotArgument(
                           cast<BinaryOperator>(subtreeRoot->getInstruction()));
        mvec.push_back(Create3OperandInstr_Reg(XNOR, notArg,
                                          target.getRegInfo().getZeroRegNum(),
                                          subtreeRoot->getValue()));
        break;
      }

      case 22:	// reg:   ToBoolTy(reg):
      {
        const Type* opType = subtreeRoot->leftChild()->getValue()->getType();
        assert(opType->isIntegral() || isa<PointerType>(opType));
        forwardOperandNum = 0;          // forward first operand to user
        break;
      }
      
      case 23:	// reg:   ToUByteTy(reg)
      case 25:	// reg:   ToUShortTy(reg)
      case 27:	// reg:   ToUIntTy(reg)
      case 29:	// reg:   ToULongTy(reg)
      {
        Instruction* destI =  subtreeRoot->getInstruction();
        Value* opVal = subtreeRoot->leftChild()->getValue();
        const Type* opType = subtreeRoot->leftChild()->getValue()->getType();
        if (opType->isIntegral() || isa<PointerType>(opType))
          {
            unsigned opSize = target.DataLayout.getTypeSize(opType);
            unsigned destSize = target.DataLayout.getTypeSize(destI->getType());
            if (opSize > destSize ||
                (opType->isSigned()
                 && destSize < target.DataLayout.getIntegerRegize()))
              { // operand is larger than dest,
                //    OR both are equal but smaller than the full register size
                //       AND operand is signed, so it may have extra sign bits:
                // mask high bits using AND
                M = Create3OperandInstr(AND, opVal,
                                        ConstantUInt::get(Type::ULongTy,
                                              ((uint64_t) 1 << 8*destSize) - 1),
                                        destI);
                mvec.push_back(M);
              }
            else
              forwardOperandNum = 0;          // forward first operand to user
          }
        else if (opType->isFloatingPoint())
          {
            CreateCodeToConvertFloatToInt(target, opVal, destI, mvec,
                                         MachineCodeForInstruction::get(destI));
            maskUnsignedResult = true;  // not handled by convert code
          }
        else
          assert(0 && "Unrecognized operand type for convert-to-unsigned");

        break;
      }
      
      case 24:	// reg:   ToSByteTy(reg)
      case 26:	// reg:   ToShortTy(reg)
      case 28:	// reg:   ToIntTy(reg)
      case 30:	// reg:   ToLongTy(reg)
      {
        Instruction* destI =  subtreeRoot->getInstruction();
        Value* opVal = subtreeRoot->leftChild()->getValue();
        MachineCodeForInstruction& mcfi =MachineCodeForInstruction::get(destI);

        const Type* opType = opVal->getType();
        if (opType->isIntegral() || isa<PointerType>(opType))
          {
            // These operand types have the same format as the destination,
            // but may have different size: add sign bits or mask as needed.
            // 
            const Type* destType = destI->getType();
            unsigned opSize = target.DataLayout.getTypeSize(opType);
            unsigned destSize = target.DataLayout.getTypeSize(destType);
            
            if (opSize < destSize ||
                (opSize == destSize &&
                 opSize == target.DataLayout.getIntegerRegize()))
              { // operand is smaller or both operand and result fill register
                forwardOperandNum = 0;          // forward first operand to user
              }
            else
              { // need to mask (possibly) and then sign-extend (definitely)
                Value* srcForSignExt = opVal;
                unsigned srcSizeForSignExt = 8 * opSize;
                if (opSize > destSize)
                  { // operand is larger than dest: mask high bits
                    TmpInstruction *tmpI = new TmpInstruction(destType, opVal,
                                                              destI, "maskHi");
                    mcfi.addTemp(tmpI);
                    M = Create3OperandInstr(AND, opVal,
                                            ConstantUInt::get(Type::ULongTy,
                                              ((uint64_t) 1 << 8*destSize)-1),
                                            tmpI);
                    mvec.push_back(M);
                    srcForSignExt = tmpI;
                    srcSizeForSignExt = 8 * destSize;
                  }
                
                // sign-extend
                target.getInstrInfo().CreateSignExtensionInstructions(target, destI->getParent()->getParent(), srcForSignExt, srcSizeForSignExt, destI, mvec, mcfi);
              }
          }
        else if (opType->isFloatingPoint())
          CreateCodeToConvertFloatToInt(target, opVal, destI, mvec, mcfi);
        else
          assert(0 && "Unrecognized operand type for convert-to-signed");

        break;
      }  

      case  31:	// reg:   ToFloatTy(reg):
      case  32:	// reg:   ToDoubleTy(reg):
      case 232:	// reg:   ToDoubleTy(Constant):
      
        // If this instruction has a parent (a user) in the tree 
        // and the user is translated as an FsMULd instruction,
        // then the cast is unnecessary.  So check that first.
        // In the future, we'll want to do the same for the FdMULq instruction,
        // so do the check here instead of only for ToFloatTy(reg).
        // 
        if (subtreeRoot->parent() != NULL)
          {
            const MachineCodeForInstruction& mcfi =
              MachineCodeForInstruction::get(
                cast<InstructionNode>(subtreeRoot->parent())->getInstruction());
            if (mcfi.size() == 0 || mcfi.front()->getOpCode() == FSMULD)
              forwardOperandNum = 0;    // forward first operand to user
          }

        if (forwardOperandNum != 0)     // we do need the cast
          {
            Value* leftVal = subtreeRoot->leftChild()->getValue();
            const Type* opType = leftVal->getType();
            MachineOpCode opCode=ChooseConvertToFloatInstr(
                                       subtreeRoot->getOpLabel(), opType);
            if (opCode == INVALID_OPCODE)	// no conversion needed
              {
                forwardOperandNum = 0;      // forward first operand to user
              }
            else
              {
                // If the source operand is a non-FP type it must be
                // first copied from int to float register via memory!
                Instruction *dest = subtreeRoot->getInstruction();
                Value* srcForCast;
                int n = 0;
                if (! opType->isFloatingPoint())
                  {
                    // Create a temporary to represent the FP register
                    // into which the integer will be copied via memory.
                    // The type of this temporary will determine the FP
                    // register used: single-prec for a 32-bit int or smaller,
                    // double-prec for a 64-bit int.
                    // 
                    uint64_t srcSize =
                      target.DataLayout.getTypeSize(leftVal->getType());
                    Type* tmpTypeToUse =
                      (srcSize <= 4)? Type::FloatTy : Type::DoubleTy;
                    srcForCast = new TmpInstruction(tmpTypeToUse, dest);
                    MachineCodeForInstruction &destMCFI = 
                      MachineCodeForInstruction::get(dest);
                    destMCFI.addTemp(srcForCast);

                    target.getInstrInfo().CreateCodeToCopyIntToFloat(target,
                         dest->getParent()->getParent(),
                         leftVal, cast<Instruction>(srcForCast),
                         mvec, destMCFI);
                  }
                else
                  srcForCast = leftVal;
                
                M = new MachineInstr(opCode);
                M->SetMachineOperandVal(0, MachineOperand::MO_VirtualRegister,
                                           srcForCast);
                M->SetMachineOperandVal(1, MachineOperand::MO_VirtualRegister,
                                           dest);
                mvec.push_back(M);
              }
          }
        break;

      case 19:	// reg:   ToArrayTy(reg):
      case 20:	// reg:   ToPointerTy(reg):
        forwardOperandNum = 0;          // forward first operand to user
        break;

      case 233:	// reg:   Add(reg, Constant)
        maskUnsignedResult = true;
        M = CreateAddConstInstruction(subtreeRoot);
        if (M != NULL)
          {
            mvec.push_back(M);
            break;
          }
        // ELSE FALL THROUGH
        
      case 33:	// reg:   Add(reg, reg)
        maskUnsignedResult = true;
        mvec.push_back(new MachineInstr(ChooseAddInstruction(subtreeRoot)));
        Set3OperandsFromInstr(mvec.back(), subtreeRoot, target);
        break;

      case 234:	// reg:   Sub(reg, Constant)
        maskUnsignedResult = true;
        M = CreateSubConstInstruction(subtreeRoot);
        if (M != NULL)
          {
            mvec.push_back(M);
            break;
          }
        // ELSE FALL THROUGH
        
      case 34:	// reg:   Sub(reg, reg)
        maskUnsignedResult = true;
        mvec.push_back(new MachineInstr(ChooseSubInstructionByType(
                                   subtreeRoot->getInstruction()->getType())));
        Set3OperandsFromInstr(mvec.back(), subtreeRoot, target);
        break;

      case 135:	// reg:   Mul(todouble, todouble)
        checkCast = true;
        // FALL THROUGH 

      case 35:	// reg:   Mul(reg, reg)
      {
        maskUnsignedResult = true;
        MachineOpCode forceOp = ((checkCast && BothFloatToDouble(subtreeRoot))
                                 ? FSMULD
                                 : INVALID_MACHINE_OPCODE);
        Instruction* mulInstr = subtreeRoot->getInstruction();
        CreateMulInstruction(target, mulInstr->getParent()->getParent(),
                             subtreeRoot->leftChild()->getValue(),
                             subtreeRoot->rightChild()->getValue(),
                             mulInstr, mvec,
                             MachineCodeForInstruction::get(mulInstr),forceOp);
        break;
      }
      case 335:	// reg:   Mul(todouble, todoubleConst)
        checkCast = true;
        // FALL THROUGH 

      case 235:	// reg:   Mul(reg, Constant)
      {
        maskUnsignedResult = true;
        MachineOpCode forceOp = ((checkCast && BothFloatToDouble(subtreeRoot))
                                 ? FSMULD
                                 : INVALID_MACHINE_OPCODE);
        Instruction* mulInstr = subtreeRoot->getInstruction();
        CreateMulInstruction(target, mulInstr->getParent()->getParent(),
                             subtreeRoot->leftChild()->getValue(),
                             subtreeRoot->rightChild()->getValue(),
                             mulInstr, mvec,
                             MachineCodeForInstruction::get(mulInstr),
                             forceOp);
        break;
      }
      case 236:	// reg:   Div(reg, Constant)
        maskUnsignedResult = true;
        L = mvec.size();
        CreateDivConstInstruction(target, subtreeRoot, mvec);
        if (mvec.size() > L)
          break;
        // ELSE FALL THROUGH
      
      case 36:	// reg:   Div(reg, reg)
        maskUnsignedResult = true;
        mvec.push_back(new MachineInstr(ChooseDivInstruction(target, subtreeRoot)));
        Set3OperandsFromInstr(mvec.back(), subtreeRoot, target);
        break;

      case  37:	// reg:   Rem(reg, reg)
      case 237:	// reg:   Rem(reg, Constant)
      {
        maskUnsignedResult = true;
        Instruction* remInstr = subtreeRoot->getInstruction();
        
        TmpInstruction* quot = new TmpInstruction(
                                        subtreeRoot->leftChild()->getValue(),
                                        subtreeRoot->rightChild()->getValue());
        TmpInstruction* prod = new TmpInstruction(
                                        quot,
                                        subtreeRoot->rightChild()->getValue());
        MachineCodeForInstruction::get(remInstr).addTemp(quot).addTemp(prod); 
        
        M = new MachineInstr(ChooseDivInstruction(target, subtreeRoot));
        Set3OperandsFromInstr(M, subtreeRoot, target);
        M->SetMachineOperandVal(2, MachineOperand::MO_VirtualRegister,quot);
        mvec.push_back(M);
        
        M = Create3OperandInstr(ChooseMulInstructionByType(
                                   subtreeRoot->getInstruction()->getType()),
                                quot, subtreeRoot->rightChild()->getValue(),
                                prod);
        mvec.push_back(M);
        
        M = new MachineInstr(ChooseSubInstructionByType(
                                   subtreeRoot->getInstruction()->getType()));
        Set3OperandsFromInstr(M, subtreeRoot, target);
        M->SetMachineOperandVal(1, MachineOperand::MO_VirtualRegister,prod);
        mvec.push_back(M);
        
        break;
      }
      
      case  38:	// bool:   And(bool, bool)
      case 238:	// bool:   And(bool, boolconst)
      case 338:	// reg :   BAnd(reg, reg)
      case 538:	// reg :   BAnd(reg, Constant)
        mvec.push_back(new MachineInstr(AND));
        Set3OperandsFromInstr(mvec.back(), subtreeRoot, target);
        break;

      case 138:	// bool:   And(bool, not)
      case 438:	// bool:   BAnd(bool, bnot)
      { // Use the argument of NOT as the second argument!
        // Mark the NOT node so that no code is generated for it.
        InstructionNode* notNode = (InstructionNode*) subtreeRoot->rightChild();
        Value* notArg = BinaryOperator::getNotArgument(
                           cast<BinaryOperator>(notNode->getInstruction()));
        notNode->markFoldedIntoParent();
        mvec.push_back(Create3OperandInstr(ANDN,
                                           subtreeRoot->leftChild()->getValue(),
                                           notArg, subtreeRoot->getValue()));
        break;
      }

      case  39:	// bool:   Or(bool, bool)
      case 239:	// bool:   Or(bool, boolconst)
      case 339:	// reg :   BOr(reg, reg)
      case 539:	// reg :   BOr(reg, Constant)
        mvec.push_back(new MachineInstr(OR));
        Set3OperandsFromInstr(mvec.back(), subtreeRoot, target);
        break;

      case 139:	// bool:   Or(bool, not)
      case 439:	// bool:   BOr(bool, bnot)
      { // Use the argument of NOT as the second argument!
        // Mark the NOT node so that no code is generated for it.
        InstructionNode* notNode = (InstructionNode*) subtreeRoot->rightChild();
        Value* notArg = BinaryOperator::getNotArgument(
                           cast<BinaryOperator>(notNode->getInstruction()));
        notNode->markFoldedIntoParent();
        mvec.push_back(Create3OperandInstr(ORN,
                                           subtreeRoot->leftChild()->getValue(),
                                           notArg, subtreeRoot->getValue()));
        break;
      }

      case  40:	// bool:   Xor(bool, bool)
      case 240:	// bool:   Xor(bool, boolconst)
      case 340:	// reg :   BXor(reg, reg)
      case 540:	// reg :   BXor(reg, Constant)
        mvec.push_back(new MachineInstr(XOR));
        Set3OperandsFromInstr(mvec.back(), subtreeRoot, target);
        break;

      case 140:	// bool:   Xor(bool, not)
      case 440:	// bool:   BXor(bool, bnot)
      { // Use the argument of NOT as the second argument!
        // Mark the NOT node so that no code is generated for it.
        InstructionNode* notNode = (InstructionNode*) subtreeRoot->rightChild();
        Value* notArg = BinaryOperator::getNotArgument(
                           cast<BinaryOperator>(notNode->getInstruction()));
        notNode->markFoldedIntoParent();
        mvec.push_back(Create3OperandInstr(XNOR,
                                           subtreeRoot->leftChild()->getValue(),
                                           notArg, subtreeRoot->getValue()));
        break;
      }

      case 41:	// boolconst:   SetCC(reg, Constant)
        // 
        // If the SetCC was folded into the user (parent), it will be
        // caught above.  All other cases are the same as case 42,
        // so just fall through.
        // 
      case 42:	// bool:   SetCC(reg, reg):
      {
        // This generates a SUBCC instruction, putting the difference in
        // a result register, and setting a condition code.
        // 
        // If the boolean result of the SetCC is used by anything other
        // than a branch instruction, or if it is used outside the current
        // basic block, the boolean must be
        // computed and stored in the result register.  Otherwise, discard
        // the difference (by using %g0) and keep only the condition code.
        // 
        // To compute the boolean result in a register we use a conditional
        // move, unless the result of the SUBCC instruction can be used as
        // the bool!  This assumes that zero is FALSE and any non-zero
        // integer is TRUE.
        // 
        InstructionNode* parentNode = (InstructionNode*) subtreeRoot->parent();
        Instruction* setCCInstr = subtreeRoot->getInstruction();
        
        bool keepBoolVal = parentNode == NULL ||
                           ! AllUsesAreBranches(setCCInstr);
        bool subValIsBoolVal = setCCInstr->getOpcode() == Instruction::SetNE;
        bool keepSubVal = keepBoolVal && subValIsBoolVal;
        bool computeBoolVal = keepBoolVal && ! subValIsBoolVal;
        
        bool mustClearReg;
        int valueToMove;
        MachineOpCode movOpCode = 0;
        
        // Mark the 4th operand as being a CC register, and as a def
        // A TmpInstruction is created to represent the CC "result".
        // Unlike other instances of TmpInstruction, this one is used
        // by machine code of multiple LLVM instructions, viz.,
        // the SetCC and the branch.  Make sure to get the same one!
        // Note that we do this even for FP CC registers even though they
        // are explicit operands, because the type of the operand
        // needs to be a floating point condition code, not an integer
        // condition code.  Think of this as casting the bool result to
        // a FP condition code register.
        // 
        Value* leftVal = subtreeRoot->leftChild()->getValue();
        bool isFPCompare = leftVal->getType()->isFloatingPoint();
        
        TmpInstruction* tmpForCC = GetTmpForCC(setCCInstr,
                                     setCCInstr->getParent()->getParent(),
                                     isFPCompare ? Type::FloatTy : Type::IntTy);
        MachineCodeForInstruction::get(setCCInstr).addTemp(tmpForCC);
        
        if (! isFPCompare)
          {
            // Integer condition: dest. should be %g0 or an integer register.
            // If result must be saved but condition is not SetEQ then we need
            // a separate instruction to compute the bool result, so discard
            // result of SUBcc instruction anyway.
            // 
            M = new MachineInstr(SUBcc);
            Set3OperandsFromInstr(M, subtreeRoot, target, ! keepSubVal);
            M->SetMachineOperandVal(3, MachineOperand::MO_CCRegister,
                                    tmpForCC, /*def*/true);
            mvec.push_back(M);
            
            if (computeBoolVal)
              { // recompute bool using the integer condition codes
                movOpCode =
                  ChooseMovpccAfterSub(subtreeRoot,mustClearReg,valueToMove);
              }
          }
        else
          {
            // FP condition: dest of FCMP should be some FCCn register
            M = new MachineInstr(ChooseFcmpInstruction(subtreeRoot));
            M->SetMachineOperandVal(0, MachineOperand::MO_CCRegister,
                                          tmpForCC);
            M->SetMachineOperandVal(1,MachineOperand::MO_VirtualRegister,
                                         subtreeRoot->leftChild()->getValue());
            M->SetMachineOperandVal(2,MachineOperand::MO_VirtualRegister,
                                        subtreeRoot->rightChild()->getValue());
            mvec.push_back(M);
            
            if (computeBoolVal)
              {// recompute bool using the FP condition codes
                mustClearReg = true;
                valueToMove = 1;
                movOpCode = ChooseMovFpccInstruction(subtreeRoot);
              }
          }
        
        if (computeBoolVal)
          {
            if (mustClearReg)
              {// Unconditionally set register to 0
                M = new MachineInstr(SETHI);
                M->SetMachineOperandConst(0,MachineOperand::MO_UnextendedImmed,
                                          (int64_t)0);
                M->SetMachineOperandVal(1, MachineOperand::MO_VirtualRegister,
                                        setCCInstr);
                mvec.push_back(M);
              }
            
            // Now conditionally move `valueToMove' (0 or 1) into the register
            // Mark the register as a use (as well as a def) because the old
            // value should be retained if the condition is false.
            M = new MachineInstr(movOpCode);
            M->SetMachineOperandVal(0, MachineOperand::MO_CCRegister,
                                    tmpForCC);
            M->SetMachineOperandConst(1, MachineOperand::MO_UnextendedImmed,
                                      valueToMove);
            M->SetMachineOperandVal(2, MachineOperand::MO_VirtualRegister,
                                    setCCInstr, /*isDef*/ true,
                                    /*isDefAndUse*/ true);
            mvec.push_back(M);
          }
        break;
      }    

      case 51:	// reg:   Load(reg)
      case 52:	// reg:   Load(ptrreg)
        mvec.push_back(new MachineInstr(ChooseLoadInstruction(
                                     subtreeRoot->getValue()->getType())));
        SetOperandsForMemInstr(mvec, subtreeRoot, target);
        break;

      case 55:	// reg:   GetElemPtr(reg)
      case 56:	// reg:   GetElemPtrIdx(reg,reg)
        // If the GetElemPtr was folded into the user (parent), it will be
        // caught above.  For other cases, we have to compute the address.
        mvec.push_back(new MachineInstr(ADD));
        SetOperandsForMemInstr(mvec, subtreeRoot, target);
        break;
        
      case 57:	// reg:  Alloca: Implement as 1 instruction:
      {         //	    add %fp, offsetFromFP -> result
        AllocationInst* instr =
          cast<AllocationInst>(subtreeRoot->getInstruction());
        unsigned int tsize =
          target.findOptimalStorageSize(instr->getAllocatedType());
        assert(tsize != 0);
        CreateCodeForFixedSizeAlloca(target, instr, tsize, 1, mvec);
        break;
      }
      
      case 58:	// reg:   Alloca(reg): Implement as 3 instructions:
                //	mul num, typeSz -> tmp
                //	sub %sp, tmp    -> %sp
      {         //	add %sp, frameSizeBelowDynamicArea -> result
        AllocationInst* instr =
          cast<AllocationInst>(subtreeRoot->getInstruction());
        const Type* eltType = instr->getAllocatedType();
        
        // If #elements is constant, use simpler code for fixed-size allocas
        int tsize = (int) target.findOptimalStorageSize(eltType);
        Value* numElementsVal = NULL;
        bool isArray = instr->isArrayAllocation();
        
        if (!isArray ||
            isa<Constant>(numElementsVal = instr->getArraySize()))
          { // total size is constant: generate code for fixed-size alloca
            unsigned int numElements = isArray? 
              cast<ConstantUInt>(numElementsVal)->getValue() : 1;
            CreateCodeForFixedSizeAlloca(target, instr, tsize,
                                         numElements, mvec);
          }
        else // total size is not constant.
          CreateCodeForVariableSizeAlloca(target, instr, tsize,
                                          numElementsVal, mvec);
        break;
      }
      
      case 61:	// reg:   Call
      {         // Generate a direct (CALL) or indirect (JMPL). depending
                // Mark the return-address register and the indirection
                // register (if any) as hidden virtual registers.
                // Also, mark the operands of the Call and return value (if
                // any) as implicit operands of the CALL machine instruction.
                // 
                // If this is a varargs function, floating point arguments
                // have to passed in integer registers so insert
                // copy-float-to-int instructions for each float operand.
                // 
        CallInst *callInstr = cast<CallInst>(subtreeRoot->getInstruction());
        Value *callee = callInstr->getCalledValue();
        
        // Create hidden virtual register for return address, with type void*. 
        TmpInstruction* retAddrReg =
          new TmpInstruction(PointerType::get(Type::VoidTy), callInstr);
        MachineCodeForInstruction::get(callInstr).addTemp(retAddrReg);
        
        // Generate the machine instruction and its operands.
        // Use CALL for direct function calls; this optimistically assumes
        // the PC-relative address fits in the CALL address field (22 bits).
        // Use JMPL for indirect calls.
        // 
        if (isa<Function>(callee))
          { // direct function call
            M = new MachineInstr(CALL);
            M->SetMachineOperandVal(0, MachineOperand::MO_PCRelativeDisp,
                                    callee);
          } 
        else
          { // indirect function call
            M = new MachineInstr(JMPLCALL);
            M->SetMachineOperandVal(0, MachineOperand::MO_VirtualRegister,
                                    callee);
            M->SetMachineOperandConst(1, MachineOperand::MO_SignExtendedImmed,
                                      (int64_t) 0);
            M->SetMachineOperandVal(2, MachineOperand::MO_VirtualRegister,
                                    retAddrReg);
          }
        
        mvec.push_back(M);

        const FunctionType* funcType =
          cast<FunctionType>(cast<PointerType>(callee->getType())
                             ->getElementType());
        bool isVarArgs = funcType->isVarArg();
        bool noPrototype = isVarArgs && funcType->getNumParams() == 0;
        
        // Use an annotation to pass information about call arguments
        // to the register allocator.
        CallArgsDescriptor* argDesc = new CallArgsDescriptor(callInstr,
                                         retAddrReg, isVarArgs, noPrototype);
        M->addAnnotation(argDesc);
        
        assert(callInstr->getOperand(0) == callee
               && "This is assumed in the loop below!");
        
        for (unsigned i=1, N=callInstr->getNumOperands(); i < N; ++i)
          {
            Value* argVal = callInstr->getOperand(i);
            Instruction* intArgReg = NULL;
            
            // Check for FP arguments to varargs functions.
            // Any such argument in the first $K$ args must be passed in an
            // integer register, where K = #integer argument registers.
            if (isVarArgs && argVal->getType()->isFloatingPoint())
              {
                // If it is a function with no prototype, pass value
                // as an FP value as well as a varargs value
                if (noPrototype)
                  argDesc->getArgInfo(i-1).setUseFPArgReg();
                
                // If this arg. is in the first $K$ regs, add a copy
                // float-to-int instruction to pass the value as an integer.
                if (i < target.getRegInfo().GetNumOfIntArgRegs())
                  {
                    MachineCodeForInstruction &destMCFI = 
                      MachineCodeForInstruction::get(callInstr);   
                    intArgReg = new TmpInstruction(Type::IntTy, argVal);
                    destMCFI.addTemp(intArgReg);
                    
                    vector<MachineInstr*> copyMvec;
                    target.getInstrInfo().CreateCodeToCopyFloatToInt(target,
                                           callInstr->getParent()->getParent(),
                                           argVal, (TmpInstruction*) intArgReg,
                                           copyMvec, destMCFI);
                    mvec.insert(mvec.begin(),copyMvec.begin(),copyMvec.end());
                    
                    argDesc->getArgInfo(i-1).setUseIntArgReg();
                    argDesc->getArgInfo(i-1).setArgCopy(intArgReg);
                  }
                else
                  // Cannot fit in first $K$ regs so pass the arg on the stack
                  argDesc->getArgInfo(i-1).setUseStackSlot();
              }
            
            if (intArgReg)
              mvec.back()->addImplicitRef(intArgReg);
            
            mvec.back()->addImplicitRef(argVal);
          }
        
        // Add the return value as an implicit ref.  The call operands
        // were added above.
        if (callInstr->getType() != Type::VoidTy)
          mvec.back()->addImplicitRef(callInstr, /*isDef*/ true);
        
        // For the CALL instruction, the ret. addr. reg. is also implicit
        if (isa<Function>(callee))
          mvec.back()->addImplicitRef(retAddrReg, /*isDef*/ true);
        
        // delay slot
        mvec.push_back(new MachineInstr(NOP));
        break;
      }
      
      case 62:	// reg:   Shl(reg, reg)
      {
        Value* argVal1 = subtreeRoot->leftChild()->getValue();
        Value* argVal2 = subtreeRoot->rightChild()->getValue();
        Instruction* shlInstr = subtreeRoot->getInstruction();
        
        const Type* opType = argVal1->getType();
        assert((opType->isInteger() || isa<PointerType>(opType)) &&
               "Shl unsupported for other types");
        
        CreateShiftInstructions(target, shlInstr->getParent()->getParent(),
                                (opType == Type::LongTy)? SLLX : SLL,
                                argVal1, argVal2, 0, shlInstr, mvec,
                                MachineCodeForInstruction::get(shlInstr));
        break;
      }
      
      case 63:	// reg:   Shr(reg, reg)
      { const Type* opType = subtreeRoot->leftChild()->getValue()->getType();
        assert((opType->isInteger() || isa<PointerType>(opType)) &&
               "Shr unsupported for other types");
        mvec.push_back(new MachineInstr((opType->isSigned()
                                   ? ((opType == Type::LongTy)? SRAX : SRA)
                                   : ((opType == Type::LongTy)? SRLX : SRL))));
        Set3OperandsFromInstr(mvec.back(), subtreeRoot, target);
        break;
      }
      
      case 64:	// reg:   Phi(reg,reg)
        break;                          // don't forward the value

      case 71:	// reg:     VReg
      case 72:	// reg:     Constant
        break;                          // don't forward the value

      default:
        assert(0 && "Unrecognized BURG rule");
        break;
      }
    }

  if (forwardOperandNum >= 0)
    { // We did not generate a machine instruction but need to use operand.
      // If user is in the same tree, replace Value in its machine operand.
      // If not, insert a copy instruction which should get coalesced away
      // by register allocation.
      if (subtreeRoot->parent() != NULL)
        ForwardOperand(subtreeRoot, subtreeRoot->parent(), forwardOperandNum);
      else
        {
          vector<MachineInstr*> minstrVec;
          Instruction* instr = subtreeRoot->getInstruction();
          target.getInstrInfo().
            CreateCopyInstructionsByType(target,
                                         instr->getParent()->getParent(),
                                         instr->getOperand(forwardOperandNum),
                                         instr, minstrVec,
                                        MachineCodeForInstruction::get(instr));
          assert(minstrVec.size() > 0);
          mvec.insert(mvec.end(), minstrVec.begin(), minstrVec.end());
        }
    }

  if (maskUnsignedResult)
    { // If result is unsigned and smaller than int reg size,
      // we need to clear high bits of result value.
      assert(forwardOperandNum < 0 && "Need mask but no instruction generated");
      Instruction* dest = subtreeRoot->getInstruction();
      if (dest->getType()->isUnsigned())
        {
          unsigned destSize = target.DataLayout.getTypeSize(dest->getType());
          if (destSize <= 4)
            { // Mask high bits.  Use a TmpInstruction to represent the
              // intermediate result before masking.  Since those instructions
              // have already been generated, go back and substitute tmpI
              // for dest in the result position of each one of them.
              TmpInstruction *tmpI = new TmpInstruction(dest->getType(), dest,
                                                        NULL, "maskHi");
              MachineCodeForInstruction::get(dest).addTemp(tmpI);

              for (unsigned i=0, N=mvec.size(); i < N; ++i)
                mvec[i]->substituteValue(dest, tmpI);

              M = Create3OperandInstr_UImmed(SRL, tmpI, 4-destSize, dest);
              mvec.push_back(M);
            }
          else if (destSize < target.DataLayout.getIntegerRegize())
            assert(0 && "Unsupported type size: 32 < size < 64 bits");
        }
    }
}
