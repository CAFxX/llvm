//===- bugpoint.cpp - The LLVM Bugpoint utility ---------------------------===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This program is an automated compiler debugger tool.  It is used to narrow
// down miscompilations and crash problems to a specific pass in the compiler,
// and the specific Module or Function input that is causing the problem.
//
//===----------------------------------------------------------------------===//

#include "BugDriver.h"
#include "llvm/Support/PassNameParser.h"
#include "llvm/Support/ToolRunner.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/PluginLoader.h"
#include "llvm/System/Process.h"
#include "llvm/System/Signals.h"
using namespace llvm;

static cl::list<std::string>
InputFilenames(cl::Positional, cl::OneOrMore,
               cl::desc("<input llvm ll/bc files>"));

// The AnalysesList is automatically populated with registered Passes by the
// PassNameParser.
//
static cl::list<const PassInfo*, bool, PassNameParser>
PassList(cl::desc("Passes available:"), cl::ZeroOrMore);

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv,
                              " LLVM automatic testcase reducer. See\nhttp://"
                              "llvm.cs.uiuc.edu/docs/CommandGuide/bugpoint.html"
                              " for more information.\n");
  sys::PrintStackTraceOnErrorSignal();

  BugDriver D(argv[0]);
  if (D.addSources(InputFilenames)) return 1;
  D.addPasses(PassList.begin(), PassList.end());

  // Bugpoint has the ability of generating a plethora of core files, so to
  // avoid filling up the disk, we prevent it
  sys::Process::PreventCoreFiles();

  try {
    return D.run();
  } catch (ToolExecutionError &TEE) {
    std::cerr << "Tool execution error: " << TEE.what() << '\n';
    return 1;
  } catch (...) {
    std::cerr << "Whoops, an exception leaked out of bugpoint.  "
              << "This is a bug in bugpoint!\n";
    return 1;
  }
}
