//===- DataStructure.h - Build data structure graphs ------------*- C++ -*-===//
//
// Implement the LLVM data structure analysis library.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_DATA_STRUCTURE_H
#define LLVM_ANALYSIS_DATA_STRUCTURE_H

#include "llvm/Analysis/DSGraph.h"
#include "llvm/Pass.h"
#if 0
#include "llvm/GlobalValue.h"
#include "Support/HashExtras.h"
#include "Support/hash_set"
#endif
#include <set>

class Type;
class GlobalValue;
#if 0
class GlobalDSGraph;           // A common graph for globals in a program 
#endif
class LocalDataStructures;     // A collection of local graphs for a program
class BUDataStructures;        // A collection of bu graphs for a program
class TDDataStructures;        // A collection of td graphs for a program


// FIXME: move this stuff to a private header
namespace DataStructureAnalysis {
  // isPointerType - Return true if this first class type is big enough to hold
  // a pointer.
  //
  bool isPointerType(const Type *Ty);
}


#if 0
// GlobalDSGraph - A common graph for all the globals and their outgoing links
// to externally visible nodes.  This includes GlobalValues, New nodes,
// Cast nodes, and Calls.  This graph can only be used by one of the
// individual function graphs, and it goes away when they all go away.
// 
class GlobalDSGraph : public DSGraph {
  hash_set<const DSGraph*> Referrers;
  void addReference(const DSGraph* referrer);
  void removeReference(const DSGraph* referrer);
  friend class DSGraph;                           // give access to Referrers
  
  GlobalDSGraph(const GlobalDSGraph &GlobalDSG);  // Do not implement

  // Helper function for cloneGlobals and cloneCalls
  DSNode* cloneNodeInto(DSNode *OldNode,
                        std::map<const DSNode*, DSNode*> &NodeCache,
                        bool GlobalsAreFinal = false);

public:
  GlobalDSGraph();                                // Create an empty DSGraph
  virtual ~GlobalDSGraph();

  void    cloneGlobals(DSGraph& Graph, bool CloneCalls = false);
  void    cloneCalls  (DSGraph& Graph);
};
#endif

// LocalDataStructures - The analysis that computes the local data structure
// graphs for all of the functions in the program.
//
// FIXME: This should be a Function pass that can be USED by a Pass, and would
// be automatically preserved.  Until we can do that, this is a Pass.
//
class LocalDataStructures : public Pass {
  // DSInfo, one graph for each function
  std::map<const Function*, DSGraph*> DSInfo;
public:
  ~LocalDataStructures() { releaseMemory(); }

  virtual bool run(Module &M);

  // getDSGraph - Return the data structure graph for the specified function.
  DSGraph &getDSGraph(const Function &F) const {
    std::map<const Function*, DSGraph*>::const_iterator I = DSInfo.find(&F);
    assert(I != DSInfo.end() && "Function not in module!");
    return *I->second;
  }

  // print - Print out the analysis results...
  void print(std::ostream &O, const Module *M) const;

  // If the pass pipeline is done with this pass, we can release our memory...
  virtual void releaseMemory();

  // getAnalysisUsage - This obviously provides a data structure graph.
  virtual void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.setPreservesAll();
  }
};

#if 0
// BUDataStructures - The analysis that computes the interprocedurally closed
// data structure graphs for all of the functions in the program.  This pass
// only performs a "Bottom Up" propogation (hence the name).
//
class BUDataStructures : public Pass {
  // DSInfo, one graph for each function
  std::map<const Function*, DSGraph*> DSInfo;
public:
  ~BUDataStructures() { releaseMemory(); }

  virtual bool run(Module &M);

  // getDSGraph - Return the data structure graph for the specified function.
  DSGraph &getDSGraph(const Function &F) const {
    std::map<const Function*, DSGraph*>::const_iterator I = DSInfo.find(&F);
    assert(I != DSInfo.end() && "Function not in module!");
    return *I->second;
  }

  // print - Print out the analysis results...
  void print(std::ostream &O, const Module *M) const;

  // If the pass pipeline is done with this pass, we can release our memory...
  virtual void releaseMemory();

  // getAnalysisUsage - This obviously provides a data structure graph.
  virtual void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.setPreservesAll();
    AU.addRequired<LocalDataStructures>();
  }
private:
  DSGraph &calculateGraph(Function &F);
};


// TDDataStructures - Analysis that computes new data structure graphs
// for each function using the closed graphs for the callers computed
// by the bottom-up pass.
//
class TDDataStructures : public Pass {
  // DSInfo, one graph for each function
  std::map<const Function*, DSGraph*> DSInfo;
public:
  ~TDDataStructures() { releaseMemory(); }

  virtual bool run(Module &M);

  // getDSGraph - Return the data structure graph for the specified function.
  DSGraph &getDSGraph(const Function &F) const {
    std::map<const Function*, DSGraph*>::const_iterator I = DSInfo.find(&F);
    assert(I != DSInfo.end() && "Function not in module!");
    return *I->second;
  }

  // print - Print out the analysis results...
  void print(std::ostream &O, const Module *M) const;

  // If the pass pipeline is done with this pass, we can release our memory...
  virtual void releaseMemory();

  // getAnalysisUsage - This obviously provides a data structure graph.
  virtual void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.setPreservesAll();
    AU.addRequired<BUDataStructures>();
  }
private:
  DSGraph &calculateGraph(Function &F);
  void pushGraphIntoCallee(DSGraph &callerGraph, DSGraph &calleeGraph,
                           std::map<Value*, DSNodeHandle> &OldValMap,
                           std::map<const DSNode*, DSNode*> &OldNodeMap);
};
#endif

#endif
