//===- SunOS/MappedFile.cpp - SunOS MappedFile Implementation ---*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by Reid Spencer and is distributed under the 
// University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file provides the SunOS specific implementation of the MappedFile
// concept.
//
//===----------------------------------------------------------------------===//

// Include the generic unix implementation
#include <sys/stat.h>
#include "../Unix/MappedFile.cpp"

// vim: sw=2 smartindent smarttab tw=80 autoindent expandtab
