//===- Unix/Process.cpp - Unix Process Implementation --------- -*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by Reid Spencer and is distributed under the 
// University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file provides the generic Unix implementation of the Process class.
//
//===----------------------------------------------------------------------===//

#include "Unix.h"
#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif
#ifdef HAVE_MALLOC_H
#include <malloc.h>
#endif

//===----------------------------------------------------------------------===//
//=== WARNING: Implementation here must contain only generic UNIX code that
//===          is guaranteed to work on *all* UNIX variants.
//===----------------------------------------------------------------------===//

namespace llvm {
using namespace sys;

unsigned 
Process::GetPageSize() 
{
#if defined(HAVE_GETPAGESIZE)
  static const int page_size = ::getpagesize();
#elif defined(HAVE_SYSCONF)
  static long page_size = ::sysconf(_SC_PAGE_SIZE);
#else
#warning Cannot get the page size on this machine
#endif
  return static_cast<unsigned>(page_size);
}

#if defined(HAVE_SBRK)
static char* som = reinterpret_cast<char*>(::sbrk(0));
#endif

uint64_t 
Process::GetMallocUsage()
{
#if defined(HAVE_MALLINFO)
  struct mallinfo mi;
  mi = ::mallinfo();
  return mi.uordblks;
#elif defined(HAVE_SBRK)
  // Note this is only an approximation and more closely resembles
  // the value returned by mallinfo in the arena field.
  char * eom = sbrk(0);
  if (eom != ((char*)-1) && som != ((char*)-1))
    return eom - som;
  else
    return 0;
#else
#warning Cannot get malloc info on this platform
  return 0;
#endif
}

uint64_t
Process::GetTotalMemoryUsage()
{
#if defined(HAVE_MALLINFO)
  struct mallinfo mi = ::mallinfo();
  return mi.uordblks + mi.hblkhd;
#else
#warning Cannot get total memory size on this platform
  return 0;
#endif
}

void
Process::GetTimeUsage(TimeValue& elapsed, TimeValue& user_time, 
                      TimeValue& sys_time)
{
  elapsed = TimeValue::now();
#if defined(HAVE_GETRUSAGE)
  struct rusage usage;
  ::getrusage(RUSAGE_SELF, &usage);
  user_time.seconds( usage.ru_utime.tv_sec );
  user_time.microseconds( usage.ru_utime.tv_usec );
  sys_time.seconds( usage.ru_stime.tv_sec );
  sys_time.microseconds( usage.ru_stime.tv_usec );
#else
#warning Cannot get usage times on this platform
  user_time.seconds(0);
  user_time.microseconds(0);
  sys_time.seconds(0);
  sys_time.microseconds(0);
#endif
}


}
// vim: sw=2 smartindent smarttab tw=80 autoindent expandtab
