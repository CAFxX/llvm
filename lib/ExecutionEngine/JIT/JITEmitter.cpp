//===-- JITEmitter.cpp - Write machine code to executable memory ----------===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file defines a MachineCodeEmitter object that is used by the JIT to
// write machine code to memory and remember where relocatable values are.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "jit"
#include "JIT.h"
#include "llvm/Constant.h"
#include "llvm/Module.h"
#include "llvm/CodeGen/MachineCodeEmitter.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineRelocation.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Target/TargetJITInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/System/Memory.h"
using namespace llvm;

namespace {
  Statistic<> NumBytes("jit", "Number of bytes of machine code compiled");
  JIT *TheJIT = 0;
}


//===----------------------------------------------------------------------===//
// JITMemoryManager code.
//
namespace {
  /// JITMemoryManager - Manage memory for the JIT code generation in a logical,
  /// sane way.  This splits a large block of MAP_NORESERVE'd memory into two
  /// sections, one for function stubs, one for the functions themselves.  We
  /// have to do this because we may need to emit a function stub while in the
  /// middle of emitting a function, and we don't know how large the function we
  /// are emitting is.  This never bothers to release the memory, because when
  /// we are ready to destroy the JIT, the program exits.
  class JITMemoryManager {
    sys::MemoryBlock  MemBlock;  // Virtual memory block allocated RWX
    unsigned char *MemBase;      // Base of block of memory, start of stub mem
    unsigned char *FunctionBase; // Start of the function body area
    unsigned char *CurStubPtr, *CurFunctionPtr;
  public:
    JITMemoryManager();
    ~JITMemoryManager();
    
    inline unsigned char *allocateStub(unsigned StubSize);
    inline unsigned char *startFunctionBody();
    inline void endFunctionBody(unsigned char *FunctionEnd);    
  };
}

JITMemoryManager::JITMemoryManager() {
  // Allocate a 16M block of memory...
  MemBlock = sys::Memory::AllocateRWX((16 << 20));
  MemBase = reinterpret_cast<unsigned char*>(MemBlock.base());
  FunctionBase = MemBase + 512*1024; // Use 512k for stubs

  // Allocate stubs backwards from the function base, allocate functions forward
  // from the function base.
  CurStubPtr = CurFunctionPtr = FunctionBase;
}

JITMemoryManager::~JITMemoryManager() {
  sys::Memory::ReleaseRWX(MemBlock);
}

unsigned char *JITMemoryManager::allocateStub(unsigned StubSize) {
  CurStubPtr -= StubSize;
  if (CurStubPtr < MemBase) {
    std::cerr << "JIT ran out of memory for function stubs!\n";
    abort();
  }
  return CurStubPtr;
}

unsigned char *JITMemoryManager::startFunctionBody() {
  // Round up to an even multiple of 8 bytes, this should eventually be target
  // specific.
  return (unsigned char*)(((intptr_t)CurFunctionPtr + 7) & ~7);
}

void JITMemoryManager::endFunctionBody(unsigned char *FunctionEnd) {
  assert(FunctionEnd > CurFunctionPtr);
  CurFunctionPtr = FunctionEnd;
}

//===----------------------------------------------------------------------===//
// JIT lazy compilation code.
//
namespace {
  /// JITResolver - Keep track of, and resolve, call sites for functions that
  /// have not yet been compiled.
  class JITResolver {
    /// MCE - The MachineCodeEmitter to use to emit stubs with.
    MachineCodeEmitter &MCE;

    /// LazyResolverFn - The target lazy resolver function that we actually
    /// rewrite instructions to use.
    TargetJITInfo::LazyResolverFn LazyResolverFn;

    // FunctionToStubMap - Keep track of the stub created for a particular
    // function so that we can reuse them if necessary.
    std::map<Function*, void*> FunctionToStubMap;

    // StubToFunctionMap - Keep track of the function that each stub corresponds
    // to.
    std::map<void*, Function*> StubToFunctionMap;

  public:
    JITResolver(MachineCodeEmitter &mce) : MCE(mce) {
      LazyResolverFn =
        TheJIT->getJITInfo().getLazyResolverFunction(JITCompilerFn);
    }

