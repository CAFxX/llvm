//===-- llvm/CodeGen/MachineInstr.h - MachineInstr class ---------*- C++ -*--=//
//
// This file contains the declaration of the MachineInstr class, which is the
// basic representation for all target dependant machine instructions used by
// the back end.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_MACHINEINSTR_H
#define LLVM_CODEGEN_MACHINEINSTR_H

#include "llvm/Annotation.h"
#include "Support/iterator"
#include "Support/NonCopyable.h"
#include <vector>
class Value;
class Function;
class MachineBasicBlock;
class TargetMachine;

typedef int MachineOpCode;

//---------------------------------------------------------------------------
// class MachineOperand 
// 
// Purpose:
//   Representation of each machine instruction operand.
//   This class is designed so that you can allocate a vector of operands
//   first and initialize each one later.
//
//   E.g, for this VM instruction:
//		ptr = alloca type, numElements
//   we generate 2 machine instructions on the SPARC:
// 
//		mul Constant, Numelements -> Reg
//		add %sp, Reg -> Ptr
// 
//   Each instruction has 3 operands, listed above.  Of those:
//   -	Reg, NumElements, and Ptr are of operand type MO_Register.
//   -	Constant is of operand type MO_SignExtendedImmed on the SPARC.
//	
//   For the register operands, the virtual register type is as follows:
//	
//   -  Reg will be of virtual register type MO_MInstrVirtualReg.  The field
//	MachineInstr* minstr will point to the instruction that computes reg.
// 
//   -	%sp will be of virtual register type MO_MachineReg.
//	The field regNum identifies the machine register.
// 
//   -	NumElements will be of virtual register type MO_VirtualReg.
//	The field Value* value identifies the value.
// 
//   -	Ptr will also be of virtual register type MO_VirtualReg.
//	Again, the field Value* value identifies the value.
// 
//---------------------------------------------------------------------------

class MachineOperand {
public:
  enum MachineOperandType {
    MO_VirtualRegister,		// virtual register for *value
    MO_MachineRegister,		// pre-assigned machine register `regNum'
    MO_CCRegister,
    MO_SignExtendedImmed,
    MO_UnextendedImmed,
    MO_PCRelativeDisp,
  };
  
private:
  // Bit fields of the flags variable used for different operand properties
  static const char DEFFLAG    = 0x1;  // this is a def of the operand
  static const char DEFUSEFLAG = 0x2;  // this is both a def and a use
  static const char HIFLAG32   = 0x4;  // operand is %hi32(value_or_immedVal)
  static const char LOFLAG32   = 0x8;  // operand is %lo32(value_or_immedVal)
  static const char HIFLAG64   = 0x10; // operand is %hi64(value_or_immedVal)
  static const char LOFLAG64   = 0x20; // operand is %lo64(value_or_immedVal)
  
private:
  union {
    Value*	value;		// BasicBlockVal for a label operand.
				// ConstantVal for a non-address immediate.
				// Virtual register for an SSA operand,
				// including hidden operands required for
				// the generated machine code.     
    int64_t immedVal;		// constant value for an explicit constant
  };

  MachineOperandType opType:8;  // Pack into 8 bits efficiently after flags.
  char flags;                   // see bit field definitions above
  int regNum;	                // register number for an explicit register
                                // will be set for a value after reg allocation
private:
  MachineOperand()
    : immedVal(0),
      opType(MO_VirtualRegister),
      flags(0),
      regNum(-1) {}

  MachineOperand(int64_t ImmVal, MachineOperandType OpTy)
    : immedVal(ImmVal),
      opType(OpTy),
      flags(0),
      regNum(-1) {}

  MachineOperand(int Reg, MachineOperandType OpTy, bool isDef = false)
    : immedVal(0),
      opType(OpTy),
      flags(isDef ? DEFFLAG : 0),
      regNum(Reg) {}

  MachineOperand(Value *V, MachineOperandType OpTy,
                 bool isDef = false, bool isDNU = false)
    : value(V),
      opType(OpTy),
      regNum(-1) {
    flags = (isDef ? DEFFLAG : 0) | (isDNU ? DEFUSEFLAG : 0);
  }

public:
  MachineOperand(const MachineOperand &M)
    : immedVal(M.immedVal),
      opType(M.opType),
      flags(M.flags),
      regNum(M.regNum) {}

  ~MachineOperand() {}
  
  // Accessor methods.  Caller is responsible for checking the
  // operand type before invoking the corresponding accessor.
  // 
  MachineOperandType getType() const { return opType; }

