//===- llvm/Bytecode/WriteBytecodePass.h - Bytecode Writer Pass --*- C++ -*--=//
//
// This file defines a simple pass to write the working module to a file after
// pass processing is completed.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_BYTECODE_WRITEBYTECODEPASS_H
#define LLVM_BYTECODE_WRITEBYTECODEPASS_H

#include "llvm/Pass.h"
#include "llvm/Bytecode/Writer.h"

class WriteBytecodePass : public Pass {
  ostream *Out;           // ostream to print on
  bool DeleteStream;
public:
  inline WriteBytecodePass(ostream *o = &cout, bool DS = false)
    : Out(o), DeleteStream(DS) {
  }

  inline ~WriteBytecodePass() {
    if (DeleteStream) delete Out;
  }
  
  bool doPassFinalization(Module *M) {
    WriteBytecodeToFile(M, *Out);    
    return false;
  }
};

#endif
