// $Id$ -*- C++ -*--
//***************************************************************************
// File:
//	SparcInternals.h
// 
// Purpose:
//       This file defines stuff that is to be private to the Sparc
//       backend, but is shared among different portions of the backend.
//**************************************************************************/


#ifndef SPARC_INTERNALS_H
#define SPARC_INTERNALS_H


#include "SparcRegClassInfo.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/MachineInstrInfo.h"
#include "llvm/Target/MachineSchedInfo.h"
#include "llvm/Target/MachineFrameInfo.h"
#include "llvm/Target/MachineCacheInfo.h"
#include "llvm/CodeGen/RegClass.h"
#include "llvm/Type.h"
#include <sys/types.h>

class LiveRange;
class UltraSparc;
class PhyRegAlloc;


// OpCodeMask definitions for the Sparc V9
// 
const OpCodeMask	Immed		= 0x00002000; // immed or reg operand?
const OpCodeMask	Annul		= 0x20000000; // annul delay instr?
const OpCodeMask	PredictTaken	= 0x00080000; // predict branch taken?


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

class UltraSparcInstrInfo : public MachineInstrInfo {
public:
  /*ctor*/	UltraSparcInstrInfo(const TargetMachine& tgt);

  //
  // All immediate constants are in position 0 except the
  // store instructions.
  // 
  virtual int getImmmedConstantPos(MachineOpCode opCode) const {
    bool ignore;
    if (this->maxImmedConstant(opCode, ignore) != 0)
      {
        assert(! this->isStore((MachineOpCode) STB - 1)); // first store is STB
        assert(! this->isStore((MachineOpCode) STD + 1)); // last  store is STD
        return (opCode >= STB || opCode <= STD)? 2 : 1;
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
  // Code generation support for creating individual machine instructions
  //-------------------------------------------------------------------------
  
  // Create an instruction sequence to put the constant `val' into
  // the virtual register `dest'.  The generated instructions are
  // returned in `minstrVec'.  Any temporary registers (TmpInstruction)
  // created are returned in `tempVec'.
  // 
  virtual void  CreateCodeToLoadConst(Value* val,
                                      Instruction* dest,
                                      std::vector<MachineInstr*>& minstrVec,
                                      std::vector<TmpInstruction*>& tmp) const;

  
  // Create an instruction sequence to copy an integer value `val'
  // to a floating point value `dest' by copying to memory and back.
  // val must be an integral type.  dest must be a Float or Double.
  // The generated instructions are returned in `minstrVec'.
  // Any temp. registers (TmpInstruction) created are returned in `tempVec'.
  // 
  virtual void  CreateCodeToCopyIntToFloat(Method* method,
                                           Value* val,
                                           Instruction* dest,
                                           std::vector<MachineInstr*>& minstr,
                                           std::vector<TmpInstruction*>& temp,
                                           TargetMachine& target) const;

  // Similarly, create an instruction sequence to copy an FP value
  // `val' to an integer value `dest' by copying to memory and back.
  // See the previous function for information about return values.
  // 
  virtual void  CreateCodeToCopyFloatToInt(Method* method,
                                           Value* val,
                                           Instruction* dest,
                                           std::vector<MachineInstr*>& minstr,
                                           std::vector<TmpInstruction*>& temp,
                                           TargetMachine& target) const;

 // create copy instruction(s)
  virtual void
  CreateCopyInstructionsByType(const TargetMachine& target,
                             Value* src,
                             Instruction* dest,
                             std::vector<MachineInstr*>& minstr) const;


};


//----------------------------------------------------------------------------
// class UltraSparcRegInfo
//
// This class implements the virtual class MachineRegInfo for Sparc.
//
//----------------------------------------------------------------------------

class UltraSparcRegInfo : public MachineRegInfo
{
 private:

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


  // reverse pointer to get info about the ultra sparc machine
  //
  const UltraSparc *const UltraSparcInfo;

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
  // method args and return values etc.) with specific hardware registers
  // as required. See SparcRegInfo.cpp for the implementation.
  //
  void setCallOrRetArgCol(LiveRange *const LR, const unsigned RegNo,
			 const MachineInstr *MI,AddedInstrMapType &AIMap)const;

  MachineInstr * getCopy2RegMI(const Value *SrcVal, const unsigned Reg,
			       unsigned RegClassID) const ;

  void suggestReg4RetAddr(const MachineInstr * RetMI, 
			  LiveRangeInfo& LRI) const;

  void suggestReg4CallAddr(const MachineInstr * CallMI, LiveRangeInfo& LRI,
			   std::vector<RegClass *> RCList) const;



  // The following methods are used to find the addresses etc. contained
  // in specail machine instructions like CALL/RET
  //
  Value *getValue4ReturnAddr( const MachineInstr * MInst ) const ;
  const Value *getCallInstRetAddr(const MachineInstr *CallMI) const;
  const unsigned getCallInstNumArgs(const MachineInstr *CallMI) const;


  // The following 3  methods are used to find the RegType (see enum above)
  // of a LiveRange, Value and using the unified RegClassID

  int getRegType(const LiveRange *const LR) const {

    unsigned Typ;

    switch(  (LR->getRegClass())->getID() ) {

    case IntRegClassID: return IntRegType; 

    case FloatRegClassID: 
                          Typ =  LR->getTypeID();
			  if( Typ == Type::FloatTyID ) 
			    return FPSingleRegType;
                          else if( Typ == Type::DoubleTyID )
			    return FPDoubleRegType;
                          else assert(0 && "Unknown type in FloatRegClass");

    case IntCCRegClassID: return IntCCRegType; 
      
    case FloatCCRegClassID: return FloatCCRegType ; 

    default: assert( 0 && "Unknown reg class ID");
      return 0;
    }
  }


  int getRegType(const Value *const Val) const {

    unsigned Typ;

    switch( getRegClassIDOfValue(Val)  ) {

    case IntRegClassID: return IntRegType; 

    case FloatRegClassID: 
                          Typ =  (Val->getType())->getPrimitiveID();
			  if( Typ == Type::FloatTyID ) 
			    return FPSingleRegType;
                          else if( Typ == Type::DoubleTyID )
			    return FPDoubleRegType;
                          else assert(0 && "Unknown type in FloatRegClass");

    case IntCCRegClassID: return IntCCRegType; 
      
    case FloatCCRegClassID: return FloatCCRegType ; 

    default: assert( 0 && "Unknown reg class ID");
      return 0;
    }

  }


  int getRegType(int reg) const {
    if( reg < 32 ) 
      return IntRegType;
    else if ( reg < (32 + 32) )
      return FPSingleRegType;
    else if ( reg < (64 + 32) )
      return FPDoubleRegType;
    else if( reg < (64+32+4) )
      return FloatCCRegType;
    else if( reg < (64+32+4+2) )  
      return IntCCRegType;             
    else 
      assert(0 && "Invalid register number in getRegType");
  }




  // The following methods are used to generate copy instructions to move
  // data between condition code registers
  //
  MachineInstr * cpCCR2IntMI(const unsigned IntReg) const;
  MachineInstr * cpInt2CCRMI(const unsigned IntReg) const;

  // Used to generate a copy instruction based on the register class of
  // value.
  //
  MachineInstr * cpValue2RegMI(Value * Val,  const unsigned DestReg,
			       const int RegType) const;


  // The following 2 methods are used to order the instructions addeed by
  // the register allocator in association with method calling. See
  // SparcRegInfo.cpp for more details
  //
  void moveInst2OrdVec(std::vector<MachineInstr *> &OrdVec,
                       MachineInstr *UnordInst,
		       PhyRegAlloc &PRA) const;

  void OrderAddedInstrns(std::vector<MachineInstr *> &UnordVec, 
                         std::vector<MachineInstr *> &OrdVec,
                         PhyRegAlloc &PRA) const;


  // To find whether a particular call is to a var arg method
  //
  bool isVarArgCall(const MachineInstr *CallMI) const;



 public:

  // constructor
  //
  UltraSparcRegInfo(const TargetMachine& tgt ) :    
    MachineRegInfo(tgt),
    UltraSparcInfo(& (const UltraSparc&) tgt), 
    NumOfIntArgRegs(6), 
    NumOfFloatArgRegs(32),
    InvalidRegNum(1000) {
   
    MachineRegClassArr.push_back( new SparcIntRegClass(IntRegClassID) );
    MachineRegClassArr.push_back( new SparcFloatRegClass(FloatRegClassID) );
    MachineRegClassArr.push_back( new SparcIntCCRegClass(IntCCRegClassID) );
    MachineRegClassArr.push_back( new SparcFloatCCRegClass(FloatCCRegClassID));

    assert( SparcFloatRegOrder::StartOfNonVolatileRegs == 32 && 
	    "32 Float regs are used for float arg passing");

  }


  ~UltraSparcRegInfo(void) { }          // empty destructor 


  // To get complete machine information structure using the machine register
  // information
  //
  inline const UltraSparc & getUltraSparcInfo() const { 
    return *UltraSparcInfo;
  }


  // To find the register class of a Value
  //
  inline unsigned getRegClassIDOfValue (const Value *const Val,
				 bool isCCReg = false) const {

    Type::PrimitiveID ty = (Val->getType())->getPrimitiveID();

    unsigned res;
    
    if( (ty && ty <= Type::LongTyID) || (ty == Type::LabelTyID) ||
	(ty == Type::MethodTyID) ||  (ty == Type::PointerTyID) )
      res =  IntRegClassID;             // sparc int reg (ty=0: void)
    else if( ty <= Type::DoubleTyID)
      res = FloatRegClassID;           // sparc float reg class
    else { 
      std::cerr << "TypeID: " << ty << "\n";
      assert(0 && "Cannot resolve register class for type");
      return 0;
    }

    if(isCCReg)
      return res + 2;      // corresponidng condition code regiser 
    else 
      return res;
  }



  // returns the register that contains always zero
  // this is the unified register number
  //
  inline int getZeroRegNum() const { return SparcIntRegOrder::g0; }

  // returns the reg used for pushing the address when a method is called.
  // This can be used for other purposes between calls
  //
  unsigned getCallAddressReg() const  { return SparcIntRegOrder::o7; }

  // Returns the register containing the return address.
  // It should be made sure that this  register contains the return 
  // value when a return instruction is reached.
  //
  unsigned getReturnAddressReg()  const { return SparcIntRegOrder::i7; }



  // The following methods are used to color special live ranges (e.g.
  // method args and return values etc.) with specific hardware registers
  // as required. See SparcRegInfo.cpp for the implementation for Sparc.
  //
  void suggestRegs4MethodArgs(const Method *const Meth, 
			      LiveRangeInfo& LRI) const;

  void suggestRegs4CallArgs(const MachineInstr *const CallMI, 
			    LiveRangeInfo& LRI,
                            std::vector<RegClass *> RCL) const; 

  void suggestReg4RetValue(const MachineInstr *const RetMI, 
                           LiveRangeInfo& LRI) const;


  void colorMethodArgs(const Method *const Meth,  LiveRangeInfo& LRI,
		       AddedInstrns *const FirstAI) const;

  void colorCallArgs(const MachineInstr *const CallMI, LiveRangeInfo& LRI,
		     AddedInstrns *const CallAI,  PhyRegAlloc &PRA,
		     const BasicBlock *BB) const;

  void colorRetValue(const MachineInstr *const RetI,   LiveRangeInfo& LRI,
		     AddedInstrns *const RetAI) const;



  // method used for printing a register for debugging purposes
  //
  static void printReg(const LiveRange *const LR)  ;

  // this method provides a unique number for each register 
  //
  inline int getUnifiedRegNum(int RegClassID, int reg) const {

    if( RegClassID == IntRegClassID && reg < 32 ) 
      return reg;
    else if ( RegClassID == FloatRegClassID && reg < 64)
      return reg + 32;                  // we have 32 int regs
    else if( RegClassID == FloatCCRegClassID && reg < 4)
      return reg + 32 + 64;             // 32 int, 64 float
    else if( RegClassID == IntCCRegClassID ) 
      return 4+ 32 + 64;                // only int cc reg
    else if (reg==InvalidRegNum)                
      return InvalidRegNum;
    else  
      assert(0 && "Invalid register class or reg number");
    return 0;
  }

  // given the unified register number, this gives the name
  // for generating assembly code or debugging.
  //
  inline const std::string getUnifiedRegName(int reg) const {
    if( reg < 32 ) 
      return SparcIntRegOrder::getRegName(reg);
    else if ( reg < (64 + 32) )
      return SparcFloatRegOrder::getRegName( reg  - 32);                  
    else if( reg < (64+32+4) )
      return SparcFloatCCRegOrder::getRegName( reg -32 - 64);
    else if( reg < (64+32+4+2) )    // two names: %xcc and %ccr
      return SparcIntCCRegOrder::getRegName( reg -32 - 64 - 4);             
    else if (reg== InvalidRegNum)       //****** TODO: Remove */
      return "<*NoReg*>";
    else 
      assert(0 && "Invalid register number");
    return "";
  }



  // The fllowing methods are used by instruction selection
  //
  inline unsigned getRegNumInCallersWindow(int reg) {
    if (reg == InvalidRegNum || reg >= 32)
      return reg;
    return SparcIntRegOrder::getRegNumInCallersWindow(reg);
  }
  
  inline bool mustBeRemappedInCallersWindow(int reg) {
    return (reg != InvalidRegNum && reg < 32);
  }
  


  // returns the # of bytes of stack space allocated for each register
  // type. For Sparc, currently we allocate 8 bytes on stack for all 
  // register types. We can optimize this later if necessary to save stack
  // space (However, should make sure that stack alignment is correct)
  //
  inline int getSpilledRegSize(const int RegType) const {
    return 8;
  }


  // To obtain the return value contained in a CALL machine instruction
  //
  const Value * getCallInstRetVal(const MachineInstr *CallMI) const;


  // The following methods are used to generate "copy" machine instructions
  // for an architecture.
  //
  MachineInstr * cpReg2RegMI(const unsigned SrcReg, const unsigned DestReg,
			     const int RegType) const;

  MachineInstr * cpReg2MemMI(const unsigned SrcReg,  const unsigned DestPtrReg,
			     const int Offset, const int RegType) const;

  MachineInstr * cpMem2RegMI(const unsigned SrcPtrReg, const int Offset,
			     const unsigned DestReg, const int RegType) const;

  MachineInstr* cpValue2Value(Value *Src, Value *Dest) const;


  // To see whether a register is a volatile (i.e., whehter it must be
  // preserved acorss calls)
  //
  inline bool isRegVolatile(const int RegClassID, const int Reg) const {
    return  (MachineRegClassArr[RegClassID])->isRegVolatile(Reg);
  }


  inline unsigned getFramePointer() const {
    return SparcIntRegOrder::i6;
  }

  inline unsigned getStackPointer() const {
    return SparcIntRegOrder::o6;
  }

  inline int getInvalidRegNum() const {
    return InvalidRegNum;
  }



  // This method inserts the caller saving code for call instructions
  //
  void insertCallerSavingCode(const MachineInstr *MInst, 
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
  /*ctor*/	   UltraSparcSchedInfo	(const TargetMachine& tgt);
  /*dtor*/ virtual ~UltraSparcSchedInfo	() {}
protected:
  virtual void	initializeResources	();
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
  /*ctor*/ UltraSparcFrameInfo(const TargetMachine& tgt) : MachineFrameInfo(tgt) {}
  
public:
  int  getStackFrameSizeAlignment   () const { return StackFrameSizeAlignment;}
  int  getMinStackFrameSize         () const { return MinStackFrameSize; }
  int  getNumFixedOutgoingArgs      () const { return NumFixedOutgoingArgs; }
  int  getSizeOfEachArgOnStack      () const { return SizeOfEachArgOnStack; }
  bool argsOnStackHaveFixedSize     () const { return true; }

  //
  // These methods compute offsets using the frame contents for a
  // particular method.  The frame contents are obtained from the
  // MachineCodeInfoForMethod object for the given method.
  // 
  int getFirstIncomingArgOffset  (MachineCodeForMethod& mcInfo,
                                  bool& pos) const
  {
    pos = true;                         // arguments area grows upwards
    return FirstIncomingArgOffsetFromFP;
  }
  int getFirstOutgoingArgOffset  (MachineCodeForMethod& mcInfo,
                                  bool& pos) const
  {
    pos = true;                         // arguments area grows upwards
    return FirstOutgoingArgOffsetFromSP;
  }
  int getFirstOptionalOutgoingArgOffset(MachineCodeForMethod& mcInfo,
                                        bool& pos)const
  {
    pos = true;                         // arguments area grows upwards
    return FirstOptionalOutgoingArgOffsetFromSP;
  }
  
  int getFirstAutomaticVarOffset (MachineCodeForMethod& mcInfo,
                                  bool& pos) const;
  int getRegSpillAreaOffset      (MachineCodeForMethod& mcInfo,
                                  bool& pos) const;
  int getTmpAreaOffset           (MachineCodeForMethod& mcInfo,
                                  bool& pos) const;
  int getDynamicAreaOffset       (MachineCodeForMethod& mcInfo,
                                  bool& pos) const;

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
  // All stack addresses must be offset by 0x7ff (2047) on Sparc V9.
  static const int OFFSET                                  = (int) 0x7ff;
  static const int StackFrameSizeAlignment                 =  16;
  static const int MinStackFrameSize                       = 176;
  static const int NumFixedOutgoingArgs                    =   6;
  static const int SizeOfEachArgOnStack                    =   8;
  static const int StaticAreaOffsetFromFP                  =  0 + OFFSET;
  static const int FirstIncomingArgOffsetFromFP            = 128 + OFFSET;
  static const int FirstOptionalIncomingArgOffsetFromFP    = 176 + OFFSET;
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
// class UltraSparcMachine 
// 
// Purpose:
//   Primary interface to machine description for the UltraSPARC.
//   Primarily just initializes machine-dependent parameters in
//   class TargetMachine, and creates machine-dependent subclasses
//   for classes such as InstrInfo, SchedInfo and RegInfo. 
//---------------------------------------------------------------------------

class UltraSparc : public TargetMachine {
private:
  UltraSparcInstrInfo instrInfo;
  UltraSparcSchedInfo schedInfo;
  UltraSparcRegInfo   regInfo;
  UltraSparcFrameInfo frameInfo;
  UltraSparcCacheInfo cacheInfo;
public:
  UltraSparc();
  
  virtual const MachineInstrInfo &getInstrInfo() const { return instrInfo; }
  virtual const MachineSchedInfo &getSchedInfo() const { return schedInfo; }
  virtual const MachineRegInfo   &getRegInfo()   const { return regInfo; }
  virtual const MachineFrameInfo &getFrameInfo() const { return frameInfo; }
  virtual const MachineCacheInfo &getCacheInfo() const { return cacheInfo; }

  //
  // addPassesToEmitAssembly - Add passes to the specified pass manager to get
  // assembly langage code emited.  For sparc, we have to do ...
  //
  virtual void addPassesToEmitAssembly(PassManager &PM, std::ostream &Out);

private:
  Pass *getMethodAsmPrinterPass(PassManager &PM, std::ostream &Out);
  Pass *getModuleAsmPrinterPass(PassManager &PM, std::ostream &Out);
};

#endif
