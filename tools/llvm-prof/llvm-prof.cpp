//===- llvm-prof.cpp - Read in and process llvmprof.out data files --------===//
// 
//                      The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This tools is meant for use with the various LLVM profiling instrumentation
// passes.  It reads in the data file produced by executing an instrumented
// program, and outputs a nice report.
//
//===----------------------------------------------------------------------===//

#include "ProfileInfo.h"
#include "llvm/Function.h"
#include "llvm/Bytecode/Reader.h"
#include "Support/CommandLine.h"
#include <iostream>
#include <cstdio>
#include <map>

namespace {
  cl::opt<std::string> 
  BytecodeFile(cl::Positional, cl::desc("<program bytecode file>"),
               cl::Required);

  cl::opt<std::string> 
  ProfileDataFile(cl::Positional, cl::desc("<llvmprof.out file>"),
                  cl::Optional, cl::init("llvmprof.out"));
}

// PairSecondSort - A sorting predicate to sort by the second element of a pair.
template<class T>
struct PairSecondSort
  : public std::binary_function<std::pair<T, unsigned>,
                                std::pair<T, unsigned>, bool> {
  bool operator()(const std::pair<T, unsigned> &LHS,
                  const std::pair<T, unsigned> &RHS) const {
    return LHS.second < RHS.second;
  }
};


int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv, " llvm profile dump decoder\n");

  // Read in the bytecode file...
  std::string ErrorMessage;
  Module *M = ParseBytecodeFile(BytecodeFile, &ErrorMessage);
  if (M == 0) {
    std::cerr << argv[0] << ": " << BytecodeFile << ": " << ErrorMessage
              << "\n";
    return 1;
  }

  // Read the profiling information
  ProfileInfo PI(argv[0], ProfileDataFile, *M);

  // Output a report.  Eventually, there will be multiple reports selectable on
  // the command line, for now, just keep things simple.

  // Emit the most frequent function table...
  std::vector<std::pair<Function*, unsigned> > FunctionCounts;
  PI.getFunctionCounts(FunctionCounts);

  // Sort by the frequency, backwards.
  std::sort(FunctionCounts.begin(), FunctionCounts.end(),
            std::not2(PairSecondSort<Function*>()));

  unsigned TotalExecutions = 0;
  for (unsigned i = 0, e = FunctionCounts.size(); i != e; ++i)
    TotalExecutions += FunctionCounts[i].second;
  
  std::cout << "===" << std::string(73, '-') << "===\n"
            << "LLVM profiling output for:\n";
  
  for (unsigned i = 0, e = PI.getNumExecutions(); i != e; ++i) {
    std::cout << "  ";
    if (e != 1) std::cout << i << ". ";
    std::cout << PI.getExecution(i) << "\n";
  }
  
  std::cout << "\n===" << std::string(73, '-') << "===\n";
  std::cout << "Function execution frequencies:\n\n";

  // Print out the function frequencies...
  printf(" ##   Frequency\n");
  for (unsigned i = 0, e = FunctionCounts.size(); i != e; ++i) {
    if (FunctionCounts[i].second == 0) {
      printf("\n  NOTE: %d function%s never executed!\n",
             e-i, e-i-1 ? "s were" : " was");
      break;
    }

    printf("%3d. %5d/%d %s\n", i, FunctionCounts[i].second, TotalExecutions,
           FunctionCounts[i].first->getName().c_str());
  }


  // If we have block count information, print out the LLVM module with
  // frequency annotations.
  if (PI.hasAccurateBlockCounts()) {
    std::vector<std::pair<BasicBlock*, unsigned> > Counts;
    PI.getBlockCounts(Counts);
    std::map<BasicBlock*, unsigned> BlockFreqs(Counts.begin(), Counts.end());
                   
  }

  return 0;
}
