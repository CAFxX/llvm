//===-- SparcInternals.h ----------------------------------------*- C++ -*-===//
// 
// This file defines stuff that is to be private to the Sparc backend, but is
// shared among different portions of the backend.
//
//===----------------------------------------------------------------------===//

#ifndef SPARC_INTERNALS_H
#define SPARC_INTERNALS_H

#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/MachineSchedInfo.h"
#include "llvm/Target/MachineFrameInfo.h"
#include "llvm/Target/MachineCacheInfo.h"
#include "llvm/Target/MachineRegInfo.h"
#include "llvm/Target/MachineOptInfo.h"
#include "llvm/Type.h"
#include <sys/types.h>

class LiveRange;
class UltraSparc;
class PhyRegAlloc;
class Pass;

enum SparcInstrSchedClass {
  SPARC_NONE,		/* Instructions with no scheduling restrictions */
  SPARC_IEUN,		/* Integer class that can use IEU0 or IEU1 */
  SPARC_IEU0,		/* Integer class IEU0 */
  SPARC_IEU1,		/* Integer class IEU1 */
  SPARC_FPM,		/* FP Multiply or Divide instructions */
  SPARC_FPA,		/* All other FP instructions */	
  SPARC_CTI,		/* Control-transfer instructions */
  SPARC_LD,		/* Load instructions */
  SPARC_ST,		/* Store instructions */
  SPARC_SINGLE,		/* Instructions that must issue by themselves */
  
  SPARC_INV,		/* This should stay at the end for the next value */
  SPARC_NUM_SCHED_CLASSES = SPARC_INV
};


//---------------------------------------------------------------------------
// enum SparcMachineOpCode. 
// const MachineInstrDescriptor SparcMachineInstrDesc[]
// 
// Purpose:
//   Description of UltraSparc machine instructions.
// 
//---------------------------------------------------------------------------

enum SparcMachineOpCode {
#define I(ENUM, OPCODESTRING, NUMOPERANDS, RESULTPOS, MAXIMM, IMMSE, \
          NUMDELAYSLOTS, LATENCY, SCHEDCLASS, INSTFLAGS)             \
   ENUM,
#include "SparcInstr.def"

  // End-of-array marker
  INVALID_OPCODE,
  NUM_REAL_OPCODES = PHI,		// number of valid opcodes
  NUM_TOTAL_OPCODES = INVALID_OPCODE
};


// Array of machine instruction descriptions...
extern const MachineInstrDescriptor SparcMachineInstrDesc[];


//---------------------------------------------------------------------------
// class UltraSparcInstrInfo 
// 
// Purpose:
//   Information about individual instructions.
//   Most information is stored in the SparcMachineInstrDesc array above.
//   Other information is computed on demand, and most such functions
//   default to member functions in base class MachineInstrInfo. 
//---------------------------------------------------------------------------

struct UltraSparcInstrInfo : public MachineInstrInfo {
  UltraSparcInstrInfo();

  //
  // All immediate constants are in position 1 except the
  // store instructions and SETxx.
  // 
  virtual int getImmedConstantPos(MachineOpCode opCode) const {
    bool ignore;
    if (this->maxImmedConstant(opCode, ignore) != 0)
      {
        assert(! this->isStore((MachineOpCode) STB - 1)); // 1st  store opcode
        assert(! this->isStore((MachineOpCode) STXFSR+1));// last store opcode
        if (opCode==SETSW || opCode==SETUW || opCode==SETX || opCode==SETHI)
          return 0;
        if (opCode >= STB && opCode <= STXFSR)
          return 2;
        return 1;
      }
    else
      return -1;
  }
  
  virtual bool		hasResultInterlock	(MachineOpCode opCode) const
  {
    // All UltraSPARC instructions have interlocks (note that delay slots
    // are not considered here).
    // However, instructions that use the result of an FCMP produce a
    // 9-cycle stall if they are issued less than 3 cycles after the FCMP.
    // Force the compiler to insert a software interlock (i.e., gap of
    // 2 other groups, including NOPs if necessary).
    return (opCode == FCMPS || opCode == FCMPD || opCode == FCMPQ);
  }

