//===-- SparcV9CodeEmitter.cpp --------------------------------------------===//
//
// FIXME: document
//
//===----------------------------------------------------------------------===//

#include "llvm/Constants.h"
#include "llvm/Function.h"
#include "llvm/GlobalVariable.h"
#include "llvm/PassManager.h"
#include "llvm/CodeGen/MachineCodeEmitter.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFunctionInfo.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetData.h"
#include "Support/Debug.h"
#include "Support/hash_set"
#include "SparcInternals.h"
#include "SparcV9CodeEmitter.h"

bool UltraSparc::addPassesToEmitMachineCode(PassManager &PM,
                                            MachineCodeEmitter &MCE) {
  MachineCodeEmitter *M = &MCE;
  DEBUG(M = MachineCodeEmitter::createFilePrinterEmitter(MCE));
  PM.add(new SparcV9CodeEmitter(*this, *M));
  PM.add(createMachineCodeDestructionPass()); // Free stuff no longer needed
  return false;
}

namespace {
  class JITResolver {
    SparcV9CodeEmitter &SparcV9;
    MachineCodeEmitter &MCE;

    /// LazyCodeGenMap - Keep track of call sites for functions that are to be
    /// lazily resolved.
    ///
    std::map<uint64_t, Function*> LazyCodeGenMap;

    /// LazyResolverMap - Keep track of the lazy resolver created for a
    /// particular function so that we can reuse them if necessary.
    ///
    std::map<Function*, uint64_t> LazyResolverMap;

  public:
    enum CallType { ShortCall, FarCall };

  private:
    /// We need to keep track of whether we used a simple call or a far call
    /// (many instructions) in sequence. This means we need to keep track of
    /// what type of stub we generate.
    static std::map<uint64_t, CallType> LazyCallFlavor;

  public:
    JITResolver(SparcV9CodeEmitter &V9,
                MachineCodeEmitter &mce) : SparcV9(V9), MCE(mce) {}
    uint64_t getLazyResolver(Function *F);
    uint64_t addFunctionReference(uint64_t Address, Function *F);
    void deleteFunctionReference(uint64_t Address);
    void addCallFlavor(uint64_t Address, CallType Flavor) {
      LazyCallFlavor[Address] = Flavor;
    }

    // Utility functions for accessing data from static callback
    uint64_t getCurrentPCValue() {
      return MCE.getCurrentPCValue();
    }
    unsigned getBinaryCodeForInstr(MachineInstr &MI) {
      return SparcV9.getBinaryCodeForInstr(MI);
    }

    inline uint64_t insertFarJumpAtAddr(int64_t Value, uint64_t Addr);

  private:
    uint64_t emitStubForFunction(Function *F);
    static void CompilationCallback();
    uint64_t resolveFunctionReference(uint64_t RetAddr);

  };

  JITResolver *TheJITResolver;
  std::map<uint64_t, JITResolver::CallType> JITResolver::LazyCallFlavor;
}

/// addFunctionReference - This method is called when we need to emit the
/// address of a function that has not yet been emitted, so we don't know the
/// address.  Instead, we emit a call to the CompilationCallback method, and
/// keep track of where we are.
///
uint64_t JITResolver::addFunctionReference(uint64_t Address, Function *F) {
  LazyCodeGenMap[Address] = F;
  return (intptr_t)&JITResolver::CompilationCallback;
}

/// deleteFunctionReference - If we are emitting a far call, we already added a
/// reference to the function, but it is now incorrect, since the address to the
/// JIT resolver is too far away to be a simple call instruction. This is used
/// to remove the address from the map.
///
void JITResolver::deleteFunctionReference(uint64_t Address) {
  std::map<uint64_t, Function*>::iterator I = LazyCodeGenMap.find(Address);
  assert(I != LazyCodeGenMap.end() && "Not in map!");
  LazyCodeGenMap.erase(I);  
}

uint64_t JITResolver::resolveFunctionReference(uint64_t RetAddr) {
  std::map<uint64_t, Function*>::iterator I = LazyCodeGenMap.find(RetAddr);
  assert(I != LazyCodeGenMap.end() && "Not in map!");
  Function *F = I->second;
  LazyCodeGenMap.erase(I);
  return MCE.forceCompilationOf(F);
}

