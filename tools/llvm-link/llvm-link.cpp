//===- llvm-link.cpp - Low-level LLVM linker ------------------------------===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This utility may be invoked in the following manner:
//  llvm-link a.bc b.bc c.bc -o x.bc
//
//===----------------------------------------------------------------------===//

#include "llvm/Module.h"
#include "llvm/Analysis/Verifier.h"
#include "llvm/Bytecode/Reader.h"
#include "llvm/Bytecode/Writer.h"
#include "llvm/Support/Linker.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/System/Signals.h"
#include "llvm/System/Path.h"
#include <fstream>
#include <iostream>
#include <memory>

using namespace llvm;

static cl::list<std::string>
InputFilenames(cl::Positional, cl::OneOrMore,
               cl::desc("<input bytecode files>"));

static cl::opt<std::string>
OutputFilename("o", cl::desc("Override output filename"), cl::init("-"),
               cl::value_desc("filename"));

static cl::opt<bool> Force("f", cl::desc("Overwrite output files"));

static cl::opt<bool>
Verbose("v", cl::desc("Print information about actions taken"));

static cl::opt<bool>
DumpAsm("d", cl::desc("Print assembly as linked"), cl::Hidden);

// LoadFile - Read the specified bytecode file in and return it.  This routine
// searches the link path for the specified file to try to find it...
//
static inline std::auto_ptr<Module> LoadFile(const std::string &FN) {
  sys::Path Filename;
  if (!Filename.setFile(FN)) {
    std::cerr << "Invalid file name: '" << FN << "'\n";
    return std::auto_ptr<Module>();
  }

  std::string ErrorMessage;
  if (Filename.exists()) {
    if (Verbose) std::cerr << "Loading '" << Filename.c_str() << "'\n";
    Module* Result = ParseBytecodeFile(Filename.get(), &ErrorMessage);
    if (Result) return std::auto_ptr<Module>(Result);   // Load successful!

    if (Verbose) {
      std::cerr << "Error opening bytecode file: '" << Filename.c_str() << "'";
      if (ErrorMessage.size()) std::cerr << ": " << ErrorMessage;
      std::cerr << "\n";
    }
  } else {
    std::cerr << "Bytecode file: '" << Filename.c_str() 
              << "' does not exist.\n";
  }

  return std::auto_ptr<Module>();
}

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv, " llvm linker\n");
  sys::PrintStackTraceOnErrorSignal();
  assert(InputFilenames.size() > 0 && "OneOrMore is not working");

  unsigned BaseArg = 0;
  std::string ErrorMessage;

  std::auto_ptr<Module> Composite(LoadFile(InputFilenames[BaseArg]));
  if (Composite.get() == 0) {
    std::cerr << argv[0] << ": error loading file '"
              << InputFilenames[BaseArg] << "'\n";
    return 1;
  }

  for (unsigned i = BaseArg+1; i < InputFilenames.size(); ++i) {
    std::auto_ptr<Module> M(LoadFile(InputFilenames[i]));
    if (M.get() == 0) {
      std::cerr << argv[0] << ": error loading file '"
                << InputFilenames[i] << "'\n";
      return 1;
    }

    if (Verbose) std::cerr << "Linking in '" << InputFilenames[i] << "'\n";

    if (LinkModules(Composite.get(), M.get(), &ErrorMessage)) {
      std::cerr << argv[0] << ": link error in '" << InputFilenames[i]
                << "': " << ErrorMessage << "\n";
      return 1;
    }
  }

  // TODO: Iterate over the -l list and link in any modules containing
  // global symbols that have not been resolved so far.

  if (DumpAsm) std::cerr << "Here's the assembly:\n" << Composite.get();

  std::ostream *Out = &std::cout;  // Default to printing to stdout...
  if (OutputFilename != "-") {
    if (!Force && std::ifstream(OutputFilename.c_str())) {
      // If force is not specified, make sure not to overwrite a file!
      std::cerr << argv[0] << ": error opening '" << OutputFilename
                << "': file exists!\n"
                << "Use -f command line argument to force output\n";
      return 1;
    }
    Out = new std::ofstream(OutputFilename.c_str());
    if (!Out->good()) {
      std::cerr << argv[0] << ": error opening '" << OutputFilename << "'!\n";
      return 1;
    }

    // Make sure that the Out file gets unlinked from the disk if we get a
    // SIGINT
    sys::RemoveFileOnSignal(OutputFilename);
  }

  if (verifyModule(*Composite.get())) {
    std::cerr << argv[0] << ": linked module is broken!\n";
    return 1;
  }

  if (Verbose) std::cerr << "Writing bytecode...\n";
  WriteBytecodeToFile(Composite.get(), *Out);

  if (Out != &std::cout) delete Out;
  return 0;
}