  //-------------------------------------------------------------------------
  // Queries about representation of LLVM quantities (e.g., constants)
  //-------------------------------------------------------------------------

  virtual bool ConstantMayNotFitInImmedField(const Constant* CV,
                                             const Instruction* I) const;

  //-------------------------------------------------------------------------
  // Code generation support for creating individual machine instructions
  //-------------------------------------------------------------------------

  // Get certain common op codes for the current target.  This and all the
  // Create* methods below should be moved to a machine code generation class
  // 
  virtual MachineOpCode getNOPOpCode() const { return NOP; }

  // Create an instruction sequence to put the constant `val' into
  // the virtual register `dest'.  `val' may be a Constant or a
  // GlobalValue, viz., the constant address of a global variable or function.
  // The generated instructions are returned in `mvec'.
  // Any temp. registers (TmpInstruction) created are recorded in mcfi.
  // Any stack space required is allocated via mcff.
  // 
  virtual void  CreateCodeToLoadConst(const TargetMachine& target,
                                      Function* F,
                                      Value* val,
                                      Instruction* dest,
                                      std::vector<MachineInstr*>& mvec,
                                      MachineCodeForInstruction& mcfi) const;

  // Create an instruction sequence to copy an integer value `val'
  // to a floating point value `dest' by copying to memory and back.
  // val must be an integral type.  dest must be a Float or Double.
  // The generated instructions are returned in `mvec'.
  // Any temp. registers (TmpInstruction) created are recorded in mcfi.
  // Any stack space required is allocated via mcff.
  // 
  virtual void  CreateCodeToCopyIntToFloat(const TargetMachine& target,
                                       Function* F,
                                       Value* val,
                                       Instruction* dest,
                                       std::vector<MachineInstr*>& mvec,
                                       MachineCodeForInstruction& mcfi) const;

  // Similarly, create an instruction sequence to copy an FP value
  // `val' to an integer value `dest' by copying to memory and back.
  // The generated instructions are returned in `mvec'.
  // Any temp. registers (TmpInstruction) created are recorded in mcfi.
  // Any stack space required is allocated via mcff.
  // 
  virtual void  CreateCodeToCopyFloatToInt(const TargetMachine& target,
                                       Function* F,
                                       Value* val,
                                       Instruction* dest,
                                       std::vector<MachineInstr*>& mvec,
                                       MachineCodeForInstruction& mcfi) const;
  
  // Create instruction(s) to copy src to dest, for arbitrary types
  // The generated instructions are returned in `mvec'.
  // Any temp. registers (TmpInstruction) created are recorded in mcfi.
  // Any stack space required is allocated via mcff.
  // 
  virtual void CreateCopyInstructionsByType(const TargetMachine& target,
                                       Function* F,
                                       Value* src,
                                       Instruction* dest,
                                       std::vector<MachineInstr*>& mvec,
                                       MachineCodeForInstruction& mcfi) const;

  // Create instruction sequence to produce a sign-extended register value
  // from an arbitrary sized value (sized in bits, not bytes).
  // The generated instructions are appended to `mvec'.
  // Any temp. registers (TmpInstruction) created are recorded in mcfi.
  // Any stack space required is allocated via mcff.
  // 
  virtual void CreateSignExtensionInstructions(const TargetMachine& target,
                                       Function* F,
                                       Value* srcVal,
                                       Value* destVal,
                                       unsigned int numLowBits,
                                       std::vector<MachineInstr*>& mvec,
                                       MachineCodeForInstruction& mcfi) const;

  // Create instruction sequence to produce a zero-extended register value
  // from an arbitrary sized value (sized in bits, not bytes).
  // The generated instructions are appended to `mvec'.
  // Any temp. registers (TmpInstruction) created are recorded in mcfi.
  // Any stack space required is allocated via mcff.
  // 
  virtual void CreateZeroExtensionInstructions(const TargetMachine& target,
                                       Function* F,
                                       Value* srcVal,
                                       Value* destVal,
                                       unsigned int numLowBits,
                                       std::vector<MachineInstr*>& mvec,
                                       MachineCodeForInstruction& mcfi) const;
};


