//===-- llvm/Analysis/Writer.h - Printer for Analysis routines ---*- C++ -*--=//
//
// This library provides routines to print out various analysis results to 
// an output stream.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_WRITER_H
#define LLVM_ANALYSIS_WRITER_H

#include "llvm/Assembly/Writer.h"

namespace cfg {

  // This library provides support for printing out Intervals.
  class Interval;
  class IntervalPartition;

  void WriteToOutput(const Interval *I, std::ostream &o);
  inline std::ostream &operator <<(std::ostream &o, const Interval *I) {
    WriteToOutput(I, o); return o;
  }

  void WriteToOutput(const IntervalPartition &IP, std::ostream &o);
  inline std::ostream &operator <<(std::ostream &o,
                                   const IntervalPartition &IP) {
    WriteToOutput(IP, o); return o;
  }

  // Stuff for printing out Dominator data structures...
  class DominatorSet;
  class ImmediateDominators;
  class DominatorTree;
  class DominanceFrontier;

  void WriteToOutput(const DominatorSet &, std::ostream &o);
  inline std::ostream &operator <<(std::ostream &o, const DominatorSet &DS) {
    WriteToOutput(DS, o); return o;
  }

  void WriteToOutput(const ImmediateDominators &, std::ostream &o);
  inline std::ostream &operator <<(std::ostream &o,
                                   const ImmediateDominators &ID) {
    WriteToOutput(ID, o); return o;
  }

  void WriteToOutput(const DominatorTree &, std::ostream &o);
  inline std::ostream &operator <<(std::ostream &o, const DominatorTree &DT) {
    WriteToOutput(DT, o); return o;
  }

  void WriteToOutput(const DominanceFrontier &, std::ostream &o);
  inline std::ostream &operator <<(std::ostream &o,
                                   const DominanceFrontier &DF) {
    WriteToOutput(DF, o); return o;
  }

  // Stuff for printing out a callgraph...
  class CallGraph;
  class CallGraphNode;

  void WriteToOutput(const CallGraph &, std::ostream &o);
  inline std::ostream &operator <<(std::ostream &o, const CallGraph &CG) {
    WriteToOutput(CG, o); return o;
  }
  
  void WriteToOutput(const CallGraphNode *, std::ostream &o);
  inline std::ostream &operator <<(std::ostream &o, const CallGraphNode *CGN) {
    WriteToOutput(CGN, o); return o;
  }

  // Stuff for printing out Loop information
  class Loop;
  class LoopInfo;

  void WriteToOutput(const LoopInfo &, std::ostream &o);
  inline std::ostream &operator <<(std::ostream &o, const LoopInfo &LI) {
    WriteToOutput(LI, o); return o;
  }
  
  void WriteToOutput(const Loop *, std::ostream &o);
  inline std::ostream &operator <<(std::ostream &o, const Loop *L) {
    WriteToOutput(L, o); return o;
  }
  
}  // End namespace CFG

class InductionVariable;
void WriteToOutput(const InductionVariable &, std::ostream &o);
inline std::ostream &operator <<(std::ostream &o, const InductionVariable &IV) {
  WriteToOutput(IV, o); return o;
}


#endif
