//===- Win32/TimeValue.cpp - Win32 TimeValue Implementation -----*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by Reid Spencer and is distributed under the 
// University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file provides the Win32 specific implementation of the TimeValue class.
//
//===----------------------------------------------------------------------===//

// Include the generic Unix implementation
#include "../Unix/Unix.h"

namespace llvm {
using namespace sys;

//===----------------------------------------------------------------------===//
//=== WARNING: Implementation here must contain only Win32 specific code 
//===          and must not be generic UNIX code (see ../Unix/TimeValue.cpp)
//===----------------------------------------------------------------------===//

// FIXME: Need TimeValue::now()

// vim: sw=2 smartindent smarttab tw=80 autoindent expandtab

}