//----------------------------------------------------------------------------
// class UltraSparcRegInfo
//
// This class implements the virtual class MachineRegInfo for Sparc.
//
//----------------------------------------------------------------------------

class UltraSparcRegInfo : public MachineRegInfo {
  // The actual register classes in the Sparc
  //
  enum RegClassIDs { 
    IntRegClassID,                      // Integer
    FloatRegClassID,                    // Float (both single/double)
    IntCCRegClassID,                    // Int Condition Code
    FloatCCRegClassID                   // Float Condition code
  };


  // Type of registers available in Sparc. There can be several reg types
  // in the same class. For instace, the float reg class has Single/Double
  // types
  //
  enum RegTypes {
    IntRegType,
    FPSingleRegType,
    FPDoubleRegType,
    IntCCRegType,
    FloatCCRegType
  };

  // **** WARNING: If the above enum order is changed, also modify 
  // getRegisterClassOfValue method below since it assumes this particular 
  // order for efficiency.


  // Number of registers used for passing int args (usually 6: %o0 - %o5)
  //
  unsigned const NumOfIntArgRegs;

  // Number of registers used for passing float args (usually 32: %f0 - %f31)
  //
  unsigned const NumOfFloatArgRegs;

  // An out of bound register number that can be used to initialize register
  // numbers. Useful for error detection.
  //
  int const InvalidRegNum;


  // ========================  Private Methods =============================

  // The following methods are used to color special live ranges (e.g.
  // function args and return values etc.) with specific hardware registers
  // as required. See SparcRegInfo.cpp for the implementation.
  //
  void suggestReg4RetAddr(MachineInstr *RetMI, 
			  LiveRangeInfo &LRI) const;

  void suggestReg4CallAddr(MachineInstr *CallMI, LiveRangeInfo &LRI) const;
  
  void InitializeOutgoingArg(MachineInstr* CallMI, AddedInstrns *CallAI,
                             PhyRegAlloc &PRA, LiveRange* LR,
                             unsigned regType, unsigned RegClassID,
                             int  UniArgReg, unsigned int argNo,
                             std::vector<MachineInstr *>& AddedInstrnsBefore)
    const;
  
  // The following 4 methods are used to find the RegType (see enum above)
  // for a reg class and a given primitive type, a LiveRange, a Value,
  // or a particular machine register.
  // The fifth function gives the reg class of the given RegType.
  // 
  int getRegType(unsigned regClassID, const Type* type) const;
  int getRegType(const LiveRange *LR) const;
  int getRegType(const Value *Val) const;
  int getRegType(int unifiedRegNum) const;

  // Used to generate a copy instruction based on the register class of
  // value.
  //
  MachineInstr *cpValue2RegMI(Value *Val,  unsigned DestReg,
                              int RegType) const;


  // The following 2 methods are used to order the instructions addeed by
  // the register allocator in association with function calling. See
  // SparcRegInfo.cpp for more details
  //
  void moveInst2OrdVec(std::vector<MachineInstr *> &OrdVec,
                       MachineInstr *UnordInst,
		       PhyRegAlloc &PRA) const;

  void OrderAddedInstrns(std::vector<MachineInstr *> &UnordVec, 
                         std::vector<MachineInstr *> &OrdVec,
                         PhyRegAlloc &PRA) const;


  // Compute which register can be used for an argument, if any
  // 
  int regNumForIntArg(bool inCallee, bool isVarArgsCall,
                      unsigned argNo, unsigned intArgNo, unsigned fpArgNo,
                      unsigned& regClassId) const;

  int regNumForFPArg(unsigned RegType, bool inCallee, bool isVarArgsCall,
                     unsigned argNo, unsigned intArgNo, unsigned fpArgNo,
                     unsigned& regClassId) const;
  
public:
  UltraSparcRegInfo(const UltraSparc &tgt);

  // To find the register class used for a specified Type
  //
  unsigned getRegClassIDOfType(const Type *type,
                               bool isCCReg = false) const;

