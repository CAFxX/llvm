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

#include "llvm/System/Path.h"
#include "llvm/ModuleProvider.h"
#include "llvm/Module.h"
#include <string>

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

/// @brief Parse the given bytecode file
Module* ParseBytecodeFile(const std::string &Filename,
                          std::string *ErrorStr = 0);

/// @brief Parse a given bytecode buffer
Module* ParseBytecodeBuffer(const unsigned char *Buffer,
                            unsigned BufferSize,
                            const std::string &ModuleID = "",
                            std::string *ErrorStr = 0);

/// This function will read only the necessary parts of a bytecode file in order
/// to determine the list of dependent libraries encoded within it. The \p
/// deplibs parameter will contain a vector of strings of the bytecode module's
/// dependent libraries.
/// @returns true on success, false otherwise
/// @brief Get the list of dependent libraries from a bytecode file.
bool GetBytecodeDependentLibraries(const std::string &fileName, 
                                   Module::LibraryListType& deplibs);

/// This function will read only the necessary parts of a bytecode file in order
/// to obtain a list of externally visible global symbols that the bytecode
/// module defines. This is used for archiving and linking when only the list 
/// of symbols the module defines is needed.
/// @returns true on success, false otherwise
/// @brief Get a bytecode file's externally visibile defined global symbols.
bool GetBytecodeSymbols(const sys::Path& fileName, 
                        std::vector<std::string>& syms);

/// Read bytecode files from the specfied archive (.a) file, convert them
/// to Module* and provide them in the \p Objects argument. If an error
/// occurs, ErrorStr (if non-null) will be set to a string explaining
/// the error.
/// @return true on error, false on success.
/// @brief Get a vector of Module* from a bytecode archive file
bool ReadArchiveFile(const std::string &Filename,
                     std::vector<Module*> &Objects,
                     std::string *ErrorStr = 0);

} // End llvm namespace

#endif
