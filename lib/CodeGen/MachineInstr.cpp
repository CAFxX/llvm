// $Id$
//***************************************************************************
// File:
//	MachineInstr.cpp
// 
// Purpose:
//	
// 
// Strategy:
// 
// History:
//	7/2/01	 -  Vikram Adve  -  Created
//**************************************************************************/

#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/ConstPoolVals.h"
#include "llvm/Instruction.h"
#include <strstream>

//************************ Class Implementations **************************/


bool
MachineInstrInfo::constantFitsInImmedField(int64_t intValue) const
{
  // First, check if opCode has an immed field.
  bool isSignExtended;
  uint64_t maxImmedValue = this->maxImmedConstant(isSignExtended);
  if (maxImmedValue != 0)
    {
      // Now check if the constant fits
      if (intValue <= (int64_t) maxImmedValue &&
	  intValue >= -((int64_t) maxImmedValue+1))
	return true;
    }
  
  return false;
}

MachineInstr::MachineInstr(MachineOpCode _opCode,
			   OpCodeMask    _opCodeMask)
  : opCode(_opCode),
    opCodeMask(_opCodeMask),
    operands(TargetMachineInstrInfo[_opCode].numOperands)
{
}

MachineInstr::~MachineInstr()
{
}

void
MachineInstr::SetMachineOperand(unsigned int i,
				MachineOperand::MachineOperandType operandType,
				Value* _val)
{
  assert(i < TargetMachineInstrInfo[opCode].numOperands);
  operands[i].Initialize(operandType, _val);
}

void
MachineInstr::SetMachineOperand(unsigned int i,
				MachineOperand::MachineOperandType operandType,
				int64_t intValue)
{
  assert(i < TargetMachineInstrInfo[opCode].numOperands);
  operands[i].InitializeConst(operandType, intValue);
}

void
MachineInstr::SetMachineOperand(unsigned int i,
				unsigned int regNum)
{
  assert(i < TargetMachineInstrInfo[opCode].numOperands);
  operands[i].InitializeReg(regNum);
}

void
MachineInstr::dump(unsigned int indent)
{
  for (unsigned i=0; i < indent; i++)
    cout << "    ";
  
  cout << *this;
}

ostream&
operator<< (ostream& os, const MachineInstr& minstr)
{
  os << TargetMachineInstrInfo[minstr.opCode].opCodeString;
  
  for (unsigned i=0, N=minstr.getNumOperands(); i < N; i++)
    os << "\t" << minstr.getOperand(i);
  
  return os;
}

ostream&
operator<< (ostream& os, const MachineOperand& mop)
{
  strstream regInfo;
  if (mop.machineOperandType == MachineOperand::MO_Register)
    {
      if (mop.vregType == MachineOperand::MO_VirtualReg)
	regInfo << "(val " << mop.value << ")" << ends;
      else
	regInfo << "("       << mop.regNum << ")" << ends;
    }
  else if (mop.machineOperandType == MachineOperand::MO_CCRegister)
    regInfo << "(val " << mop.value << ")" << ends;
  
  switch(mop.machineOperandType)
    {
    case MachineOperand::MO_Register:
      os << "%reg" << regInfo.str();
      free(regInfo.str());
      break;
      
    case MachineOperand::MO_CCRegister:
      os << "%ccreg" << regInfo.str();
      free(regInfo.str());
      break;

    case MachineOperand::MO_SignExtendedImmed:
      os << mop.immedVal;
      break;

    case MachineOperand::MO_UnextendedImmed:
      os << mop.immedVal;
      break;

    case MachineOperand::MO_PCRelativeDisp:
      os << "%disp(label " << mop.value << ")";
      break;

    default:
      assert(0 && "Unrecognized operand type");
      break;
    }

  return os;
}


//---------------------------------------------------------------------------
// Target-independent utility routines for creating machine instructions
//---------------------------------------------------------------------------


//------------------------------------------------------------------------ 
// Function Set2OperandsFromInstr
// Function Set3OperandsFromInstr
// 
// For the common case of 2- and 3-operand arithmetic/logical instructions,
// set the m/c instr. operands directly from the VM instruction's operands.
// Check whether the first or second operand is 0 and can use a dedicated "0" register.
// Check whether the second operand should use an immediate field or register.
// (First and third operands are never immediates for such instructions.)
// 
// Arguments:
// canDiscardResult: Specifies that the result operand can be discarded
//		     by using the dedicated "0"
// 
// op1position, op2position and resultPosition: Specify in which position
//		     in the machine instruction the 3 operands (arg1, arg2
//		     and result) should go.
// 
// RETURN VALUE: unsigned int flags, where
//	flags & 0x01	=> operand 1 is constant and needs a register
//	flags & 0x02	=> operand 2 is constant and needs a register
//------------------------------------------------------------------------ 

void
Set2OperandsFromInstr(MachineInstr* minstr,
		      InstructionNode* vmInstrNode,
		      const TargetMachine& targetMachine,
		      bool canDiscardResult,
		      int op1Position,
		      int resultPosition)
{
  Set3OperandsFromInstr(minstr, vmInstrNode, targetMachine,
			canDiscardResult, op1Position,
			/*op2Position*/ -1, resultPosition);
}