  // To find the register class of a Value
  //
  inline unsigned getRegClassIDOfValue(const Value *Val,
                                       bool isCCReg = false) const {
    return getRegClassIDOfType(Val->getType(), isCCReg);
  }

  // To find the register class to which a specified register belongs
  //
  unsigned getRegClassIDOfReg(int unifiedRegNum) const;
  unsigned getRegClassIDOfRegType(int regType) const;
  
  // getZeroRegNum - returns the register that contains always zero this is the
  // unified register number
  //
  virtual int getZeroRegNum() const;

  // getCallAddressReg - returns the reg used for pushing the address when a
  // function is called. This can be used for other purposes between calls
  //
  unsigned getCallAddressReg() const;

  // Returns the register containing the return address.
  // It should be made sure that this  register contains the return 
  // value when a return instruction is reached.
  //
  unsigned getReturnAddressReg() const;

  // Number of registers used for passing int args (usually 6: %o0 - %o5)
  // and float args (usually 32: %f0 - %f31)
  //
  unsigned const GetNumOfIntArgRegs() const   { return NumOfIntArgRegs; }
  unsigned const GetNumOfFloatArgRegs() const { return NumOfFloatArgRegs; }
  
  // The following methods are used to color special live ranges (e.g.
  // function args and return values etc.) with specific hardware registers
  // as required. See SparcRegInfo.cpp for the implementation for Sparc.
  //
  void suggestRegs4MethodArgs(const Function *Meth, 
			      LiveRangeInfo& LRI) const;

  void suggestRegs4CallArgs(MachineInstr *CallMI, 
			    LiveRangeInfo& LRI) const; 

  void suggestReg4RetValue(MachineInstr *RetMI, 
                           LiveRangeInfo& LRI) const;
  
  void colorMethodArgs(const Function *Meth,  LiveRangeInfo &LRI,
		       AddedInstrns *FirstAI) const;

  void colorCallArgs(MachineInstr *CallMI, LiveRangeInfo &LRI,
		     AddedInstrns *CallAI,  PhyRegAlloc &PRA,
		     const BasicBlock *BB) const;

  void colorRetValue(MachineInstr *RetI,   LiveRangeInfo& LRI,
		     AddedInstrns *RetAI) const;


  // method used for printing a register for debugging purposes
  //
  static void printReg(const LiveRange *LR);

  // Each register class has a seperate space for register IDs. To convert
  // a regId in a register class to a common Id, or vice versa,
  // we use the folloing methods.
  //
  // This method provides a unique number for each register 
  inline int getUnifiedRegNum(unsigned regClassID, int reg) const {
    
    if (regClassID == IntRegClassID) {
      assert(reg < 32 && "Invalid reg. number");
      return reg;
    }
    else if (regClassID == FloatRegClassID) {
      assert(reg < 64 && "Invalid reg. number");
      return reg + 32;                  // we have 32 int regs
    }
    else if (regClassID == FloatCCRegClassID) {
      assert(reg < 4 && "Invalid reg. number");
      return reg + 32 + 64;             // 32 int, 64 float
    }
    else if (regClassID == IntCCRegClassID ) {
      assert(reg == 0 && "Invalid reg. number");
      return reg + 4+ 32 + 64;          // only one int CC reg
    }
    else if (reg==InvalidRegNum) {
      return InvalidRegNum;
    }
    else  
      assert(0 && "Invalid register class");
    return 0;
  }
  
  // This method converts the unified number to the number in its class,
  // and returns the class ID in regClassID.
  inline int getClassRegNum(int ureg, unsigned& regClassID) const {
    if      (ureg < 32)     { regClassID = IntRegClassID;     return ureg;    }
    else if (ureg < 32+64)  { regClassID = FloatRegClassID;   return ureg-32; }
    else if (ureg < 4 +96)  { regClassID = FloatCCRegClassID; return ureg-96; }
    else if (ureg < 1 +100) { regClassID = IntCCRegClassID;   return ureg-100;}
    else if (ureg == InvalidRegNum) { return InvalidRegNum; }
    else { assert(0 && "Invalid unified register number"); }
    return 0;
  }
  