    /// getFunctionStub - This returns a pointer to a function stub, creating
    /// one on demand as needed.
    void *getFunctionStub(Function *F);

    /// AddCallbackAtLocation - If the target is capable of rewriting an
    /// instruction without the use of a stub, record the location of the use so
    /// we know which function is being used at the location.
    void *AddCallbackAtLocation(Function *F, void *Location) {
      /// Get the target-specific JIT resolver function.
      StubToFunctionMap[Location] = F;
      return (void*)LazyResolverFn;
    }

    /// JITCompilerFn - This function is called to resolve a stub to a compiled
    /// address.  If the LLVM Function corresponding to the stub has not yet
    /// been compiled, this function compiles it first.
    static void *JITCompilerFn(void *Stub);
  };
}

/// getJITResolver - This function returns the one instance of the JIT resolver.
///
static JITResolver &getJITResolver(MachineCodeEmitter *MCE = 0) {
  static JITResolver TheJITResolver(*MCE);
  return TheJITResolver;
}

/// getFunctionStub - This returns a pointer to a function stub, creating
/// one on demand as needed.
void *JITResolver::getFunctionStub(Function *F) {
  // If we already have a stub for this function, recycle it.
  void *&Stub = FunctionToStubMap[F];
  if (Stub) return Stub;

  // Call the lazy resolver function unless we already KNOW it is an external
  // function, in which case we just skip the lazy resolution step.
  void *Actual = (void*)LazyResolverFn;
  if (F->hasExternalLinkage())
    Actual = TheJIT->getPointerToFunction(F);
    
  // Otherwise, codegen a new stub.  For now, the stub will call the lazy
  // resolver function.
  Stub = TheJIT->getJITInfo().emitFunctionStub(Actual, MCE);

  if (F->hasExternalLinkage()) {
    // If we are getting the stub for an external function, we really want the
    // address of the stub in the GlobalAddressMap for the JIT, not the address
    // of the external function.
    TheJIT->updateGlobalMapping(F, Stub);
  }

  DEBUG(std::cerr << "JIT: Stub emitted at [" << Stub << "] for function '"
                  << F->getName() << "'\n");

  // Finally, keep track of the stub-to-Function mapping so that the
  // JITCompilerFn knows which function to compile!
  StubToFunctionMap[Stub] = F;
  return Stub;
}

/// JITCompilerFn - This function is called when a lazy compilation stub has
/// been entered.  It looks up which function this stub corresponds to, compiles
/// it if necessary, then returns the resultant function pointer.
void *JITResolver::JITCompilerFn(void *Stub) {
  JITResolver &JR = getJITResolver();
  
  // The address given to us for the stub may not be exactly right, it might be
  // a little bit after the stub.  As such, use upper_bound to find it.
  std::map<void*, Function*>::iterator I =
    JR.StubToFunctionMap.upper_bound(Stub);
  assert(I != JR.StubToFunctionMap.begin() && "This is not a known stub!");
  Function *F = (--I)->second;

  // The target function will rewrite the stub so that the compilation callback
  // function is no longer called from this stub.
  JR.StubToFunctionMap.erase(I);

  DEBUG(std::cerr << "JIT: Lazily resolving function '" << F->getName()
                  << "' In stub ptr = " << Stub << " actual ptr = "
                  << I->first << "\n");

  void *Result = TheJIT->getPointerToFunction(F);

  // We don't need to reuse this stub in the future, as F is now compiled.
  JR.FunctionToStubMap.erase(F);

  // FIXME: We could rewrite all references to this stub if we knew them.
  return Result;
}


// getPointerToFunctionOrStub - If the specified function has been
// code-gen'd, return a pointer to the function.  If not, compile it, or use
// a stub to implement lazy compilation if available.
//
void *JIT::getPointerToFunctionOrStub(Function *F) {
  // If we have already code generated the function, just return the address.
  if (void *Addr = getPointerToGlobalIfAvailable(F))
    return Addr;

  // Get a stub if the target supports it
  return getJITResolver(MCE).getFunctionStub(F);
}



