//===-- MachineInstr.cpp --------------------------------------------------===//
// 
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/Value.h"
#include "llvm/Target/MachineInstrInfo.h"  // FIXME: shouldn't need this!
using std::cerr;

// Global variable holding an array of descriptors for machine instructions.
// The actual object needs to be created separately for each target machine.
// This variable is initialized and reset by class MachineInstrInfo.
// 
// FIXME: This should be a property of the target so that more than one target
// at a time can be active...
//
extern const MachineInstrDescriptor *TargetInstrDescriptors;

// Constructor for instructions with fixed #operands (nearly all)
MachineInstr::MachineInstr(MachineOpCode _opCode)
  : opCode(_opCode),
    operands(TargetInstrDescriptors[_opCode].numOperands, MachineOperand()),
    numImplicitRefs(0)
{
  assert(TargetInstrDescriptors[_opCode].numOperands >= 0);
}

// Constructor for instructions with variable #operands
MachineInstr::MachineInstr(MachineOpCode OpCode, unsigned  numOperands)
  : opCode(OpCode),
    operands(numOperands, MachineOperand()),
    numImplicitRefs(0)
{
}

MachineInstr::MachineInstr(MachineOpCode Opcode, unsigned numOperands,
                           bool XX, bool YY)
  : opCode(Opcode),
    numImplicitRefs(0)
{
  operands.reserve(numOperands);
}

// OperandComplete - Return true if it's illegal to add a new operand
bool MachineInstr::OperandsComplete() const
{
  int NumOperands = TargetInstrDescriptors[opCode].numOperands;
  if (NumOperands >= 0 && getNumOperands() >= (unsigned)NumOperands)
    return true;  // Broken!
  return false;
}


// 
// Support for replacing opcode and operands of a MachineInstr in place.
// This only resets the size of the operand vector and initializes it.
// The new operands must be set explicitly later.
// 
void MachineInstr::replace(MachineOpCode Opcode, unsigned numOperands)
{
  assert(getNumImplicitRefs() == 0 &&
         "This is probably broken because implicit refs are going to be lost.");
  opCode = Opcode;
  operands.clear();
  operands.resize(numOperands, MachineOperand());
}

void
MachineInstr::SetMachineOperandVal(unsigned i,
                                   MachineOperand::MachineOperandType opType,
                                   Value* V,
                                   bool isdef,
                                   bool isDefAndUse)
{
  assert(i < operands.size());          // may be explicit or implicit op
  operands[i].opType = opType;
  operands[i].value = V;
  operands[i].regNum = -1;
  operands[i].flags = 0;

  if (isdef || TargetInstrDescriptors[opCode].resultPos == (int) i)
    operands[i].markDef();
  if (isDefAndUse)
    operands[i].markDefAndUse();
}

void
MachineInstr::SetMachineOperandConst(unsigned i,
				MachineOperand::MachineOperandType operandType,
                                     int64_t intValue)
{
  assert(i < getNumOperands());          // must be explicit op
  assert(TargetInstrDescriptors[opCode].resultPos != (int) i &&
         "immed. constant cannot be defined");

  operands[i].opType = operandType;
  operands[i].value = NULL;
  operands[i].immedVal = intValue;
  operands[i].regNum = -1;
  operands[i].flags = 0;
}

void
MachineInstr::SetMachineOperandReg(unsigned i,
                                   int regNum,
                                   bool isdef) {
  assert(i < getNumOperands());          // must be explicit op

  operands[i].opType = MachineOperand::MO_MachineRegister;
  operands[i].value = NULL;
  operands[i].regNum = regNum;
  operands[i].flags = 0;

  if (isdef || TargetInstrDescriptors[opCode].resultPos == (int) i)
    operands[i].markDef();
  insertUsedReg(regNum);
}

void
MachineInstr::SetRegForOperand(unsigned i, int regNum)
{
  assert(i < getNumOperands());          // must be explicit op
  operands[i].setRegForValue(regNum);
  insertUsedReg(regNum);
}


