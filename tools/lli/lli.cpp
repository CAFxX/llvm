//===- lli.cpp - LLVM Interpreter / Dynamic compiler ----------------------===//
//
// This utility provides a way to execute LLVM bytecode without static
// compilation.  This consists of a very simple and slow (but portable)
// interpreter, along with capability for system specific dynamic compilers.  At
// runtime, the fastest (stable) execution engine is selected to run the
// program.  This means the JIT compiler for the current platform if it's
// available.
//
//===----------------------------------------------------------------------===//

#include "ExecutionEngine.h"
#include "Support/CommandLine.h"
#include "llvm/Bytecode/Reader.h"
#include "llvm/Module.h"
#include "llvm/Target/TargetMachineImpls.h"

namespace {
  cl::opt<std::string>
  InputFile(cl::desc("<input bytecode>"), cl::Positional, cl::init("-"));

  cl::list<std::string>
  InputArgv(cl::ConsumeAfter, cl::desc("<program arguments>..."));

  cl::opt<std::string>
  MainFunction ("f", cl::desc("Function to execute"), cl::init("main"),
		cl::value_desc("function name"));

  cl::opt<bool> DebugMode("d", cl::desc("Start program in debugger"));

  cl::opt<bool> TraceMode("trace", cl::desc("Enable Tracing"));

  cl::opt<bool> ForceInterpreter("force-interpreter",
				 cl::desc("Force interpretation: disable JIT"),
				 cl::init(true));
}

//===----------------------------------------------------------------------===//
// ExecutionEngine Class Implementation
//

ExecutionEngine::~ExecutionEngine() {
  delete &CurMod;
}

//===----------------------------------------------------------------------===//
// main Driver function
//
int main(int argc, char** argv) {
  cl::ParseCommandLineOptions(argc, argv,
			      " llvm interpreter & dynamic compiler\n");

  // Load the bytecode...
  string ErrorMsg;
  Module *M = ParseBytecodeFile(InputFile, &ErrorMsg);
  if (M == 0) {
    cout << "Error parsing '" << InputFile << "': "
         << ErrorMsg << "\n";
    exit(1);
  }

#if 0
  // Link in the runtime library for LLI...
  string RuntimeLib = getCurrentExecutablePath();
  if (!RuntimeLib.empty()) RuntimeLib += "/";
  RuntimeLib += "RuntimeLib.bc";

  if (Module *SupportLib = ParseBytecodeFile(RuntimeLib, &ErrorMsg)) {
    if (LinkModules(M, SupportLib, &ErrorMsg))
      std::cerr << "Error Linking runtime library into current module: "
                << ErrorMsg << "\n";
  } else {
    std::cerr << "Error loading runtime library '"+RuntimeLib+"': "
              << ErrorMsg << "\n";
  }
#endif

  // FIXME: This should look at the PointerSize and endianness of the bytecode
  // file to determine the endianness and pointer size of target machine to use.
  unsigned Config = TM::PtrSize64 | TM::BigEndian;

  ExecutionEngine *EE = 0;

  // If there is nothing that is forcing us to use the interpreter, make a JIT.
  if (!ForceInterpreter && !DebugMode && !TraceMode)
    EE = ExecutionEngine::createJIT(M, Config);

  // If we can't make a JIT, make an interpreter instead.
  if (EE == 0)
    EE = ExecutionEngine::createInterpreter(M, Config, DebugMode, TraceMode);

  // Add the module name to the start of the argv vector...
  InputArgv.insert(InputArgv.begin(), InputFile);

  // Run the main function!
  int ExitCode = EE->run(MainFunction, InputArgv);

  // Now that we are done executing the program, shut down the execution engine
  delete EE;
  return ExitCode;
}
