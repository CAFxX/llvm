//===- ReaderWrappers.cpp - Parse bytecode from file or buffer  -----------===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file implements loading and parsing a bytecode file and parsing a
// bytecode module from a given buffer.
//
//===----------------------------------------------------------------------===//

#include "llvm/Bytecode/Analyzer.h"
#include "llvm/Bytecode/Reader.h"
#include "Reader.h"
#include "llvm/Module.h"
#include "llvm/Instructions.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Config/unistd.h"
#include <cerrno>
using namespace llvm;

//===----------------------------------------------------------------------===//
// BytecodeFileReader - Read from an mmap'able file descriptor.
//

namespace {
  /// BytecodeFileReader - parses a bytecode file from a file
  ///
  class BytecodeFileReader : public BytecodeReader {
  private:
    unsigned char *Buffer;
    unsigned Length;

    BytecodeFileReader(const BytecodeFileReader&); // Do not implement
    void operator=(const BytecodeFileReader &BFR); // Do not implement

  public:
    BytecodeFileReader(const std::string &Filename, llvm::BytecodeHandler* H=0);
    ~BytecodeFileReader();
  };
}

static std::string ErrnoMessage (int savedErrNum, std::string descr) {
   return ::strerror(savedErrNum) + std::string(", while trying to ") + descr;
}

BytecodeFileReader::BytecodeFileReader(const std::string &Filename,
                                       llvm::BytecodeHandler* H ) 
  : BytecodeReader(H)
{
  Buffer = (unsigned char*)ReadFileIntoAddressSpace(Filename, Length);
  if (Buffer == 0)
    throw "Error reading file '" + Filename + "'.";

  try {
    // Parse the bytecode we mmapped in
    ParseBytecode(Buffer, Length, Filename);
  } catch (...) {
    UnmapFileFromAddressSpace(Buffer, Length);
    throw;
  }
}

BytecodeFileReader::~BytecodeFileReader() {
  // Unmmap the bytecode...
  UnmapFileFromAddressSpace(Buffer, Length);
}

//===----------------------------------------------------------------------===//
// BytecodeBufferReader - Read from a memory buffer
//

namespace {
  /// BytecodeBufferReader - parses a bytecode file from a buffer
  ///
  class BytecodeBufferReader : public BytecodeReader {
  private:
    const unsigned char *Buffer;
    bool MustDelete;

    BytecodeBufferReader(const BytecodeBufferReader&); // Do not implement
    void operator=(const BytecodeBufferReader &BFR);   // Do not implement

  public:
    BytecodeBufferReader(const unsigned char *Buf, unsigned Length,
                         const std::string &ModuleID,
                         llvm::BytecodeHandler* Handler = 0);
    ~BytecodeBufferReader();

  };
}

BytecodeBufferReader::BytecodeBufferReader(const unsigned char *Buf,
                                           unsigned Length,
                                           const std::string &ModuleID,
                                           llvm::BytecodeHandler* H )
  : BytecodeReader(H)
{
  // If not aligned, allocate a new buffer to hold the bytecode...
  const unsigned char *ParseBegin = 0;
  if (reinterpret_cast<uint64_t>(Buf) & 3) {
    Buffer = new unsigned char[Length+4];
    unsigned Offset = 4 - ((intptr_t)Buffer & 3);   // Make sure it's aligned
    ParseBegin = Buffer + Offset;
    memcpy((unsigned char*)ParseBegin, Buf, Length);    // Copy it over
    MustDelete = true;
  } else {
    // If we don't need to copy it over, just use the caller's copy
    ParseBegin = Buffer = Buf;
    MustDelete = false;
  }
  try {
    ParseBytecode(ParseBegin, Length, ModuleID);
  } catch (...) {
    if (MustDelete) delete [] Buffer;
    throw;
  }
}

BytecodeBufferReader::~BytecodeBufferReader() {
  if (MustDelete) delete [] Buffer;
}

//===----------------------------------------------------------------------===//
//  BytecodeStdinReader - Read bytecode from Standard Input
//

namespace {
  /// BytecodeStdinReader - parses a bytecode file from stdin
  /// 
  class BytecodeStdinReader : public BytecodeReader {
  private:
    std::vector<unsigned char> FileData;
    unsigned char *FileBuf;

    BytecodeStdinReader(const BytecodeStdinReader&); // Do not implement
    void operator=(const BytecodeStdinReader &BFR);  // Do not implement

  public:
    BytecodeStdinReader( llvm::BytecodeHandler* H = 0 );
  };
}

