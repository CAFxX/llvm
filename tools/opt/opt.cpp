//===------------------------------------------------------------------------===
// LLVM 'OPT' UTILITY 
//
// This utility may be invoked in the following manner:
//  opt --help               - Output information about command line switches
//  opt [options] -dce       - Run a dead code elimination pass on input 
//                             bytecodes
//  opt [options] -constprop - Run a constant propogation pass on input 
//                             bytecodes
//  opt [options] -inline    - Run a method inlining pass on input bytecodes
//  opt [options] -strip     - Strip symbol tables out of methods
//  opt [options] -mstrip    - Strip module & method symbol tables
//
// Optimizations may be specified an arbitrary number of times on the command
// line, they are run in the order specified.
//
// TODO: Add a -all option to keep applying all optimizations until the program
//       stops permuting.
//
//===------------------------------------------------------------------------===

#include <iostream.h>
#include <fstream.h>
#include "llvm/Module.h"
#include "llvm/Bytecode/Reader.h"
#include "llvm/Bytecode/Writer.h"
#include "llvm/Tools/CommandLine.h"
#include "llvm/Optimizations/AllOpts.h"

using namespace opt;

struct {
  const string ArgName, Name;
  bool (*OptPtr)(Module *C);
} OptTable[] = {
  { "-dce"       , "Dead Code Elimination", DoDeadCodeElimination },
  { "-constprop" , "Constant Propogation",  DoConstantPropogation }, 
  { "-inline"    , "Method Inlining",       DoMethodInlining      },
  { "-strip"     , "Strip Symbols",         DoSymbolStripping     },
  { "-mstrip"    , "Strip Module Symbols",  DoFullSymbolStripping },
  { "-indvars"   , "Simplify Induction Vars",DoInductionVariableCannonicalize },
  { "-sccp"      , "Sparse Conditional Constant Prop", DoSCCP },
  { "-cpm"       , "Constant Pool Merging", DoConstantPoolMerging },
  { "-adce"      , "Agressive DCE",         DoADCE },
  { "-raise"     , "Raise to Higher Level", DoRaiseRepresentation },
};

int main(int argc, char **argv) {
  ToolCommandLine Opts(argc, argv, false);
  bool Quiet = false;

  for (int i = 1; i < argc; i++) {
    if (string(argv[i]) == string("--help")) {
      cerr << argv[0] << " usage:\n"
           << "  " << argv[0] << " --help  - Print this usage information\n";
      for (unsigned j = 0; j < sizeof(OptTable)/sizeof(OptTable[0]); ++j) {
	cerr << "\t" << OptTable[j].ArgName << "\t - Enable " 
	     << OptTable[j].Name << endl;
      }
      return 1;
    } else if (string(argv[i]) == string("-q")) {
      Quiet = true; argv[i] = 0;
    }
  }
  
  ostream *Out = &cout;  // Default to printing to stdout...

  Module *C = ParseBytecodeFile(Opts.getInputFilename());
  if (C == 0) {
    cerr << "bytecode didn't read correctly.\n";
    return 1;
  }


  for (int i = 1; i < argc; i++) {
    if (argv[i] == 0) continue;
    unsigned j;
    for (j = 0; j < sizeof(OptTable)/sizeof(OptTable[0]); j++) {
      if (string(argv[i]) == OptTable[j].ArgName) {
        if (OptTable[j].OptPtr(C) && !Quiet)
          cerr << OptTable[j].Name << " pass made modifications!\n";
        break;
      }
    }

    if (j == sizeof(OptTable)/sizeof(OptTable[0])) 
      cerr << "'" << argv[i] << "' argument unrecognized: ignored\n";
  }

  if (Opts.getOutputFilename() != "-") {
    Out = new ofstream(Opts.getOutputFilename().c_str(), 
                       (Opts.getForce() ? 0 : ios::noreplace)|ios::out);
    if (!Out->good()) {
      cerr << "Error opening " << Opts.getOutputFilename() 
           << "!\n";
      delete C;
      return 1;
    }
  }

  // Okay, we're done now... write out result...
  WriteBytecodeToFile(C, *Out);
  delete C;

  if (Out != &cout) delete Out;
  return 0;
}
