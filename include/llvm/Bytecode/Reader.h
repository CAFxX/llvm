//===-- llvm/Bytecode/Reader.h - Reader for VM bytecode files ---*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This functionality is implemented by the lib/Bytecode/Reader library.
// This library is used to read VM bytecode files from an iostream.
//
// Note that performance of this library is _crucial_ for performance of the
// JIT type applications, so we have designed the bytecode format to support
// quick reading.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_BYTECODE_READER_H
#define LLVM_BYTECODE_READER_H

#include "llvm/ModuleProvider.h"
#include <string>
#include <vector>

namespace llvm {

// Forward declare the handler class
class BytecodeHandler;

/// getBytecodeModuleProvider - lazy function-at-a-time loading from a file
///
ModuleProvider *getBytecodeModuleProvider(
  const std::string &Filename, ///< Name of file to be read
  BytecodeHandler* H = 0       ///< Optional handler for reader events
);

/// getBytecodeBufferModuleProvider - lazy function-at-a-time loading from a
/// buffer
///
ModuleProvider *getBytecodeBufferModuleProvider(const unsigned char *Buffer,
                                                unsigned BufferSize,
                                                const std::string &ModuleID="",
						BytecodeHandler* H = 0);

/// ParseBytecodeFile - Parse the given bytecode file
///
Module* ParseBytecodeFile(const std::string &Filename,
                          std::string *ErrorStr = 0);

/// ParseBytecodeBuffer - Parse a given bytecode buffer
///
Module* ParseBytecodeBuffer(const unsigned char *Buffer,
                            unsigned BufferSize,
                            const std::string &ModuleID = "",
                            std::string *ErrorStr = 0);

/// ReadArchiveFile - Read bytecode files from the specfied .a file, returning
/// true on error, or false on success.
///
bool ReadArchiveFile(const std::string &Filename,
                     std::vector<Module*> &Objects,
                     std::string *ErrorStr = 0);

} // End llvm namespace

#endif