//===----------------------------------------------------------------------===//
// JITEmitter code.
//
namespace {
  /// JITEmitter - The JIT implementation of the MachineCodeEmitter, which is
  /// used to output functions to memory for execution.
  class JITEmitter : public MachineCodeEmitter {
    JITMemoryManager MemMgr;

    // CurBlock - The start of the current block of memory.  CurByte - The
    // current byte being emitted to.
    unsigned char *CurBlock, *CurByte;

    // When outputting a function stub in the context of some other function, we
    // save CurBlock and CurByte here.
    unsigned char *SavedCurBlock, *SavedCurByte;

    // ConstantPoolAddresses - Contains the location for each entry in the
    // constant pool.
    std::vector<void*> ConstantPoolAddresses;

    /// Relocations - These are the relocations that the function needs, as
    /// emitted.
    std::vector<MachineRelocation> Relocations;
  public:
    JITEmitter(JIT &jit) { TheJIT = &jit; }

    virtual void startFunction(MachineFunction &F);
    virtual void finishFunction(MachineFunction &F);
    virtual void emitConstantPool(MachineConstantPool *MCP);
    virtual void startFunctionStub(unsigned StubSize);
    virtual void* finishFunctionStub(const Function *F);
    virtual void emitByte(unsigned char B);
    virtual void emitWord(unsigned W);
    virtual void emitWordAt(unsigned W, unsigned *Ptr);

    virtual void addRelocation(const MachineRelocation &MR) {
      Relocations.push_back(MR);
    }

    virtual uint64_t getCurrentPCValue();
    virtual uint64_t getCurrentPCOffset();
    virtual uint64_t getConstantPoolEntryAddress(unsigned Entry);

  private:
    void *getPointerToGlobal(GlobalValue *GV, void *Reference, bool NoNeedStub);
  };
}

MachineCodeEmitter *JIT::createEmitter(JIT &jit) {
  return new JITEmitter(jit);
}

void *JITEmitter::getPointerToGlobal(GlobalValue *V, void *Reference,
                                     bool DoesntNeedStub) {
  if (GlobalVariable *GV = dyn_cast<GlobalVariable>(V)) {
    /// FIXME: If we straightened things out, this could actually emit the
    /// global immediately instead of queuing it for codegen later!
    return TheJIT->getOrEmitGlobalVariable(GV);
  }

  // If we have already compiled the function, return a pointer to its body.
  Function *F = cast<Function>(V);
  void *ResultPtr = TheJIT->getPointerToGlobalIfAvailable(F);
  if (ResultPtr) return ResultPtr;

  if (F->hasExternalLinkage() && F->isExternal()) {
    // If this is an external function pointer, we can force the JIT to
    // 'compile' it, which really just adds it to the map.
    if (DoesntNeedStub)
      return TheJIT->getPointerToFunction(F);

    return getJITResolver(this).getFunctionStub(F);
  }

  // Okay, the function has not been compiled yet, if the target callback
  // mechanism is capable of rewriting the instruction directly, prefer to do
  // that instead of emitting a stub.
  if (DoesntNeedStub)
    return getJITResolver(this).AddCallbackAtLocation(F, Reference);

  // Otherwise, we have to emit a lazy resolving stub.
  return getJITResolver(this).getFunctionStub(F);
}

void JITEmitter::startFunction(MachineFunction &F) {
  CurByte = CurBlock = MemMgr.startFunctionBody();
  TheJIT->addGlobalMapping(F.getFunction(), CurBlock);
}

void JITEmitter::finishFunction(MachineFunction &F) {
  MemMgr.endFunctionBody(CurByte);
  ConstantPoolAddresses.clear();
  NumBytes += CurByte-CurBlock;

  if (!Relocations.empty()) {
    // Resolve the relocations to concrete pointers.
    for (unsigned i = 0, e = Relocations.size(); i != e; ++i) {
      MachineRelocation &MR = Relocations[i];
      void *ResultPtr;
      if (MR.isString())
        ResultPtr = TheJIT->getPointerToNamedFunction(MR.getString());
      else
        ResultPtr = getPointerToGlobal(MR.getGlobalValue(),
                                       CurBlock+MR.getMachineCodeOffset(),
                                       MR.doesntNeedFunctionStub());
      MR.setResultPointer(ResultPtr);
    }

    TheJIT->getJITInfo().relocate(CurBlock, &Relocations[0],
                                  Relocations.size());
  }

  DEBUG(std::cerr << "JIT: Finished CodeGen of [" << (void*)CurBlock
                  << "] Function: " << F.getFunction()->getName()
                  << ": " << CurByte-CurBlock << " bytes of text, "
                  << Relocations.size() << " relocations\n");
  Relocations.clear();
}