  // Returns the assembly-language name of the specified machine register.
  //
  virtual const char * const getUnifiedRegName(int reg) const;


  // returns the # of bytes of stack space allocated for each register
  // type. For Sparc, currently we allocate 8 bytes on stack for all 
  // register types. We can optimize this later if necessary to save stack
  // space (However, should make sure that stack alignment is correct)
  //
  inline int getSpilledRegSize(int RegType) const {
    return 8;
  }


  // To obtain the return value and the indirect call address (if any)
  // contained in a CALL machine instruction
  //
  const Value * getCallInstRetVal(const MachineInstr *CallMI) const;
  const Value * getCallInstIndirectAddrVal(const MachineInstr *CallMI) const;

  // The following methods are used to generate "copy" machine instructions
  // for an architecture.
  //
  // The function regTypeNeedsScratchReg() can be used to check whether a
  // scratch register is needed to copy a register of type `regType' to
  // or from memory.  If so, such a scratch register can be provided by
  // the caller (e.g., if it knows which regsiters are free); otherwise
  // an arbitrary one will be chosen and spilled by the copy instructions.
  //
  bool regTypeNeedsScratchReg(int RegType,
                              int& scratchRegClassId) const;

  void cpReg2RegMI(std::vector<MachineInstr*>& mvec,
                   unsigned SrcReg, unsigned DestReg,
                   int RegType) const;

  void cpReg2MemMI(std::vector<MachineInstr*>& mvec,
                   unsigned SrcReg, unsigned DestPtrReg,
                   int Offset, int RegType, int scratchReg = -1) const;

  void cpMem2RegMI(std::vector<MachineInstr*>& mvec,
                   unsigned SrcPtrReg, int Offset, unsigned DestReg,
                   int RegType, int scratchReg = -1) const;

  void cpValue2Value(Value *Src, Value *Dest,
                     std::vector<MachineInstr*>& mvec) const;

  // To see whether a register is a volatile (i.e., whehter it must be
  // preserved acorss calls)
  //
  inline bool isRegVolatile(int RegClassID, int Reg) const {
    return MachineRegClassArr[RegClassID]->isRegVolatile(Reg);
  }


  virtual unsigned getFramePointer() const;
  virtual unsigned getStackPointer() const;

  virtual int getInvalidRegNum() const {
    return InvalidRegNum;
  }

  // This method inserts the caller saving code for call instructions
  //
  void insertCallerSavingCode(std::vector<MachineInstr*>& instrnsBefore,
                              std::vector<MachineInstr*>& instrnsAfter,
                              MachineInstr *MInst, 
			      const BasicBlock *BB, PhyRegAlloc &PRA ) const;
};




//---------------------------------------------------------------------------
// class UltraSparcSchedInfo
// 
// Purpose:
//   Interface to instruction scheduling information for UltraSPARC.
//   The parameter values above are based on UltraSPARC IIi.
//---------------------------------------------------------------------------


class UltraSparcSchedInfo: public MachineSchedInfo {
public:
  UltraSparcSchedInfo(const TargetMachine &tgt);
protected:
  virtual void initializeResources();
};


//---------------------------------------------------------------------------
// class UltraSparcFrameInfo 
// 
// Purpose:
//   Interface to stack frame layout info for the UltraSPARC.
//   Starting offsets for each area of the stack frame are aligned at
//   a multiple of getStackFrameSizeAlignment().
//---------------------------------------------------------------------------

class UltraSparcFrameInfo: public MachineFrameInfo {
public:
  UltraSparcFrameInfo(const TargetMachine &tgt) : MachineFrameInfo(tgt) {}
  
public:
  // These methods provide constant parameters of the frame layout.
  // 
  int  getStackFrameSizeAlignment() const { return StackFrameSizeAlignment;}
  int  getMinStackFrameSize()       const { return MinStackFrameSize; }
  int  getNumFixedOutgoingArgs()    const { return NumFixedOutgoingArgs; }
  int  getSizeOfEachArgOnStack()    const { return SizeOfEachArgOnStack; }
  bool argsOnStackHaveFixedSize()   const { return true; }

