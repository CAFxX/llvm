//===- SunOS/TimeValue.cpp - SunOS TimeValue Implementation -----*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by Reid Spencer and is distributed under the 
// University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file provides the SunOS specific implementation of the TimeValue class.
//
//===----------------------------------------------------------------------===//

// Include the generic Unix implementation
#include "../Unix/Unix.h"
#include <sys/time.h>

namespace llvm {
using namespace sys;

//===----------------------------------------------------------------------===//
//=== WARNING: Implementation here must contain only SunOS specific code 
//===          and must not be generic UNIX code (see ../Unix/TimeValue.cpp)
//===----------------------------------------------------------------------===//

void TimeValue::now() {
  struct timeval the_time;
  timerclear(&the_time);
  if (0 != ::gettimeofday(&the_time,0)) 
      ThrowErrno("Couldn't obtain time of day");

  this->set( static_cast<TimeValue::SecondsType>( the_time.tv_sec ), 
             static_cast<TimeValue::NanoSecondsType>( the_time.tv_usec * 
               NANOSECONDS_PER_MICROSECOND ) );
}

// vim: sw=2 smartindent smarttab tw=80 autoindent expandtab

}