void JITEmitter::emitConstantPool(MachineConstantPool *MCP) {
  const std::vector<Constant*> &Constants = MCP->getConstants();
  if (Constants.empty()) return;

  std::vector<unsigned> ConstantOffset;
  ConstantOffset.reserve(Constants.size());

  // Calculate how much space we will need for all the constants, and the offset
  // each one will live in.
  unsigned TotalSize = 0;
  for (unsigned i = 0, e = Constants.size(); i != e; ++i) {
    const Type *Ty = Constants[i]->getType();
    unsigned Size      = TheJIT->getTargetData().getTypeSize(Ty);
    unsigned Alignment = TheJIT->getTargetData().getTypeAlignment(Ty);
    // Make sure to take into account the alignment requirements of the type.
    TotalSize = (TotalSize + Alignment-1) & ~(Alignment-1);

    // Remember the offset this element lives at.
    ConstantOffset.push_back(TotalSize);
    TotalSize += Size;   // Reserve space for the constant.
  }

  // Now that we know how much memory to allocate, do so.
  char *Pool = new char[TotalSize];

  // Actually output all of the constants, and remember their addresses.
  for (unsigned i = 0, e = Constants.size(); i != e; ++i) {
    void *Addr = Pool + ConstantOffset[i];
    TheJIT->InitializeMemory(Constants[i], Addr);
    ConstantPoolAddresses.push_back(Addr);
  }
}

void JITEmitter::startFunctionStub(unsigned StubSize) {
  SavedCurBlock = CurBlock;  SavedCurByte = CurByte;
  CurByte = CurBlock = MemMgr.allocateStub(StubSize);
}

void *JITEmitter::finishFunctionStub(const Function *F) {
  NumBytes += CurByte-CurBlock;
  std::swap(CurBlock, SavedCurBlock);
  CurByte = SavedCurByte;
  return SavedCurBlock;
}

void JITEmitter::emitByte(unsigned char B) {
  *CurByte++ = B;   // Write the byte to memory
}

void JITEmitter::emitWord(unsigned W) {
  // This won't work if the endianness of the host and target don't agree!  (For
  // a JIT this can't happen though.  :)
  *(unsigned*)CurByte = W;
  CurByte += sizeof(unsigned);
}

void JITEmitter::emitWordAt(unsigned W, unsigned *Ptr) {
  *Ptr = W;
}

// getConstantPoolEntryAddress - Return the address of the 'ConstantNum' entry
// in the constant pool that was last emitted with the 'emitConstantPool'
// method.
//
uint64_t JITEmitter::getConstantPoolEntryAddress(unsigned ConstantNum) {
  assert(ConstantNum < ConstantPoolAddresses.size() &&
	 "Invalid ConstantPoolIndex!");
  return (intptr_t)ConstantPoolAddresses[ConstantNum];
}

// getCurrentPCValue - This returns the address that the next emitted byte
// will be output to.
//
uint64_t JITEmitter::getCurrentPCValue() {
  return (intptr_t)CurByte;
}

uint64_t JITEmitter::getCurrentPCOffset() {
  return (intptr_t)CurByte-(intptr_t)CurBlock;
}

// getPointerToNamedFunction - This function is used as a global wrapper to
// JIT::getPointerToNamedFunction for the purpose of resolving symbols when
// bugpoint is debugging the JIT. In that scenario, we are loading an .so and
// need to resolve function(s) that are being mis-codegenerated, so we need to
// resolve their addresses at runtime, and this is the way to do it.
extern "C" {
  void *getPointerToNamedFunction(const char *Name) {
    Module &M = TheJIT->getModule();
    if (Function *F = M.getNamedFunction(Name))
      return TheJIT->getPointerToFunction(F);
    return TheJIT->getPointerToNamedFunction(Name);
  }
}