unsigned
Set3OperandsFromInstrJUNK(MachineInstr* minstr,
		      InstructionNode* vmInstrNode,
		      const TargetMachine& targetMachine,
		      bool canDiscardResult,
		      int op1Position,
		      int op2Position,
		      int resultPosition)
{
  assert(op1Position >= 0);
  assert(resultPosition >= 0);
  
  unsigned returnFlags = 0x0;
  
  // Check if operand 1 is 0 and if so, try to use the register that gives 0, if any.
  Value* op1Value = vmInstrNode->leftChild()->getValue();
  bool isValidConstant;
  int64_t intValue = GetConstantValueAsSignedInt(op1Value, isValidConstant);
  if (isValidConstant && intValue == 0 && targetMachine.zeroRegNum >= 0)
    minstr->SetMachineOperand(op1Position, /*regNum*/ targetMachine.zeroRegNum);
  else
    {
      if (op1Value->getValueType() == Value::ConstantVal)
	{// value is constant and must be loaded from constant pool
	  returnFlags = returnFlags | (1 << op1Position);
	}
      minstr->SetMachineOperand(op1Position, MachineOperand::MO_Register,
					     op1Value);
    }
  
  // Check if operand 2 (if any) fits in the immediate field of the instruction,
  // of if it is 0 and can use a dedicated machine register
  if (op2Position >= 0)
    {
      Value* op2Value = vmInstrNode->rightChild()->getValue();
      int64_t immedValue;
      MachineOperand::VirtualRegisterType vregType;
      unsigned int machineRegNum;
      
      MachineOperand::MachineOperandType
	op2type = ChooseRegOrImmed(op2Value, minstr->getOpCode(),targetMachine,
				   /*canUseImmed*/ true,
				   vregType, machineRegNum, immedValue);
      
      if (op2type == MachineOperand::MO_Register)
	{
	  if (vregType == MachineOperand::MO_MachineReg)
	    minstr->SetMachineOperand(op2Position, machineRegNum);
	  else
	    {
	      if (op2Value->getValueType() == Value::ConstantVal)
		{// value is constant and must be loaded from constant pool
		  returnFlags = returnFlags | (1 << op2Position);
		}
	      minstr->SetMachineOperand(op2Position, op2type, op2Value);
	    }
	}
      else
	minstr->SetMachineOperand(op2Position, op2type, immedValue);
    }
  
  // If operand 3 (result) can be discarded, use a dead register if one exists
  if (canDiscardResult && targetMachine.zeroRegNum >= 0)
    minstr->SetMachineOperand(resultPosition, targetMachine.zeroRegNum);
  else
    minstr->SetMachineOperand(resultPosition, MachineOperand::MO_Register,
					      vmInstrNode->getValue());

  return returnFlags;
}


void
Set3OperandsFromInstr(MachineInstr* minstr,
		      InstructionNode* vmInstrNode,
		      const TargetMachine& targetMachine,
		      bool canDiscardResult,
		      int op1Position,
		      int op2Position,
		      int resultPosition)
{
  assert(op1Position >= 0);
  assert(resultPosition >= 0);
  
  // operand 1
  minstr->SetMachineOperand(op1Position, MachineOperand::MO_Register,
			    vmInstrNode->leftChild()->getValue());   
  
  // operand 2 (if any)
  if (op2Position >= 0)
    minstr->SetMachineOperand(op2Position, MachineOperand::MO_Register,
			      vmInstrNode->rightChild()->getValue());   
  
  // result operand: if it can be discarded, use a dead register if one exists
  if (canDiscardResult && targetMachine.zeroRegNum >= 0)
    minstr->SetMachineOperand(resultPosition, targetMachine.zeroRegNum);
  else
    minstr->SetMachineOperand(resultPosition, MachineOperand::MO_Register,
					      vmInstrNode->getValue());
}


MachineOperand::MachineOperandType
ChooseRegOrImmed(Value* val,
		 MachineOpCode opCode,
		 const TargetMachine& targetMachine,
		 bool canUseImmed,
		 MachineOperand::VirtualRegisterType& getVRegType,
		 unsigned int& getMachineRegNum,
		 int64_t& getImmedValue)
{
  MachineOperand::MachineOperandType opType = MachineOperand::MO_Register;
  getVRegType = MachineOperand::MO_VirtualReg;
  getMachineRegNum = 0;
  getImmedValue = 0;
  
  // Check for the common case first: argument is not constant
  // 
  if (val->getValueType() != Value::ConstantVal)
    return opType;
  
  // Now get the constant value and check if it fits in the IMMED field.
  // Take advantage of the fact that the max unsigned value will rarely
  // fit into any IMMED field and ignore that case (i.e., cast smaller
  // unsigned constants to signed).
  // 
  bool isValidConstant;
  int64_t intValue = GetConstantValueAsSignedInt(val, isValidConstant);
  
  if (isValidConstant)
    {
      if (intValue == 0 && targetMachine.zeroRegNum >= 0)
	{
	  getVRegType = MachineOperand::MO_MachineReg;
	  getMachineRegNum = targetMachine.zeroRegNum;
	}
      else if (canUseImmed &&
	       targetMachine.machineInstrInfo[opCode].constantFitsInImmedField(intValue))
	{
	  opType = MachineOperand::MO_SignExtendedImmed;
	  getImmedValue = intValue;
	}
    }
  
  return opType;
}