// Subsitute all occurrences of Value* oldVal with newVal in all operands
// and all implicit refs.  If defsOnly == true, substitute defs only.
unsigned
MachineInstr::substituteValue(const Value* oldVal, Value* newVal, bool defsOnly)
{
  unsigned numSubst = 0;

  // Subsitute operands
  for (MachineInstr::val_op_iterator O = begin(), E = end(); O != E; ++O)
    if (*O == oldVal)
      if (!defsOnly || O.isDef())
        {
          O.getMachineOperand().value = newVal;
          ++numSubst;
        }

  // Subsitute implicit refs
  for (unsigned i=0, N=getNumImplicitRefs(); i < N; ++i)
    if (getImplicitRef(i) == oldVal)
      if (!defsOnly || implicitRefIsDefined(i))
        {
          getImplicitOp(i).value = newVal;
          ++numSubst;
        }

  return numSubst;
}


void
MachineInstr::dump() const 
{
  cerr << "  " << *this;
}

static inline std::ostream&
OutputValue(std::ostream &os, const Value* val)
{
  os << "(val ";
  if (val && val->hasName())
    return os << val->getName() << ")";
  else
    return os << (void*) val << ")";              // print address only
}

static inline std::ostream&
OutputReg(std::ostream &os, unsigned int regNum)
{
  return os << "%mreg(" << regNum << ")";
}

std::ostream &operator<<(std::ostream& os, const MachineInstr& minstr)
{
  os << TargetInstrDescriptors[minstr.opCode].Name;
  
  for (unsigned i=0, N=minstr.getNumOperands(); i < N; i++) {
    os << "\t" << minstr.getOperand(i);
    if( minstr.operandIsDefined(i) ) 
      os << "*";
    if( minstr.operandIsDefinedAndUsed(i) ) 
      os << "*";
  }
  
  // code for printing implict references
  unsigned NumOfImpRefs =  minstr.getNumImplicitRefs();
  if(  NumOfImpRefs > 0 ) {
    os << "\tImplicit: ";
    for(unsigned z=0; z < NumOfImpRefs; z++) {
      OutputValue(os, minstr.getImplicitRef(z)); 
      if( minstr.implicitRefIsDefined(z)) os << "*";
      if( minstr.implicitRefIsDefinedAndUsed(z)) os << "*";
      os << "\t";
    }
  }
  
  return os << "\n";
}

std::ostream &operator<<(std::ostream &os, const MachineOperand &mop)
{
  if (mop.opHiBits32())
    os << "%lm(";
  else if (mop.opLoBits32())
    os << "%lo(";
  else if (mop.opHiBits64())
    os << "%hh(";
  else if (mop.opLoBits64())
    os << "%hm(";
  
  switch(mop.opType)
    {
    case MachineOperand::MO_VirtualRegister:
      os << "%reg";
      OutputValue(os, mop.getVRegValue());
      if (mop.hasAllocatedReg())
        os << "==" << OutputReg(os, mop.getAllocatedRegNum());
      break;
    case MachineOperand::MO_CCRegister:
      os << "%ccreg";
      OutputValue(os, mop.getVRegValue());
      if (mop.hasAllocatedReg())
        os << "==" << OutputReg(os, mop.getAllocatedRegNum());
      break;
    case MachineOperand::MO_MachineRegister:
      OutputReg(os, mop.getMachineRegNum());
      break;
    case MachineOperand::MO_SignExtendedImmed:
      os << (long)mop.immedVal;
      break;
    case MachineOperand::MO_UnextendedImmed:
      os << (long)mop.immedVal;
      break;
    case MachineOperand::MO_PCRelativeDisp:
      {
        const Value* opVal = mop.getVRegValue();
        bool isLabel = isa<Function>(opVal) || isa<BasicBlock>(opVal);
        os << "%disp(" << (isLabel? "label " : "addr-of-val ");
        if (opVal->hasName())
          os << opVal->getName();
        else
          os << (const void*) opVal;
        os << ")";
        break;
      }
    default:
      assert(0 && "Unrecognized operand type");
      break;
    }
  
  if (mop.flags &
      (MachineOperand::HIFLAG32 | MachineOperand::LOFLAG32 | 
       MachineOperand::HIFLAG64 | MachineOperand::LOFLAG64))
    os << ")";
  
  return os;
}