uint64_t JITResolver::getLazyResolver(Function *F) {
  std::map<Function*, uint64_t>::iterator I = LazyResolverMap.lower_bound(F);
  if (I != LazyResolverMap.end() && I->first == F) return I->second;
  
//std::cerr << "Getting lazy resolver for : " << ((Value*)F)->getName() << "\n";

  uint64_t Stub = emitStubForFunction(F);
  LazyResolverMap.insert(I, std::make_pair(F, Stub));
  return Stub;
}

uint64_t JITResolver::insertFarJumpAtAddr(int64_t Target, uint64_t Addr) {

  static const unsigned i1 = SparcIntRegClass::i1, i2 = SparcIntRegClass::i2,
    i7 = SparcIntRegClass::i7,
    o6 = SparcIntRegClass::o6, g0 = SparcIntRegClass::g0,
    g1 = SparcIntRegClass::g1, g5 = SparcIntRegClass::g5;

  MachineInstr* BinaryCode[] = {
    //
    // Get address to branch into %g1, using %g5 as a temporary
    //
    // sethi %uhi(Target), %g5     ;; get upper 22 bits of Target into %g5
    BuildMI(V9::SETHI, 2).addSImm(Target >> 42).addReg(g5),
    // or %g5, %ulo(Target), %g5   ;; get 10 lower bits of upper word into %g5
    BuildMI(V9::ORi, 3).addReg(g5).addSImm((Target >> 32) & 0x03ff).addReg(g5),
    // sllx %g5, 32, %g5           ;; shift those 10 bits to the upper word
    BuildMI(V9::SLLXi6, 3).addReg(g5).addSImm(32).addReg(g5),
    // sethi %hi(Target), %g1      ;; extract bits 10-31 into the dest reg
    BuildMI(V9::SETHI, 2).addSImm((Target >> 10) & 0x03fffff).addReg(g1),
    // or %g5, %g1, %g1            ;; get upper word (in %i1) into %g1
    BuildMI(V9::ORr, 3).addReg(g5).addReg(g1).addReg(g1),
    // or %g1, %lo(Target), %g1    ;; get lowest 10 bits of Target into %g1
    BuildMI(V9::ORi, 3).addReg(g1).addSImm(Target & 0x03ff).addReg(g1),
    // jmpl %g1, %g0, %g0          ;; indirect branch on %g1
    BuildMI(V9::JMPLRETr, 3).addReg(g1).addReg(g0).addReg(g0),
    // nop                         ;; delay slot
    BuildMI(V9::NOP, 0)
  };

  for (unsigned i=0, e=sizeof(BinaryCode)/sizeof(BinaryCode[0]); i!=e; ++i) {
    *((unsigned*)(intptr_t)Addr) = getBinaryCodeForInstr(*BinaryCode[i]);
    delete BinaryCode[i];
    Addr += 4;
  }

  return Addr;
}