  inline Value*		getVRegValue	() const {
    assert(opType == MO_VirtualRegister || opType == MO_CCRegister || 
	   opType == MO_PCRelativeDisp);
    return value;
  }
  inline Value*		getVRegValueOrNull() const {
    return (opType == MO_VirtualRegister || opType == MO_CCRegister || 
            opType == MO_PCRelativeDisp)? value : NULL;
  }
  inline int            getMachineRegNum() const {
    assert(opType == MO_MachineRegister);
    return regNum;
  }
  inline int64_t	getImmedValue	() const {
    assert(opType == MO_SignExtendedImmed || opType == MO_UnextendedImmed);
    return immedVal;
  }
  bool		opIsDef		() const { return flags & DEFFLAG; }
  bool		opIsDefAndUse	() const { return flags & DEFUSEFLAG; }
  bool          opHiBits32      () const { return flags & HIFLAG32; }
  bool          opLoBits32      () const { return flags & LOFLAG32; }
  bool          opHiBits64      () const { return flags & HIFLAG64; }
  bool          opLoBits64      () const { return flags & LOFLAG64; }

  // used to check if a machine register has been allocated to this operand
  inline bool   hasAllocatedReg() const {
    return (regNum >= 0 &&
            (opType == MO_VirtualRegister || opType == MO_CCRegister || 
             opType == MO_MachineRegister));
  }

  // used to get the reg number if when one is allocated
  inline int  getAllocatedRegNum() const {
    assert(opType == MO_VirtualRegister || opType == MO_CCRegister || 
	   opType == MO_MachineRegister);
    return regNum;
  }

  
  friend std::ostream& operator<<(std::ostream& os, const MachineOperand& mop);

private:

  // Construction methods needed for fine-grain control.
  // These must be accessed via coresponding methods in MachineInstr.
  void markDef()       { flags |= DEFFLAG; }
  void markDefAndUse() { flags |= DEFUSEFLAG; }
  void markHi32()      { flags |= HIFLAG32; }
  void markLo32()      { flags |= LOFLAG32; }
  void markHi64()      { flags |= HIFLAG64; }
  void markLo64()      { flags |= LOFLAG64; }
  
  // Replaces the Value with its corresponding physical register after
  // register allocation is complete
  void setRegForValue(int reg) {
    assert(opType == MO_VirtualRegister || opType == MO_CCRegister || 
	   opType == MO_MachineRegister);
    regNum = reg;
  }
  
  friend class MachineInstr;
};


//---------------------------------------------------------------------------
// class MachineInstr 
// 
// Purpose:
//   Representation of each machine instruction.
// 
//   MachineOpCode must be an enum, defined separately for each target.
//   E.g., It is defined in SparcInstructionSelection.h for the SPARC.
// 
//  There are 2 kinds of operands:
// 
//  (1) Explicit operands of the machine instruction in vector operands[] 
// 
//  (2) "Implicit operands" are values implicitly used or defined by the
//      machine instruction, such as arguments to a CALL, return value of
//      a CALL (if any), and return value of a RETURN.
//---------------------------------------------------------------------------

class MachineInstr: public NonCopyable {      // Disable copy operations

  MachineOpCode    opCode;              // the opcode
  std::vector<MachineOperand> operands; // the operands
  unsigned numImplicitRefs;             // number of implicit operands

  MachineOperand& getImplicitOp(unsigned i) {
    assert(i < numImplicitRefs && "implicit ref# out of range!");
    return operands[i + operands.size() - numImplicitRefs];
  }
  const MachineOperand& getImplicitOp(unsigned i) const {
    assert(i < numImplicitRefs && "implicit ref# out of range!");
    return operands[i + operands.size() - numImplicitRefs];
  }

  // regsUsed - all machine registers used for this instruction, including regs
  // used to save values across the instruction.  This is a bitset of registers.
  std::vector<bool> regsUsed;

  // OperandComplete - Return true if it's illegal to add a new operand
  bool OperandsComplete() const;

public:
  MachineInstr(MachineOpCode Opcode);
  MachineInstr(MachineOpCode Opcode, unsigned numOperands);

  /// MachineInstr ctor - This constructor only does a _reserve_ of the
  /// operands, not a resize for them.  It is expected that if you use this that
  /// you call add* methods below to fill up the operands, instead of the Set
  /// methods.  Eventually, the "resizing" ctors will be phased out.
  ///
  MachineInstr(MachineOpCode Opcode, unsigned numOperands, bool XX, bool YY);

  /// MachineInstr ctor - Work exactly the same as the ctor above, except that
  /// the MachineInstr is created and added to the end of the specified basic
  /// block.
  ///
  MachineInstr(MachineBasicBlock *MBB, MachineOpCode Opcode, unsigned numOps);
  

  /// replace - Support to rewrite a machine instruction in place: for now,
  /// simply replace() and then set new operands with Set.*Operand methods
  /// below.
  /// 
  void replace(MachineOpCode Opcode, unsigned numOperands);
  