  // This method adjusts a stack offset to meet alignment rules of target.
  // The fixed OFFSET (0x7ff) must be subtracted and the result aligned.
  virtual int  adjustAlignment                  (int unalignedOffset,
                                                 bool growUp,
                                                 unsigned int align) const {
    return unalignedOffset + (growUp? +1:-1)*((unalignedOffset-OFFSET) % align);
  }

  // These methods compute offsets using the frame contents for a
  // particular function.  The frame contents are obtained from the
  // MachineCodeInfoForMethod object for the given function.
  // 
  int getFirstIncomingArgOffset  (MachineFunction& mcInfo,
                                  bool& growUp) const
  {
    growUp = true;                         // arguments area grows upwards
    return FirstIncomingArgOffsetFromFP;
  }
  int getFirstOutgoingArgOffset  (MachineFunction& mcInfo,
                                  bool& growUp) const
  {
    growUp = true;                         // arguments area grows upwards
    return FirstOutgoingArgOffsetFromSP;
  }
  int getFirstOptionalOutgoingArgOffset(MachineFunction& mcInfo,
                                        bool& growUp)const
  {
    growUp = true;                         // arguments area grows upwards
    return FirstOptionalOutgoingArgOffsetFromSP;
  }
  
  int getFirstAutomaticVarOffset (MachineFunction& mcInfo,
                                  bool& growUp) const;
  int getRegSpillAreaOffset      (MachineFunction& mcInfo,
                                  bool& growUp) const;
  int getTmpAreaOffset           (MachineFunction& mcInfo,
                                  bool& growUp) const;
  int getDynamicAreaOffset       (MachineFunction& mcInfo,
                                  bool& growUp) const;

  //
  // These methods specify the base register used for each stack area
  // (generally FP or SP)
  // 
  virtual int getIncomingArgBaseRegNum()               const {
    return (int) target.getRegInfo().getFramePointer();
  }
  virtual int getOutgoingArgBaseRegNum()               const {
    return (int) target.getRegInfo().getStackPointer();
  }
  virtual int getOptionalOutgoingArgBaseRegNum()       const {
    return (int) target.getRegInfo().getStackPointer();
  }
  virtual int getAutomaticVarBaseRegNum()              const {
    return (int) target.getRegInfo().getFramePointer();
  }
  virtual int getRegSpillAreaBaseRegNum()              const {
    return (int) target.getRegInfo().getFramePointer();
  }
  virtual int getDynamicAreaBaseRegNum()               const {
    return (int) target.getRegInfo().getStackPointer();
  }
  
private:
  /*----------------------------------------------------------------------
    This diagram shows the stack frame layout used by llc on Sparc V9.
    Note that only the location of automatic variables, spill area,
    temporary storage, and dynamically allocated stack area are chosen
    by us.  The rest conform to the Sparc V9 ABI.
    All stack addresses are offset by OFFSET = 0x7ff (2047).

    Alignment assumpteions and other invariants:
    (1) %sp+OFFSET and %fp+OFFSET are always aligned on 16-byte boundary
    (2) Variables in automatic, spill, temporary, or dynamic regions
        are aligned according to their size as in all memory accesses.
    (3) Everything below the dynamically allocated stack area is only used
        during a call to another function, so it is never needed when
        the current function is active.  This is why space can be allocated
        dynamically by incrementing %sp any time within the function.
    
    STACK FRAME LAYOUT:

       ...
       %fp+OFFSET+176      Optional extra incoming arguments# 1..N
       %fp+OFFSET+168      Incoming argument #6
       ...                 ...
       %fp+OFFSET+128      Incoming argument #1
       ...                 ...
    ---%fp+OFFSET-0--------Bottom of caller's stack frame--------------------
       %fp+OFFSET-8        Automatic variables <-- ****TOP OF STACK FRAME****
                           Spill area
                           Temporary storage
       ...

       %sp+OFFSET+176+8N   Bottom of dynamically allocated stack area
       %sp+OFFSET+168+8N   Optional extra outgoing argument# N
       ...                 ...
       %sp+OFFSET+176      Optional extra outgoing argument# 1
       %sp+OFFSET+168      Outgoing argument #6
       ...                 ...
       %sp+OFFSET+128      Outgoing argument #1
       %sp+OFFSET+120      Save area for %i7
       ...                 ...
       %sp+OFFSET+0        Save area for %l0 <-- ****BOTTOM OF STACK FRAME****

   *----------------------------------------------------------------------*/