void JITResolver::CompilationCallback() {
  uint64_t CameFrom = (uint64_t)(intptr_t)__builtin_return_address(0);
  int64_t Target = (int64_t)TheJITResolver->resolveFunctionReference(CameFrom);
  DEBUG(std::cerr << "In callback! Addr=0x" << std::hex << CameFrom << "\n");
#if defined(sparc) || defined(__sparc__) || defined(__sparcv9)
  register int64_t returnAddr;
  __asm__ __volatile__ ("add %%i7, %%g0, %0" : "=r" (returnAddr) : );
  DEBUG(std::cerr << "Read i7 (return addr) = "
                  << std::hex << returnAddr << ", value: "
                  << std::hex << *(unsigned*)returnAddr << "\n");
#endif

  // Rewrite the call target so that we don't fault every time we execute it.
  //

  static const unsigned o6 = SparcIntRegClass::o6;

  // Subtract enough to overwrite up to the 'save' instruction
  // This depends on whether we made a short call (1 instruction) or the
  // farCall (long form: 10 instructions, short form: 7 instructions)
  uint64_t Offset = (LazyCallFlavor[CameFrom] == ShortCall) ? 4 : 28;
  uint64_t CodeBegin = CameFrom - Offset;
  
  // Make sure that what we're about to overwrite is indeed "save"
  MachineInstr *SV = BuildMI(V9::SAVEi, 3).addReg(o6).addSImm(-192).addReg(o6);
  unsigned SaveInst = TheJITResolver->getBinaryCodeForInstr(*SV);
  delete SV;
  unsigned CodeInMem = *(unsigned*)(intptr_t)CodeBegin;
  assert(CodeInMem == SaveInst && "About to overwrite smthg not a save instr!");
  DEBUG(std::cerr << "Emitting a far jump to 0x" << std::hex << Target << "\n");
  TheJITResolver->insertFarJumpAtAddr(Target, CodeBegin);

  // FIXME: if the target function is close enough to fit into the 19bit disp of
  // BA, we should use this version, as its much cheaper to generate.
#if 0  
  uint64_t InstAddr = CodeBegin;
  // ba <target>
  MachineInstr *MI = BuildMI(V9::BA, 1).addSImm(Target);
  *((unsigned*)(intptr_t)InstAddr)=TheJITResolver->getBinaryCodeForInstr(*MI);
  InstAddr += 4;
  delete MI;

  // nop
  MI = BuildMI(V9::NOP, 0);
  *((unsigned*)(intptr_t))=TheJITResolver->getBinaryCodeForInstr(*Nop);
  delete MI;
#endif

  // Change the return address to reexecute the restore, then the jump
  // The return address is really %o7, but will disappear after this function
  // returns, and the register windows are rotated away.
#if defined(sparc) || defined(__sparc__) || defined(__sparcv9)
  __asm__ __volatile__ ("sub %%i7, %0, %%i7" : : "r" (Offset+12));
  DEBUG(std::cerr << "Callback setting return addr to "
                  << std::hex << (CameFrom-Offset-12) << "\n");
#endif
}

/// emitStubForFunction - This method is used by the JIT when it needs to emit
/// the address of a function for a function whose code has not yet been
/// generated.  In order to do this, it generates a stub which jumps to the lazy
/// function compiler, which will eventually get fixed to call the function
/// directly.
///
uint64_t JITResolver::emitStubForFunction(Function *F) {
  MCE.startFunctionStub(*F, 20);

  DEBUG(std::cerr << "Emitting stub at addr: 0x" 
                  << std::hex << MCE.getCurrentPCValue() << "\n");

  unsigned o6 = SparcIntRegClass::o6, g0 = SparcIntRegClass::g0;

  // restore %g0, 0, %g0
  MachineInstr *R = BuildMI(V9::RESTOREi, 3).addMReg(g0).addSImm(0)
                                            .addMReg(g0, MOTy::Def);
  SparcV9.emitWord(SparcV9.getBinaryCodeForInstr(*R));
  delete R;

  // save %sp, -192, %sp
  MachineInstr *SV = BuildMI(V9::SAVEi, 3).addReg(o6).addSImm(-192).addReg(o6);
  SparcV9.emitWord(SparcV9.getBinaryCodeForInstr(*SV));
  delete SV;

  int64_t CurrPC = MCE.getCurrentPCValue();
  int64_t Addr = (int64_t)addFunctionReference(CurrPC, F);
  int64_t CallTarget = (Addr-CurrPC) >> 2;
  //if (CallTarget >= (1 << 29) || CallTarget <= -(1 << 29)) {
  // Since this is a far call, the actual address of the call is shifted
  // by the number of instructions it takes to calculate the exact address
    deleteFunctionReference(CurrPC);
    SparcV9.emitFarCall(Addr, F);
#if 0
  else {
    // call CallTarget              ;; invoke the callback
    MachineInstr *Call = BuildMI(V9::CALL, 1).addSImm(CallTarget);
    SparcV9.emitWord(SparcV9.getBinaryCodeForInstr(*Call));
    delete Call;
  
    // nop                          ;; call delay slot
    MachineInstr *Nop = BuildMI(V9::NOP, 0);
    SparcV9.emitWord(SparcV9.getBinaryCodeForInstr(*Nop));
    delete Nop;

    addCallFlavor(CurrPC, ShortCall);
  }
#endif

  SparcV9.emitWord(0xDEADBEEF); // marker so that we know it's really a stub
  return (intptr_t)MCE.finishFunctionStub(*F)+4; /* 1 instr past the restore */
}