BytecodeStdinReader::BytecodeStdinReader( BytecodeHandler* H ) 
  : BytecodeReader(H)
{
  int BlockSize;
  unsigned char Buffer[4096*4];

  // Read in all of the data from stdin, we cannot mmap stdin...
  while ((BlockSize = ::read(0 /*stdin*/, Buffer, 4096*4))) {
    if (BlockSize == -1)
      throw ErrnoMessage(errno, "read from standard input");
    
    FileData.insert(FileData.end(), Buffer, Buffer+BlockSize);
  }

  if (FileData.empty())
    throw std::string("Standard Input empty!");

  FileBuf = &FileData[0];
  ParseBytecode(FileBuf, FileData.size(), "<stdin>");
}

//===----------------------------------------------------------------------===//
//  Varargs transmogrification code...
//

// CheckVarargs - This is used to automatically translate old-style varargs to
// new style varargs for backwards compatibility.
static ModuleProvider *CheckVarargs(ModuleProvider *MP) {
  Module *M = MP->getModule();
  
  // Check to see if va_start takes arguments...
  Function *F = M->getNamedFunction("llvm.va_start");
  if (F == 0) return MP;  // No varargs use, just return.

  if (F->getFunctionType()->getNumParams() == 0)
    return MP;  // Modern varargs processing, just return.

  // If we get to this point, we know that we have an old-style module.
  // Materialize the whole thing to perform the rewriting.
  MP->materializeModule();

  // If the user is making use of obsolete varargs intrinsics, adjust them for
  // the user.
  if (Function *F = M->getNamedFunction("llvm.va_start")) {
    assert(F->asize() == 1 && "Obsolete va_start takes 1 argument!");
        
    const Type *RetTy = F->getFunctionType()->getParamType(0);
    RetTy = cast<PointerType>(RetTy)->getElementType();
    Function *NF = M->getOrInsertFunction("llvm.va_start", RetTy, 0);
        
    for (Value::use_iterator I = F->use_begin(), E = F->use_end(); I != E; )
      if (CallInst *CI = dyn_cast<CallInst>(*I++)) {
        Value *V = new CallInst(NF, "", CI);
        new StoreInst(V, CI->getOperand(1), CI);
        CI->getParent()->getInstList().erase(CI);
      }
    F->setName("");
  }

  if (Function *F = M->getNamedFunction("llvm.va_end")) {
    assert(F->asize() == 1 && "Obsolete va_end takes 1 argument!");
    const Type *ArgTy = F->getFunctionType()->getParamType(0);
    ArgTy = cast<PointerType>(ArgTy)->getElementType();
    Function *NF = M->getOrInsertFunction("llvm.va_end", Type::VoidTy,
                                                  ArgTy, 0);
        
    for (Value::use_iterator I = F->use_begin(), E = F->use_end(); I != E; )
      if (CallInst *CI = dyn_cast<CallInst>(*I++)) {
        Value *V = new LoadInst(CI->getOperand(1), "", CI);
        new CallInst(NF, V, "", CI);
        CI->getParent()->getInstList().erase(CI);
      }
    F->setName("");
  }
      
  if (Function *F = M->getNamedFunction("llvm.va_copy")) {
    assert(F->asize() == 2 && "Obsolete va_copy takes 2 argument!");
    const Type *ArgTy = F->getFunctionType()->getParamType(0);
    ArgTy = cast<PointerType>(ArgTy)->getElementType();
    Function *NF = M->getOrInsertFunction("llvm.va_copy", ArgTy,
                                                  ArgTy, 0);
        
    for (Value::use_iterator I = F->use_begin(), E = F->use_end(); I != E; )
      if (CallInst *CI = dyn_cast<CallInst>(*I++)) {
        Value *V = new CallInst(NF, CI->getOperand(2), "", CI);
        new StoreInst(V, CI->getOperand(1), CI);
        CI->getParent()->getInstList().erase(CI);
      }
    F->setName("");
  }
  return MP;
}

//===----------------------------------------------------------------------===//
// Wrapper functions
//===----------------------------------------------------------------------===//

/// getBytecodeBufferModuleProvider - lazy function-at-a-time loading from a
/// buffer
ModuleProvider* 
llvm::getBytecodeBufferModuleProvider(const unsigned char *Buffer,
                                      unsigned Length,
                                      const std::string &ModuleID,
                                      BytecodeHandler* H ) {
  return CheckVarargs(
      new BytecodeBufferReader(Buffer, Length, ModuleID, H));
}

