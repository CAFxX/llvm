//===- ConfigData.h - Configuration Data Provider ---------------*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by Reid Spencer and is distributed under the 
// University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file declares the LLVMC_ConfigDataProvider class which implements the
// generation of ConfigData objects for the CompilerDriver.
//
//===------------------------------------------------------------------------===
#ifndef LLVM_TOOLS_LLVMC_CONFIGDATA_H
#define LLVM_TOOLS_LLVMC_CONFIGDATA_H

#include "CompilerDriver.h"
#include <Support/hash_map>

namespace llvm {
  /// This class provides the high level interface to the LLVM Compiler Driver.
  /// The driver's purpose is to make it easier for compiler writers and users
  /// of LLVM to utilize the compiler toolkits and LLVM toolset by learning only
  /// the interface of one program (llvmc).
  /// 
  /// @see llvmc.cpp
  /// @brief The interface to the LLVM Compiler Driver.
  class LLVMC_ConfigDataProvider : public CompilerDriver::ConfigDataProvider {
    /// @name Constructor
    /// @{
    public:
      LLVMC_ConfigDataProvider();
      virtual ~LLVMC_ConfigDataProvider();

    /// @name Methods
    /// @{
    public:
      /// @brief Provide the configuration data to the CompilerDriver.
      virtual CompilerDriver::ConfigData* 
        ProvideConfigData(const std::string& filetype);

      /// @brief Allow the configuration directory to be set
      virtual void setConfigDir(const std::string& dirName) { configDir = dirName; }

    /// @}
    /// @name Data
    /// @{
    private:
      /// @brief This type is used internally to hold the configuration data.
      typedef hash_map<std::string,CompilerDriver::ConfigData*,
          hash<std::string>,std::equal_to<std::string> > ConfigDataMap;
      ConfigDataMap Configurations; ///< The cache of configurations
      std::string configDir;
    /// @}
  };
}

#endif
