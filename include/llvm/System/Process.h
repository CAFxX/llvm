//===- llvm/System/Process.h ------------------------------------*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by Reid Spencer and is distributed under the 
// University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file declares the llvm::sys::Process class.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SYSTEM_PROCESS_H
#define LLVM_SYSTEM_PROCESS_H

#include "llvm/System/TimeValue.h"

namespace llvm {
namespace sys {

  /// This class provides an abstraction for getting information about the
  /// currently executing process. 
  /// @since 1.4
  /// @brief An abstraction for operating system processes.
  class Process {
    /// @name Accessors
    /// @{
    public:
      /// This static function will return the operating system's virtual memory
      /// page size.
      /// @returns The number of bytes in a virtual memory page.
      /// @throws nothing
      /// @brief Get the virtual memory page size
      static unsigned GetPageSize();

      /// This static function will return the total amount of memory allocated
      /// by the process. This only counts the memory allocated via the malloc,
      /// calloc and realloc functions and includes any "free" holes in the 
      /// allocated space. 
      /// @throws nothing
      /// @brief Return process memory usage.
      static uint64_t GetMallocUsage();

      /// This static function will return the total memory usage of the 
      /// process. This includes code, data, stack and mapped pages usage. Notei
      /// that the value returned here is not necessarily the Running Set Size,
      /// it is the total virtual memory usage, regardless of mapped state of
      /// that memory.
      static uint64_t GetTotalMemoryUsage();

      /// This static function will set \p user_time to the amount of CPU time 
      /// spent in user (non-kernel) mode and \p sys_time to the amount of CPU
      /// time spent in system (kernel) mode.  If the operating system does not
      /// support collection of these metrics, a zero TimeValue will be for both
      /// values.
      static void GetTimeUsage(
        TimeValue& elapsed,
          ///< Returns the TimeValue::now() giving current time
        TimeValue& user_time, 
          ///< Returns the current amount of user time for the process
        TimeValue& sys_time
          ///< Returns the current amount of system time for the process
      );

      /// This function makes the necessary calls to the operating system to 
      /// prevent core files or any other kind of large memory dumps that can 
      /// occur when a program fails.
      /// @brief Prevent core file generation.
      static void PreventCoreFiles();

    /// @}
  };
}
}

// vim: sw=2

#endif