SparcV9CodeEmitter::SparcV9CodeEmitter(TargetMachine &tm,
                                       MachineCodeEmitter &M): TM(tm), MCE(M)
{
  TheJITResolver = new JITResolver(*this, M);
}

SparcV9CodeEmitter::~SparcV9CodeEmitter() {
  delete TheJITResolver;
}

void SparcV9CodeEmitter::emitWord(unsigned Val) {
  // Output the constant in big endian byte order...
  unsigned byteVal;
  for (int i = 3; i >= 0; --i) {
    byteVal = Val >> 8*i;
    MCE.emitByte(byteVal & 255);
  }
}

unsigned 
SparcV9CodeEmitter::getRealRegNum(unsigned fakeReg,
                                         MachineInstr &MI) {
  const TargetRegInfo &RI = TM.getRegInfo();
  unsigned regClass, regType = RI.getRegType(fakeReg);
  // At least map fakeReg into its class
  fakeReg = RI.getClassRegNum(fakeReg, regClass);

  switch (regClass) {
  case UltraSparcRegInfo::IntRegClassID: {
    // Sparc manual, p31
    static const unsigned IntRegMap[] = {
      // "o0", "o1", "o2", "o3", "o4", "o5",       "o7",
      8, 9, 10, 11, 12, 13, 15,
      // "l0", "l1", "l2", "l3", "l4", "l5", "l6", "l7",
      16, 17, 18, 19, 20, 21, 22, 23,
      // "i0", "i1", "i2", "i3", "i4", "i5", "i6", "i7",
      24, 25, 26, 27, 28, 29, 30, 31,
      // "g0", "g1", "g2", "g3", "g4", "g5", "g6", "g7", 
      0, 1, 2, 3, 4, 5, 6, 7,
      // "o6"
      14
    }; 
 
    return IntRegMap[fakeReg];
    break;
  }
  case UltraSparcRegInfo::FloatRegClassID: {
    DEBUG(std::cerr << "FP reg: " << fakeReg << "\n");
    if (regType == UltraSparcRegInfo::FPSingleRegType) {
      // only numbered 0-31, hence can already fit into 5 bits (and 6)
      DEBUG(std::cerr << "FP single reg, returning: " << fakeReg << "\n");
    } else if (regType == UltraSparcRegInfo::FPDoubleRegType) {
      // FIXME: This assumes that we only have 5-bit register fiels!
      // From Sparc Manual, page 40.
      // The bit layout becomes: b[4], b[3], b[2], b[1], b[5]
      fakeReg |= (fakeReg >> 5) & 1;
      fakeReg &= 0x1f;
      DEBUG(std::cerr << "FP double reg, returning: " << fakeReg << "\n");      
    }
    return fakeReg;
  }
  case UltraSparcRegInfo::IntCCRegClassID: {
    /*                                   xcc, icc, ccr */
    static const unsigned IntCCReg[] = {  6,   4,   2 };
    
    assert(fakeReg < sizeof(IntCCReg)/sizeof(IntCCReg[0])
             && "CC register out of bounds for IntCCReg map");      
    DEBUG(std::cerr << "IntCC reg: " << IntCCReg[fakeReg] << "\n");
    return IntCCReg[fakeReg];
  }
  case UltraSparcRegInfo::FloatCCRegClassID: {
    /* These are laid out %fcc0 - %fcc3 => 0 - 3, so are correct */
    DEBUG(std::cerr << "FP CC reg: " << fakeReg << "\n");
    return fakeReg;
  }
  default:
    assert(0 && "Invalid unified register number in getRegType");
    return fakeReg;
  }
}


