//===-- llvm/Target/RegInfo.h - Target Register Information ------*- C++ -*-==//
//
// This file is used to describe the register system of a target to the
// register allocator.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TARGET_MACHINEREGINFO_H
#define LLVM_TARGET_MACHINEREGINFO_H

#include "Support/NonCopyable.h"
#include <ext/hash_map>
#include <string>

class TargetMachine;
class IGNode;
class Type;
class Value;
class LiveRangeInfo;
class Function;
class Instruction;
class LiveRange;
class AddedInstrns;
class MachineInstr;
class RegClass;
class CallInst;
class ReturnInst;
class PhyRegAlloc;
class BasicBlock;

//-----------------------------------------------------------------------------
// class MachineRegClassInfo
// 
// Purpose:
//   Interface to description of machine register class (e.g., int reg class
//   float reg class etc)
// 
//--------------------------------------------------------------------------


class MachineRegClassInfo {
protected:
  const unsigned RegClassID;        // integer ID of a reg class
  const unsigned NumOfAvailRegs;    // # of avail for coloring -without SP etc.
  const unsigned NumOfAllRegs;      // # of all registers -including SP,g0 etc.
  
public:
  inline unsigned getRegClassID()     const { return RegClassID; }
  inline unsigned getNumOfAvailRegs() const { return NumOfAvailRegs; }
  inline unsigned getNumOfAllRegs()   const { return NumOfAllRegs; }

  // This method should find a color which is not used by neighbors
  // (i.e., a false position in IsColorUsedArr) and 
  virtual void colorIGNode(IGNode *Node,
                           std::vector<bool> &IsColorUsedArr) const = 0;
  virtual bool isRegVolatile(int Reg) const = 0;

  MachineRegClassInfo(unsigned ID, unsigned NVR, unsigned NAR)
    : RegClassID(ID), NumOfAvailRegs(NVR), NumOfAllRegs(NAR) {}
};



//---------------------------------------------------------------------------
// class MachineRegInfo
// 
// Purpose:
//   Interface to register info of target machine
// 
//--------------------------------------------------------------------------

class MachineRegInfo : public NonCopyableV {
protected:
  // A vector of all machine register classes
  //
  std::vector<const MachineRegClassInfo *> MachineRegClassArr;    
  
public:
  const TargetMachine &target;

  MachineRegInfo(const TargetMachine& tgt) : target(tgt) { }
  ~MachineRegInfo() {
    for (unsigned i = 0, e = MachineRegClassArr.size(); i != e; ++i)
      delete MachineRegClassArr[i];
  }

  // According the definition of a MachineOperand class, a Value in a
  // machine instruction can go into either a normal register or a 
  // condition code register. If isCCReg is true below, the ID of the condition
  // code regiter class will be returned. Otherwise, the normal register
  // class (eg. int, float) must be returned.
  virtual unsigned getRegClassIDOfType  (const Type *type,
					 bool isCCReg = false) const =0;
  virtual unsigned getRegClassIDOfValue (const Value *Val,
					 bool isCCReg = false) const =0;
  

  inline unsigned int getNumOfRegClasses() const { 
    return MachineRegClassArr.size(); 
  }  

  const MachineRegClassInfo *getMachineRegClass(unsigned i) const { 
    return MachineRegClassArr[i]; 
  }

  // returns the register that is hardwired to zero if any (-1 if none)
  //
  virtual int getZeroRegNum() const = 0;

  // Number of registers used for passing int args (usually 6: %o0 - %o5)
  // and float args (usually 32: %f0 - %f31)
  //
  virtual unsigned const GetNumOfIntArgRegs() const   = 0;
  virtual unsigned const GetNumOfFloatArgRegs() const = 0;

  // The following methods are used to color special live ranges (e.g.
  // method args and return values etc.) with specific hardware registers
  // as required. See SparcRegInfo.cpp for the implementation for Sparc.
  //
  virtual void suggestRegs4MethodArgs(const Function *Func, 
			 LiveRangeInfo &LRI) const = 0;

  virtual void suggestRegs4CallArgs(const MachineInstr *CallI, 
			LiveRangeInfo &LRI, std::vector<RegClass *> RCL) const = 0;

  virtual void suggestReg4RetValue(const MachineInstr *RetI, 
				   LiveRangeInfo &LRI) const = 0;

  virtual void colorMethodArgs(const Function *Func,  LiveRangeInfo &LRI,
                               AddedInstrns *FirstAI) const = 0;

  virtual void colorCallArgs(const MachineInstr *CalI, 
			     LiveRangeInfo& LRI, AddedInstrns *CallAI, 
			     PhyRegAlloc &PRA, const BasicBlock *BB) const = 0;

  virtual void colorRetValue(const MachineInstr *RetI, LiveRangeInfo &LRI,
			     AddedInstrns *RetAI) const = 0;



  // The following methods are used to generate "copy" machine instructions
  // for an architecture. Currently they are used in MachineRegClass 
  // interface. However, they can be moved to MachineInstrInfo interface if
  // necessary.
  //
  virtual void cpReg2RegMI(unsigned SrcReg, unsigned DestReg,
                           int RegType, std::vector<MachineInstr*>& mvec) const = 0;

  virtual void cpReg2MemMI(unsigned SrcReg, unsigned DestPtrReg, int Offset,
                           int RegTypee, std::vector<MachineInstr*>& mvec) const=0;

  virtual void cpMem2RegMI(unsigned SrcPtrReg, int Offset, unsigned DestReg,
                           int RegTypee, std::vector<MachineInstr*>& mvec) const=0;

  virtual void cpValue2Value(Value *Src, Value *Dest,
                             std::vector<MachineInstr*>& mvec) const = 0;

  virtual bool isRegVolatile(int RegClassID, int Reg) const = 0;
  
  // Returns the reg used for pushing the address when a method is called.
  // This can be used for other purposes between calls
  //
  virtual unsigned getCallAddressReg() const = 0;

  // Returns the register containing the return address.
  //It should be made sure that this 
  // register contains the return value when a return instruction is reached.
  //
  virtual unsigned getReturnAddressReg() const = 0; 
  

  // Each register class has a seperate space for register IDs. To convert
  // a regId in a register class to a common Id, we use the folloing method(s)
  //
  virtual int getUnifiedRegNum(int RegClassID, int reg) const = 0;

  virtual const std::string getUnifiedRegName(int UnifiedRegNum) const = 0;


  // The following 4 methods are used to find the RegType (see enum above)
  // of a LiveRange, Value and using the unified RegClassID
  //
  virtual int getRegType(unsigned regClassID, const Type* type) const = 0;
  virtual int getRegType(const LiveRange *LR) const = 0;
  virtual int getRegType(const Value *Val) const = 0;
  virtual int getRegType(int reg) const = 0;

  
  // The following methods are used to get the frame/stack pointers
  // 
  virtual unsigned getFramePointer() const = 0;
  virtual unsigned getStackPointer() const = 0;

  // A register can be initialized to an invalid number. That number can
  // be obtained using this method.
  //
  virtual int getInvalidRegNum() const = 0;


  // Method for inserting caller saving code. The caller must save all the
  // volatile registers across a call based on the calling conventions of
  // an architecture. This must insert code for saving and restoring 
  // such registers on
  //
  virtual void insertCallerSavingCode(const MachineInstr *MInst, 
				      const BasicBlock *BB, 
				      PhyRegAlloc &PRA) const = 0;

  // This method gives the the number of bytes of stack spaceallocated 
  // to a register when it is spilled to the stack.
  //
  virtual int getSpilledRegSize(int RegType) const = 0;
};

#endif