  // All stack addresses must be offset by 0x7ff (2047) on Sparc V9.
  static const int OFFSET                                  = (int) 0x7ff;
  static const int StackFrameSizeAlignment                 =  16;
  static const int MinStackFrameSize                       = 176;
  static const int NumFixedOutgoingArgs                    =   6;
  static const int SizeOfEachArgOnStack                    =   8;
  static const int FirstIncomingArgOffsetFromFP            = 128 + OFFSET;
  static const int FirstOptionalIncomingArgOffsetFromFP    = 176 + OFFSET;
  static const int StaticAreaOffsetFromFP                  =   0 + OFFSET;
  static const int FirstOutgoingArgOffsetFromSP            = 128 + OFFSET;
  static const int FirstOptionalOutgoingArgOffsetFromSP    = 176 + OFFSET;
};


//---------------------------------------------------------------------------
// class UltraSparcCacheInfo 
// 
// Purpose:
//   Interface to cache parameters for the UltraSPARC.
//   Just use defaults for now.
//---------------------------------------------------------------------------

class UltraSparcCacheInfo: public MachineCacheInfo {
public:
  UltraSparcCacheInfo(const TargetMachine &T) : MachineCacheInfo(T) {} 
};


//---------------------------------------------------------------------------
// class UltraSparcOptInfo 
// 
// Purpose:
//   Interface to machine-level optimization routines for the UltraSPARC.
//---------------------------------------------------------------------------

class UltraSparcOptInfo: public MachineOptInfo {
public:
  UltraSparcOptInfo(const TargetMachine &T) : MachineOptInfo(T) {} 

  virtual bool IsUselessCopy    (const MachineInstr* MI) const;
};


//---------------------------------------------------------------------------
// class UltraSparcMachine 
// 
// Purpose:
//   Primary interface to machine description for the UltraSPARC.
//   Primarily just initializes machine-dependent parameters in
//   class TargetMachine, and creates machine-dependent subclasses
//   for classes such as InstrInfo, SchedInfo and RegInfo. 
//---------------------------------------------------------------------------

class UltraSparc : public TargetMachine {
  UltraSparcInstrInfo instrInfo;
  UltraSparcSchedInfo schedInfo;
  UltraSparcRegInfo   regInfo;
  UltraSparcFrameInfo frameInfo;
  UltraSparcCacheInfo cacheInfo;
  UltraSparcOptInfo   optInfo;
public:
  UltraSparc();

  virtual const MachineInstrInfo &getInstrInfo() const { return instrInfo; }
  virtual const MachineSchedInfo &getSchedInfo() const { return schedInfo; }
  virtual const MachineRegInfo   &getRegInfo()   const { return regInfo; }
  virtual const MachineFrameInfo &getFrameInfo() const { return frameInfo; }
  virtual const MachineCacheInfo &getCacheInfo() const { return cacheInfo; }
  virtual const MachineOptInfo   &getOptInfo()   const { return optInfo; }

  virtual bool addPassesToEmitAssembly(PassManager &PM, std::ostream &Out);

  // getPrologEpilogInsertionPass - Inserts prolog/epilog code.
  Pass* getPrologEpilogInsertionPass();

  // getFunctionAsmPrinterPass - Writes out machine code for a single function
  Pass* getFunctionAsmPrinterPass(std::ostream &Out);

  // getModuleAsmPrinterPass - Writes generated machine code to assembly file.
  Pass* getModuleAsmPrinterPass(std::ostream &Out);

  // getEmitBytecodeToAsmPass - Emits final LLVM bytecode to assembly file.
  Pass* getEmitBytecodeToAsmPass(std::ostream &Out);
};

#endif