// WARNING: if the call used the delay slot to do meaningful work, that's not
// being accounted for, and the behavior will be incorrect!!
inline void SparcV9CodeEmitter::emitFarCall(uint64_t Target, Function *F) {
  static const unsigned i1 = SparcIntRegClass::i1, i2 = SparcIntRegClass::i2,
      i7 = SparcIntRegClass::i7, o6 = SparcIntRegClass::o6,
      o7 = SparcIntRegClass::o7, g0 = SparcIntRegClass::g0,
      g1 = SparcIntRegClass::g1, g5 = SparcIntRegClass::g5;

  MachineInstr* BinaryCode[] = {
    //
    // Get address to branch into %g1, using %g5 as a temporary
    //
    // sethi %uhi(Target), %g5   ;; get upper 22 bits of Target into %g5
    BuildMI(V9::SETHI, 2).addSImm(Target >> 42).addReg(g5),
    // or %g5, %ulo(Target), %g5 ;; get 10 lower bits of upper word into %1
    BuildMI(V9::ORi, 3).addReg(g5).addSImm((Target >> 32) & 0x03ff).addReg(g5),
    // sllx %g5, 32, %g5            ;; shift those 10 bits to the upper word
    BuildMI(V9::SLLXi6, 3).addReg(g5).addSImm(32).addReg(g5),
    // sethi %hi(Target), %g1    ;; extract bits 10-31 into the dest reg
    BuildMI(V9::SETHI, 2).addSImm((Target >> 10) & 0x03fffff).addReg(g1),
    // or %g5, %g1, %g1             ;; get upper word (in %g5) into %g1
    BuildMI(V9::ORr, 3).addReg(g5).addReg(g1).addReg(g1),
    // or %g1, %lo(Target), %g1  ;; get lowest 10 bits of Target into %g1
    BuildMI(V9::ORi, 3).addReg(g1).addSImm(Target & 0x03ff).addReg(g1),
    // jmpl %g1, %g0, %o7          ;; indirect call on %g1
    BuildMI(V9::JMPLRETr, 3).addReg(g1).addReg(g0).addReg(o7),
    // nop                         ;; delay slot
    BuildMI(V9::NOP, 0)
  };

  for (unsigned i=0, e=sizeof(BinaryCode)/sizeof(BinaryCode[0]); i!=e; ++i) {
    // This is where we save the return address in the LazyResolverMap!!
    if (i == 6 && F != 0) { // Do this right before the JMPL
      uint64_t CurrPC = MCE.getCurrentPCValue();
      TheJITResolver->addFunctionReference(CurrPC, F);
      // Remember that this is a far call, to subtract appropriate offset later
      TheJITResolver->addCallFlavor(CurrPC, JITResolver::FarCall);
    }

    emitWord(getBinaryCodeForInstr(*BinaryCode[i]));
    delete BinaryCode[i];
  }
}


