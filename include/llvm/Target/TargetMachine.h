//===-- llvm/Target/TargetMachine.h - General Target Information -*- C++ -*-==//
//
// This file describes the general parts of a Target machine.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TARGET_TARGETMACHINE_H
#define LLVM_TARGET_TARGETMACHINE_H

#include "llvm/Target/TargetData.h"
#include "Support/NonCopyable.h"

class MachineInstrInfo;
class MachineInstrDescriptor;
class MachineSchedInfo;
class MachineRegInfo;
class TargetFrameInfo;
class TargetCacheInfo;
class TargetOptInfo;
class MachineCodeEmitter;
class MRegisterInfo;
class PassManager;
class Pass;

//===----------------------------------------------------------------------===//
///
/// TargetMachine - Primary interface to the complete machine description for
/// the target machine.  All target-specific information should be accessible
/// through this interface.
/// 
class TargetMachine : public NonCopyableV {
  const std::string Name;
  const TargetData DataLayout;		// Calculates type size & alignment
  
protected:
  TargetMachine(const std::string &name, // Can only create subclasses...
		bool LittleEndian = false,
                unsigned char SubWordSize = 1, unsigned char IntRegSize = 8,
		unsigned char PtrSize = 8, unsigned char PtrAl = 8,
		unsigned char DoubleAl = 8, unsigned char FloatAl = 4,
		unsigned char LongAl = 8, unsigned char IntAl = 4,
		unsigned char ShortAl = 2, unsigned char ByteAl = 1)
    : Name(name), DataLayout(name, LittleEndian, SubWordSize, IntRegSize,
			     PtrSize, PtrAl, DoubleAl, FloatAl, LongAl,
                             IntAl, ShortAl, ByteAl) {}
public:
  virtual ~TargetMachine() {}

  const std::string &getName() const { return Name; }
  
  // Interfaces to the major aspects of target machine information:
  // -- Instruction opcode and operand information
  // -- Pipelines and scheduling information
  // -- Register information
  // -- Stack frame information
  // -- Cache hierarchy information
  // -- Machine-level optimization information (peephole only)
  // 
  virtual const MachineInstrInfo&       getInstrInfo() const = 0;
  virtual const MachineSchedInfo&       getSchedInfo() const = 0;
  virtual const MachineRegInfo&	        getRegInfo()   const = 0;
  virtual const TargetFrameInfo&        getFrameInfo() const = 0;
  virtual const TargetCacheInfo&        getCacheInfo() const = 0;
  virtual const TargetOptInfo&          getOptInfo()   const = 0;
  const TargetData &getTargetData() const { return DataLayout; }

  /// getRegisterInfo - If register information is available, return it.  If
  /// not, return null.  This is kept separate from RegInfo until RegInfo has
  /// details of graph coloring register allocation removed from it.
  ///
  virtual const MRegisterInfo*          getRegisterInfo() const { return 0; }

  // Data storage information
  // 
  virtual unsigned findOptimalStorageSize(const Type* ty) const;
  
  /// addPassesToJITCompile - Add passes to the specified pass manager to
  /// implement a fast dynamic compiler for this target.  Return true if this is
  /// not supported for this target.
  ///
  virtual bool addPassesToJITCompile(PassManager &PM) { return true; }

  /// addPassesToEmitAssembly - Add passes to the specified pass manager to get
  /// assembly langage code emitted.  Typically this will involve several steps
  /// of code generation.  This method should return true if assembly emission
  /// is not supported.
  ///
  virtual bool addPassesToEmitAssembly(PassManager &PM, std::ostream &Out) {
    return true;
  }

  /// addPassesToEmitMachineCode - Add passes to the specified pass manager to
  /// get machine code emitted.  This uses a MAchineCodeEmitter object to handle
  /// actually outputting the machine code and resolving things like the address
  /// of functions.  This method should returns true if machine code emission is
  /// not supported.
  ///
  virtual bool addPassesToEmitMachineCode(PassManager &PM,
                                          MachineCodeEmitter &MCE) {
    return true;
  }
};

#endif