  // The opcode.
  // 
  const MachineOpCode getOpcode() const { return opCode; }
  const MachineOpCode getOpCode() const { return opCode; }

  //
  // Information about explicit operands of the instruction
  // 
  unsigned getNumOperands() const { return operands.size() - numImplicitRefs; }
  
  const MachineOperand& getOperand(unsigned i) const {
    assert(i < getNumOperands() && "getOperand() out of range!");
    return operands[i];
  }
  MachineOperand& getOperand(unsigned i) {
    assert(i < getNumOperands() && "getOperand() out of range!");
    return operands[i];
  }

  MachineOperand::MachineOperandType getOperandType(unsigned i) const {
    return getOperand(i).getType();
  }

  bool operandIsDefined(unsigned i) const {
    return getOperand(i).opIsDef();
  }

  bool operandIsDefinedAndUsed(unsigned i) const {
    return getOperand(i).opIsDefAndUse();
  }

  //
  // Information about implicit operands of the instruction
  // 
  unsigned getNumImplicitRefs() const{ return numImplicitRefs; }
  
  const Value* getImplicitRef(unsigned i) const {
    return getImplicitOp(i).getVRegValue();
  }
  Value* getImplicitRef(unsigned i) {
    return getImplicitOp(i).getVRegValue();
  }

  bool implicitRefIsDefined(unsigned i) const {
    return getImplicitOp(i).opIsDef();
  }
  bool implicitRefIsDefinedAndUsed(unsigned i) const {
    return getImplicitOp(i).opIsDefAndUse();
  }
  inline void addImplicitRef    (Value* V,
                                 bool isDef=false,bool isDefAndUse=false);
  inline void setImplicitRef    (unsigned i, Value* V,
                                 bool isDef=false, bool isDefAndUse=false);

  //
  // Information about registers used in this instruction
  // 
  const std::vector<bool> &getRegsUsed() const { return regsUsed; }
  
  // insertUsedReg - Add a register to the Used registers set...
  void insertUsedReg(unsigned Reg) {
    if (Reg >= regsUsed.size())
      regsUsed.resize(Reg+1);
    regsUsed[Reg] = true;
  }

  //
  // Debugging support
  //
  void print(std::ostream &OS, const TargetMachine &TM);
  void dump() const;
  friend std::ostream& operator<<(std::ostream& os, const MachineInstr& minstr);

  //
  // Define iterators to access the Value operands of the Machine Instruction.
  // Note that these iterators only enumerate the explicit operands.
  // begin() and end() are defined to produce these iterators...
  //
  template<class _MI, class _V> class ValOpIterator;
  typedef ValOpIterator<const MachineInstr*,const Value*> const_val_op_iterator;
  typedef ValOpIterator<      MachineInstr*,      Value*> val_op_iterator;

  // Access to set the operands when building the machine instruction
  // 
  void SetMachineOperandVal     (unsigned i,
                                 MachineOperand::MachineOperandType operandType,
                                 Value* V,
                                 bool isDef=false,
                                 bool isDefAndUse=false);

  void SetMachineOperandConst   (unsigned i,
                                 MachineOperand::MachineOperandType operandType,
                                 int64_t intValue);

  void SetMachineOperandReg     (unsigned i,
                                 int regNum,
                                 bool isDef=false);

  //===--------------------------------------------------------------------===//
  // Accessors to add operands when building up machine instructions
  //

  /// addRegOperand - Add a MO_VirtualRegister operand to the end of the
  /// operands list...
  ///
  void addRegOperand(Value *V, bool isDef=false, bool isDefAndUse=false) {
    assert(!OperandsComplete() &&
           "Trying to add an operand to a machine instr that is already done!");
    operands.push_back(MachineOperand(V, MachineOperand::MO_VirtualRegister,
                                      isDef, isDefAndUse));
  }

  /// addRegOperand - Add a symbolic virtual register reference...
  ///
  void addRegOperand(int reg, bool isDef = false) {
    assert(!OperandsComplete() &&
           "Trying to add an operand to a machine instr that is already done!");
    operands.push_back(MachineOperand(reg, MachineOperand::MO_VirtualRegister,
                                      isDef));
  }

  /// addPCDispOperand - Add a PC relative displacement operand to the MI
  ///
  void addPCDispOperand(Value *V) {
    assert(!OperandsComplete() &&
           "Trying to add an operand to a machine instr that is already done!");
    operands.push_back(MachineOperand(V, MachineOperand::MO_PCRelativeDisp));
  }