int64_t SparcV9CodeEmitter::getMachineOpValue(MachineInstr &MI,
                                              MachineOperand &MO) {
  int64_t rv = 0; // Return value; defaults to 0 for unhandled cases
                  // or things that get fixed up later by the JIT.

  if (MO.isVirtualRegister()) {
    std::cerr << "ERROR: virtual register found in machine code.\n";
    abort();
  } else if (MO.isPCRelativeDisp()) {
    DEBUG(std::cerr << "PCRelativeDisp: ");
    Value *V = MO.getVRegValue();
    if (BasicBlock *BB = dyn_cast<BasicBlock>(V)) {
      DEBUG(std::cerr << "Saving reference to BB (VReg)\n");
      unsigned* CurrPC = (unsigned*)(intptr_t)MCE.getCurrentPCValue();
      BBRefs.push_back(std::make_pair(BB, std::make_pair(CurrPC, &MI)));
    } else if (const Constant *C = dyn_cast<Constant>(V)) {
      if (ConstantMap.find(C) != ConstantMap.end()) {
        rv = (int64_t)MCE.getConstantPoolEntryAddress(ConstantMap[C]);
        DEBUG(std::cerr << "const: 0x" << std::hex << rv << "\n");
      } else {
        std::cerr << "ERROR: constant not in map:" << MO << "\n";
        abort();
      }
    } else if (GlobalValue *GV = dyn_cast<GlobalValue>(V)) {
      // same as MO.isGlobalAddress()
      DEBUG(std::cerr << "GlobalValue: ");
      // external function calls, etc.?
      if (Function *F = dyn_cast<Function>(GV)) {
        DEBUG(std::cerr << "Function: ");
        if (F->isExternal()) {
          // Sparc backend broken: this MO should be `ExternalSymbol'
          rv = (int64_t)MCE.getGlobalValueAddress(F->getName());
        } else {
          rv = (int64_t)MCE.getGlobalValueAddress(F);
        }
        if (rv == 0) {
          DEBUG(std::cerr << "not yet generated\n");
          // Function has not yet been code generated!
          TheJITResolver->addFunctionReference(MCE.getCurrentPCValue(), F);
          // Delayed resolution...
          rv = TheJITResolver->getLazyResolver(F);
        } else {
          DEBUG(std::cerr << "already generated: 0x" << std::hex << rv << "\n");
        }
      } else {
        rv = (int64_t)MCE.getGlobalValueAddress(GV);
        if (rv == 0) {
          if (Constant *C = ConstantPointerRef::get(GV)) {
            if (ConstantMap.find(C) != ConstantMap.end()) {
              rv = MCE.getConstantPoolEntryAddress(ConstantMap[C]);
            } else {
              std::cerr << "Constant: 0x" << std::hex << (intptr_t)C
                        << ", " << *V << " not found in ConstantMap!\n";
              abort();
            }
          }
        }
        DEBUG(std::cerr << "Global addr: 0x" << std::hex << rv << "\n");
      }
      // The real target of the call is Addr = PC + (rv * 4)
      // So undo that: give the instruction (Addr - PC) / 4
      if (MI.getOpcode() == V9::CALL) {
        int64_t CurrPC = MCE.getCurrentPCValue();
        DEBUG(std::cerr << "rv addr: 0x" << std::hex << rv << "\n"
                        << "curr PC: 0x" << std::hex << CurrPC << "\n");
        int64_t CallInstTarget = (rv - CurrPC) >> 2;
        if (CallInstTarget >= (1<<29) || CallInstTarget <= -(1<<29)) {
          DEBUG(std::cerr << "Making far call!\n");
          // addresss is out of bounds for the 30-bit call,
          // make an indirect jump-and-link
          emitFarCall(rv);
          // this invalidates the instruction so that the call with an incorrect
          // address will not be emitted
          rv = 0; 
        } else {
          // The call fits into 30 bits, so just return the corrected address
          rv = CallInstTarget;
        }
        DEBUG(std::cerr << "returning addr: 0x" << rv << "\n");
      }
    } else {
      std::cerr << "ERROR: PC relative disp unhandled:" << MO << "\n";
      abort();
    }
  } else if (MO.isPhysicalRegister() ||
             MO.getType() == MachineOperand::MO_CCRegister)
  {
    // This is necessary because the Sparc backend doesn't actually lay out
    // registers in the real fashion -- it skips those that it chooses not to
    // allocate, i.e. those that are the FP, SP, etc.
    unsigned fakeReg = MO.getAllocatedRegNum();
    unsigned realRegByClass = getRealRegNum(fakeReg, MI);
    DEBUG(std::cerr << MO << ": Reg[" << std::dec << fakeReg << "] => "
                    << realRegByClass << " (LLC: " 
                    << TM.getRegInfo().getUnifiedRegName(fakeReg) << ")\n");
    rv = realRegByClass;
  } else if (MO.isImmediate()) {
    rv = MO.getImmedValue();
    DEBUG(std::cerr << "immed: " << rv << "\n");
  } else if (MO.isGlobalAddress()) {
    DEBUG(std::cerr << "GlobalAddress: not PC-relative\n");
    rv = (int64_t)
      (intptr_t)getGlobalAddress(cast<GlobalValue>(MO.getVRegValue()),
                                 MI, MO.isPCRelative());
  } else if (MO.isMachineBasicBlock()) {
    // Duplicate code of the above case for VirtualRegister, BasicBlock... 
    // It should really hit this case, but Sparc backend uses VRegs instead
    DEBUG(std::cerr << "Saving reference to MBB\n");
    const BasicBlock *BB = MO.getMachineBasicBlock()->getBasicBlock();
    unsigned* CurrPC = (unsigned*)(intptr_t)MCE.getCurrentPCValue();
    BBRefs.push_back(std::make_pair(BB, std::make_pair(CurrPC, &MI)));
  } else if (MO.isExternalSymbol()) {
    // Sparc backend doesn't generate this (yet...)
    std::cerr << "ERROR: External symbol unhandled: " << MO << "\n";
    abort();
  } else if (MO.isFrameIndex()) {
    // Sparc backend doesn't generate this (yet...)
    int FrameIndex = MO.getFrameIndex();
    std::cerr << "ERROR: Frame index unhandled.\n";
    abort();
  } else if (MO.isConstantPoolIndex()) {
    // Sparc backend doesn't generate this (yet...)
    std::cerr << "ERROR: Constant Pool index unhandled.\n";
    abort();
  } else {
    std::cerr << "ERROR: Unknown type of MachineOperand: " << MO << "\n";
    abort();
  }

  // Finally, deal with the various bitfield-extracting functions that
  // are used in SPARC assembly. (Some of these make no sense in combination
  // with some of the above; we'll trust that the instruction selector
  // will not produce nonsense, and not check for valid combinations here.)
  if (MO.opLoBits32()) {          // %lo(val) == %lo() in Sparc ABI doc
    return rv & 0x03ff;
  } else if (MO.opHiBits32()) {   // %lm(val) == %hi() in Sparc ABI doc
    return (rv >> 10) & 0x03fffff;
  } else if (MO.opLoBits64()) {   // %hm(val) == %ulo() in Sparc ABI doc
    return (rv >> 32) & 0x03ff;
  } else if (MO.opHiBits64()) {   // %hh(val) == %uhi() in Sparc ABI doc
    return rv >> 42;
  } else {                        // (unadorned) val
    return rv;
  }
}

