//===- llvm/System/SunOS/Path.cpp - SunOS Path Implementation ---*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by Reid Spencer and is distributed under the 
// University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file provides the SunOS specific implementation of the Path class.
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
//=== WARNING: Implementation here must contain only SunOS specific code 
//===          and must not be generic UNIX code (see ../Unix/Path.cpp)
//===----------------------------------------------------------------------===//

// Include the generic Unix implementation
#include "../Unix/Path.cpp"

namespace llvm {
using namespace sys;

bool 
Path::is_valid() const {
  if (path.empty()) 
    return false;
  char pathname[MAXPATHLEN];
  if (0 == realpath(path.c_str(), pathname))
    if (errno != EACCES && errno != EIO && errno != ENOENT && errno != ENOTDIR)
      return false;
  return true;
}

Path
Path::GetTemporaryDirectory() {
  char* pathname = tempnam(0,"llvm_");
  if (0 == pathname)
    ThrowErrno(std::string("Can't create temporary directory name"));
  Path result;
  result.set_directory(pathname);
  free(pathname);
  assert(result.is_valid() && "tempnam didn't create a valid pathname!");
  if (0 != mkdir(result.c_str(), S_IRWXU))
    ThrowErrno(result.get() + ": Can't create temporary directory");
  return result;
}

}

// vim: sw=2 smartindent smarttab tw=80 autoindent expandtab