  /// addMachineRegOperand - Add a virtual register operand to this MachineInstr
  ///
  void addMachineRegOperand(int reg, bool isDef=false) {
    assert(!OperandsComplete() &&
           "Trying to add an operand to a machine instr that is already done!");
    operands.push_back(MachineOperand(reg, MachineOperand::MO_MachineRegister,
                                      isDef));
    insertUsedReg(reg);
  }

  /// addZeroExtImmOperand - Add a zero extended constant argument to the
  /// machine instruction.
  ///
  void addZeroExtImmOperand(int64_t intValue) {
    assert(!OperandsComplete() &&
           "Trying to add an operand to a machine instr that is already done!");
    operands.push_back(MachineOperand(intValue,
                                      MachineOperand::MO_UnextendedImmed));
  }

  /// addSignExtImmOperand - Add a zero extended constant argument to the
  /// machine instruction.
  ///
  void addSignExtImmOperand(int64_t intValue) {
    assert(!OperandsComplete() &&
           "Trying to add an operand to a machine instr that is already done!");
    operands.push_back(MachineOperand(intValue,
                                      MachineOperand::MO_SignExtendedImmed));
  }


  unsigned substituteValue(const Value* oldVal, Value* newVal,
                           bool defsOnly = true);

  void setOperandHi32(unsigned i) { operands[i].markHi32(); }
  void setOperandLo32(unsigned i) { operands[i].markLo32(); }
  void setOperandHi64(unsigned i) { operands[i].markHi64(); }
  void setOperandLo64(unsigned i) { operands[i].markLo64(); }
  
  
  // SetRegForOperand - Replaces the Value for the operand with its allocated
  // physical register after register allocation is complete.
  // 
  void SetRegForOperand(unsigned i, int regNum);

  //
  // Iterator to enumerate machine operands.
  // 
  template<class MITy, class VTy>
  class ValOpIterator : public forward_iterator<VTy, ptrdiff_t> {
    unsigned i;
    MITy MI;
    
    void skipToNextVal() {
      while (i < MI->getNumOperands() &&
             !( (MI->getOperandType(i) == MachineOperand::MO_VirtualRegister ||
                 MI->getOperandType(i) == MachineOperand::MO_CCRegister)
                && MI->getOperand(i).getVRegValue() != 0))
        ++i;
    }
  
    inline ValOpIterator(MITy mi, unsigned I) : i(I), MI(mi) {
      skipToNextVal();
    }
  
  public:
    typedef ValOpIterator<MITy, VTy> _Self;
    
    inline VTy operator*() const {
      return MI->getOperand(i).getVRegValue();
    }

    const MachineOperand &getMachineOperand() const { return MI->getOperand(i);}
          MachineOperand &getMachineOperand()       { return MI->getOperand(i);}

    inline VTy operator->() const { return operator*(); }

    inline bool isDef()       const { return MI->getOperand(i).opIsDef(); } 
    inline bool isDefAndUse() const { return MI->getOperand(i).opIsDefAndUse();}

    inline _Self& operator++() { i++; skipToNextVal(); return *this; }
    inline _Self  operator++(int) { _Self tmp = *this; ++*this; return tmp; }

    inline bool operator==(const _Self &y) const { 
      return i == y.i;
    }
    inline bool operator!=(const _Self &y) const { 
      return !operator==(y);
    }

    static _Self begin(MITy MI) {
      return _Self(MI, 0);
    }
    static _Self end(MITy MI) {
      return _Self(MI, MI->getNumOperands());
    }
  };

  // define begin() and end()
  val_op_iterator begin() { return val_op_iterator::begin(this); }
  val_op_iterator end()   { return val_op_iterator::end(this); }

  const_val_op_iterator begin() const {
    return const_val_op_iterator::begin(this);
  }
  const_val_op_iterator end() const {
    return const_val_op_iterator::end(this);
  }
};


// Define here to enable inlining of the functions used.
// 
void MachineInstr::addImplicitRef(Value* V,
                                  bool isDef,
                                  bool isDefAndUse)
{
  ++numImplicitRefs;
  addRegOperand(V, isDef, isDefAndUse);
}

void MachineInstr::setImplicitRef(unsigned i,
                                  Value* V,
                                  bool isDef,
                                  bool isDefAndUse)
{
  assert(i < getNumImplicitRefs() && "setImplicitRef() out of range!");
  SetMachineOperandVal(i + getNumImplicitRefs(),
                       MachineOperand::MO_VirtualRegister,
                       V, isDef, isDefAndUse);
}


//---------------------------------------------------------------------------
// Debugging Support
//---------------------------------------------------------------------------

std::ostream& operator<<        (std::ostream& os,
                                 const MachineInstr& minstr);

std::ostream& operator<<        (std::ostream& os,
                                 const MachineOperand& mop);
					 
void PrintMachineInstructions   (const Function *F);

#endif