unsigned SparcV9CodeEmitter::getValueBit(int64_t Val, unsigned bit) {
  Val >>= bit;
  return (Val & 1);
}

bool SparcV9CodeEmitter::runOnMachineFunction(MachineFunction &MF) {
  MCE.startFunction(MF);
  DEBUG(std::cerr << "Starting function " << MF.getFunction()->getName()
            << ", address: " << "0x" << std::hex 
            << (long)MCE.getCurrentPCValue() << "\n");

  // The Sparc backend does not use MachineConstantPool;
  // instead, it has its own constant pool implementation.
  // We create a new MachineConstantPool here to be compatible with the emitter.
  MachineConstantPool MCP;
  const hash_set<const Constant*> &pool = MF.getInfo()->getConstantPoolValues();
  for (hash_set<const Constant*>::const_iterator I = pool.begin(),
         E = pool.end();  I != E; ++I)
  {
    Constant *C = (Constant*)*I;
    unsigned idx = MCP.getConstantPoolIndex(C);
    DEBUG(std::cerr << "Constant[" << idx << "] = 0x" << (intptr_t)C << "\n");
    ConstantMap[C] = idx;
  }  
  MCE.emitConstantPool(&MCP);

  for (MachineFunction::iterator I = MF.begin(), E = MF.end(); I != E; ++I)
    emitBasicBlock(*I);
  MCE.finishFunction(MF);

  DEBUG(std::cerr << "Finishing fn " << MF.getFunction()->getName() << "\n");
  ConstantMap.clear();

  // Resolve branches to BasicBlocks for the entire function
  for (unsigned i = 0, e = BBRefs.size(); i != e; ++i) {
    long Location = BBLocations[BBRefs[i].first];
    unsigned *Ref = BBRefs[i].second.first;
    MachineInstr *MI = BBRefs[i].second.second;
    DEBUG(std::cerr << "Fixup @ " << std::hex << Ref << " to 0x" << Location
                    << " in instr: " << std::dec << *MI);
    for (unsigned ii = 0, ee = MI->getNumOperands(); ii != ee; ++ii) {
      MachineOperand &op = MI->getOperand(ii);
      if (op.isPCRelativeDisp()) {
        // the instruction's branch target is made such that it branches to
        // PC + (branchTarget * 4), so undo that arithmetic here:
        // Location is the target of the branch
        // Ref is the location of the instruction, and hence the PC
        int64_t branchTarget = (Location - (long)Ref) >> 2;
        // Save the flags.
        bool loBits32=false, hiBits32=false, loBits64=false, hiBits64=false;   
        if (op.opLoBits32()) { loBits32=true; }
        if (op.opHiBits32()) { hiBits32=true; }
        if (op.opLoBits64()) { loBits64=true; }
        if (op.opHiBits64()) { hiBits64=true; }
        MI->SetMachineOperandConst(ii, MachineOperand::MO_SignExtendedImmed,
                                   branchTarget);
        if (loBits32) { MI->setOperandLo32(ii); }
        else if (hiBits32) { MI->setOperandHi32(ii); }
        else if (loBits64) { MI->setOperandLo64(ii); }
        else if (hiBits64) { MI->setOperandHi64(ii); }
        DEBUG(std::cerr << "Rewrote BB ref: ");
        unsigned fixedInstr = SparcV9CodeEmitter::getBinaryCodeForInstr(*MI);
        *Ref = fixedInstr;
        break;
      }
    }
  }
  BBRefs.clear();
  BBLocations.clear();

  return false;
}

