// $Id$ -*-c++-*-
//***************************************************************************
// File:
//	InstrSelection.h
// 
// Purpose:
//	External interface to instruction selection.
// 
// History:
//	7/02/01	 -  Vikram Adve  -  Created
//**************************************************************************/

#ifndef LLVM_CODEGEN_INSTR_SELECTION_H
#define LLVM_CODEGEN_INSTR_SELECTION_H

#include "llvm/Instruction.h"
class Function;
class InstrForest;
class MachineInstr;
class InstructionNode;
class TargetMachine;

/************************* Required Functions *******************************
 * Target-dependent functions that MUST be implemented for each target.
 ***************************************************************************/

const unsigned MAX_INSTR_PER_VMINSTR = 8;

extern void	GetInstructionsByRule	(InstructionNode* subtreeRoot,
					 int ruleForNode,
					 short* nts,
					 TargetMachine &Target,
                                         vector<MachineInstr*>& mvec);

extern unsigned	GetInstructionsForProlog(BasicBlock* entryBB,
					 TargetMachine &Target,
					 MachineInstr** minstrVec);

extern unsigned	GetInstructionsForEpilog(BasicBlock* anExitBB,
					 TargetMachine &Target,
					 MachineInstr** minstrVec);

extern bool	ThisIsAChainRule	(int eruleno);


//************************ Exported Functions ******************************/


//---------------------------------------------------------------------------
// Function: SelectInstructionsForMethod
// 
// Purpose:
//   Entry point for instruction selection using BURG.
//   Returns true if instruction selection failed, false otherwise.
//   Implemented in machine-specific instruction selection file.
//---------------------------------------------------------------------------

bool		SelectInstructionsForMethod	(Function* function,
						 TargetMachine &Target);


//************************ Exported Data Types *****************************/


//---------------------------------------------------------------------------
// class TmpInstruction
//
//   This class represents temporary intermediate values
//   used within the machine code for a VM instruction
//---------------------------------------------------------------------------

class TmpInstruction : public Instruction {
  TmpInstruction(const TmpInstruction &TI)
    : Instruction(TI.getType(), TI.getOpcode()) {
    if (!TI.Operands.empty()) {
      Operands.push_back(Use(TI.Operands[0], this));
      if (TI.Operands.size() == 2)
        Operands.push_back(Use(TI.Operands[1], this));
      else
        assert(0 && "Bad # operands to TmpInstruction!");
    }
  }
public:
  // Constructor that uses the type of S1 as the type of the temporary.
  // s1 must be a valid value.  s2 may be NULL.
  TmpInstruction(Value *s1, Value *s2 = 0, const std::string &name = "")
    : Instruction(s1->getType(), Instruction::UserOp1, name) {
    Operands.push_back(Use(s1, this));  // s1 must be nonnull
    if (s2) {
      Operands.push_back(Use(s2, this));
    }
  }
  
  // Constructor that requires the type of the temporary to be specified.
  // Both S1 and S2 may be NULL.
  TmpInstruction(const Type *Ty, Value *s1 = 0, Value* s2 = 0,
                 const std::string &name = "")
    : Instruction(Ty, Instruction::UserOp1, name) {
    if (s1) { Operands.push_back(Use(s1, this)); }
    if (s2) { Operands.push_back(Use(s2, this)); }
  }
  
  virtual Instruction *clone() const { return new TmpInstruction(*this); }
  virtual const char *getOpcodeName() const {
    return "TempValueForMachineInstr";
  }
  
  // Methods for support type inquiry through isa, cast, and dyn_cast:
  static inline bool classof(const TmpInstruction *) { return true; }
  static inline bool classof(const Instruction *I) {
    return (I->getOpcode() == Instruction::UserOp1);
  }
  static inline bool classof(const Value *V) {
    return isa<Instruction>(V) && classof(cast<Instruction>(V));
  }
};

#endif