/// ParseBytecodeBuffer - Parse a given bytecode buffer
///
Module *llvm::ParseBytecodeBuffer(const unsigned char *Buffer, unsigned Length,
                                  const std::string &ModuleID,
                                  std::string *ErrorStr){
  try {
    std::auto_ptr<ModuleProvider>
      AMP(getBytecodeBufferModuleProvider(Buffer, Length, ModuleID));
    return AMP->releaseModule();
  } catch (std::string &err) {
    if (ErrorStr) *ErrorStr = err;
    return 0;
  }
}

/// getBytecodeModuleProvider - lazy function-at-a-time loading from a file
///
ModuleProvider *llvm::getBytecodeModuleProvider(const std::string &Filename,
                                                BytecodeHandler* H) {
  if (Filename != std::string("-"))        // Read from a file...
    return CheckVarargs(new BytecodeFileReader(Filename,H));
  else                                     // Read from stdin
    return CheckVarargs(new BytecodeStdinReader(H));
}

/// ParseBytecodeFile - Parse the given bytecode file
///
Module *llvm::ParseBytecodeFile(const std::string &Filename,
                                std::string *ErrorStr) {
  try {
    std::auto_ptr<ModuleProvider> AMP(getBytecodeModuleProvider(Filename));
    return AMP->releaseModule();
  } catch (std::string &err) {
    if (ErrorStr) *ErrorStr = err;
    return 0;
  }
}

// AnalyzeBytecodeFile - analyze one file
Module* llvm::AnalyzeBytecodeFile(
  const std::string &Filename,  ///< File to analyze
  BytecodeAnalysis& bca,        ///< Statistical output
  std::string *ErrorStr,        ///< Error output
  std::ostream* output          ///< Dump output
)
{
  try {
    BytecodeHandler* analyzerHandler =createBytecodeAnalyzerHandler(bca,output);
    std::auto_ptr<ModuleProvider> AMP(
      getBytecodeModuleProvider(Filename,analyzerHandler));
    return AMP->releaseModule();
  } catch (std::string &err) {
    if (ErrorStr) *ErrorStr = err;
    return 0;
  }
}

// AnalyzeBytecodeBuffer - analyze a buffer
Module* llvm::AnalyzeBytecodeBuffer(
  const unsigned char* Buffer, ///< Pointer to start of bytecode buffer
  unsigned Length,             ///< Size of the bytecode buffer
  const std::string& ModuleID, ///< Identifier for the module
  BytecodeAnalysis& bca,       ///< The results of the analysis
  std::string* ErrorStr,       ///< Errors, if any.
  std::ostream* output         ///< Dump output, if any
)
{
  try {
    BytecodeHandler* hdlr = createBytecodeAnalyzerHandler(bca, output);
    std::auto_ptr<ModuleProvider>
      AMP(getBytecodeBufferModuleProvider(Buffer, Length, ModuleID, hdlr));
    return AMP->releaseModule();
  } catch (std::string &err) {
    if (ErrorStr) *ErrorStr = err;
    return 0;
  }
}

bool llvm::GetBytecodeDependentLibraries(const std::string &fname, 
                                         Module::LibraryListType& deplibs) {
  try {
    std::auto_ptr<ModuleProvider> AMP( getBytecodeModuleProvider(fname));
    Module* M = AMP->releaseModule();

    deplibs = M->getLibraries();
    delete M;
    return true;
  } catch (...) {
    deplibs.clear();
    return false;
  }
}

// Get just the externally visible defined symbols from the bytecode
bool llvm::GetBytecodeSymbols(const sys::Path& fName,
                              std::vector<std::string>& symbols) {
  try {
    std::auto_ptr<ModuleProvider> AMP( getBytecodeModuleProvider(fName.get()));

    // Get the module from the provider
    Module* M = AMP->releaseModule();

    // Loop over global variables
    for (Module::giterator GI = M->gbegin(), GE=M->gend(); GI != GE; ++GI) {
      if (GI->hasInitializer()) {
        std::string name ( GI->getName() );
        if (!name.empty()) {
          symbols.push_back(name);
        }
      }
    }

    //Loop over functions
    for (Module::iterator FI = M->begin(), FE=M->end(); FI != FE; ++FI) {
      if (!FI->isExternal()) {
        std::string name ( FI->getName() );
        if (!name.empty()) {
          symbols.push_back(name);
        }
      }
    }

    // Done with the module
    delete M;
    return true;

  } catch (...) {
    return false;
  }
}

// vim: sw=2 ai