void SparcV9CodeEmitter::emitBasicBlock(MachineBasicBlock &MBB) {
  currBB = MBB.getBasicBlock();
  BBLocations[currBB] = MCE.getCurrentPCValue();
  for (MachineBasicBlock::iterator I = MBB.begin(), E = MBB.end(); I != E; ++I){
    unsigned binCode = getBinaryCodeForInstr(**I);
    if (binCode == (1 << 30)) {
      // this is an invalid call: the addr is out of bounds. that means a code
      // sequence has already been emitted, and this is a no-op
      DEBUG(std::cerr << "Call supressed: already emitted far call.\n");
    } else {
      emitWord(binCode);
    }
  }
}

void* SparcV9CodeEmitter::getGlobalAddress(GlobalValue *V, MachineInstr &MI,
                                           bool isPCRelative)
{
  if (isPCRelative) { // must be a call, this is a major hack!
    // Try looking up the function to see if it is already compiled!
    if (void *Addr = (void*)(intptr_t)MCE.getGlobalValueAddress(V)) {
      intptr_t CurByte = MCE.getCurrentPCValue();
      // The real target of the call is Addr = PC + (target * 4)
      // CurByte is the PC, Addr we just received
      return (void*) (((long)Addr - (long)CurByte) >> 2);
    } else {
      if (Function *F = dyn_cast<Function>(V)) {
        // Function has not yet been code generated!
        TheJITResolver->addFunctionReference(MCE.getCurrentPCValue(),
                                             cast<Function>(V));
        // Delayed resolution...
        return 
          (void*)(intptr_t)TheJITResolver->getLazyResolver(cast<Function>(V));

      } else if (Constant *C = ConstantPointerRef::get(V)) {
        if (ConstantMap.find(C) != ConstantMap.end()) {
          return (void*)
            (intptr_t)MCE.getConstantPoolEntryAddress(ConstantMap[C]);
        } else {
          std::cerr << "Constant: 0x" << std::hex << &*C << std::dec
                    << ", " << *V << " not found in ConstantMap!\n";
          abort();
        }
      } else {
        std::cerr << "Unhandled global: " << *V << "\n";
        abort();
      }
    }
  } else {
    return (void*)(intptr_t)MCE.getGlobalValueAddress(V);
  }
}


#include "SparcV9CodeEmitter.inc"
