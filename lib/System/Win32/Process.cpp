//===- Win32/Process.cpp - Win32 Process Implementation ------- -*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by Jeff Cohen and is distributed under the 
// University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file provides the Win32 specific implementation of the Process class.
//
//===----------------------------------------------------------------------===//

#include "Win32.h"
#include <psapi.h>
#include <malloc.h>

#pragma comment(lib, "psapi.lib")

//===----------------------------------------------------------------------===//
//=== WARNING: Implementation here must contain only Win32 specific code 
//===          and must not be UNIX code
//===----------------------------------------------------------------------===//

namespace llvm {
using namespace sys;

// This function retrieves the page size using GetSystemInfo and is present
// solely so it can be called once in Process::GetPageSize to initialize the
// static variable PageSize.
inline unsigned GetPageSizeOnce() {
  // NOTE: A 32-bit application running under WOW64 is supposed to use
  // GetNativeSystemInfo.  However, this interface is not present prior
  // to Windows XP so to use it requires dynamic linking.  It is not clear
  // how this affects the reported page size, if at all.  One could argue
  // that LLVM ought to run as 64-bits on a 64-bit system, anyway.
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  return static_cast<unsigned>(info.dwPageSize);
}

unsigned 
Process::GetPageSize() {
  static const unsigned PageSize = GetPageSizeOnce();
  return PageSize;
}

uint64_t 
Process::GetMallocUsage()
{
  _HEAPINFO hinfo;
  hinfo._pentry = NULL;

  size_t size = 0;

  while (_heapwalk(&hinfo) == _HEAPOK)
    size += hinfo._size;

  return size;
}

uint64_t
Process::GetTotalMemoryUsage()
{
  PROCESS_MEMORY_COUNTERS pmc;
  GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
  return pmc.PagefileUsage;
}

void
Process::GetTimeUsage(
  TimeValue& elapsed, TimeValue& user_time, TimeValue& sys_time)
{
  elapsed = TimeValue::now();

  uint64_t ProcCreate, ProcExit, KernelTime, UserTime;
  GetProcessTimes(GetCurrentProcess(), (FILETIME*)&ProcCreate, 
                  (FILETIME*)&ProcExit, (FILETIME*)&KernelTime,
                  (FILETIME*)&UserTime);

  // FILETIME's are # of 100 nanosecond ticks (1/10th of a microsecond)
  user_time.seconds( UserTime / 10000000 );
  user_time.nanoseconds( unsigned(UserTime % 10000000) * 100 );
  sys_time.seconds( KernelTime / 10000000 );
  sys_time.nanoseconds( unsigned(KernelTime % 10000000) * 100 );
}

}
// vim: sw=2 smartindent smarttab tw=80 autoindent expandtab
