//===-- EquivClassGraphs.h - Merge equiv-class graphs -----------*- C++ -*-===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This pass is the same as the complete bottom-up graphs, but with functions
// partitioned into equivalence classes and a single merged DS graph for all
// functions in an equivalence class.  After this merging, graphs are inlined
// bottom-up on the SCCs of the final (CBU) call graph.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/DataStructure/DataStructure.h"
#include "llvm/Analysis/DataStructure/DSGraph.h"
#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/ADT/STLExtras.h"
#include <vector>
#include <map>
#include <ext/hash_map>

namespace llvm {

class Module;
class Function;

namespace PA {
  /// EquivClassGraphs - This is the same as the complete bottom-up graphs, but
  /// with functions partitioned into equivalence classes and a single merged
  /// DS graph for all functions in an equivalence class.  After this merging,
  /// graphs are inlined bottom-up on the SCCs of the final (CBU) call graph.
  ///
  struct EquivClassGraphs : public ModulePass {
    CompleteBUDataStructures *CBU;

    DSGraph *GlobalsGraph;

    // DSInfo - one graph for each function.
    hash_map<const Function*, DSGraph*> DSInfo;

    /// ActualCallees - The actual functions callable from indirect call sites.
    ///
    hash_multimap<Instruction*, Function*> ActualCallees;
  
    // Equivalence class where functions that can potentially be called via the
    // same function pointer are in the same class.
    EquivalenceClasses<Function*> FuncECs;

    /// OneCalledFunction - For each indirect call, we keep track of one
    /// target of the call.  This is used to find equivalence class called by
    /// a call site.
    std::map<DSNode*, Function *> OneCalledFunction;

  public:
    /// EquivClassGraphs - Computes the equivalence classes and then the
    /// folded DS graphs for each class.
    /// 
    virtual bool runOnModule(Module &M);

    /// getDSGraph - Return the data structure graph for the specified function.
    /// This returns the folded graph.  The folded graph is the same as the CBU
    /// graph iff the function is in a singleton equivalence class AND all its 
    /// callees also have the same folded graph as the CBU graph.
    /// 
    DSGraph &getDSGraph(const Function &F) const {
      hash_map<const Function*, DSGraph*>::const_iterator I = DSInfo.find(&F);
      assert(I != DSInfo.end() && "No graph computed for that function!");
      return *I->second;
    }

    /// getSomeCalleeForCallSite - Return any one callee function at
    /// a call site.
    /// 
    Function *getSomeCalleeForCallSite(const CallSite &CS) const;

    DSGraph &getGlobalsGraph() const {
      return *GlobalsGraph;
    }
    
    typedef hash_multimap<Instruction*, Function*> ActualCalleesTy;
    const ActualCalleesTy &getActualCallees() const {
      return ActualCallees;
    }

    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.setPreservesAll();
      AU.addRequired<CompleteBUDataStructures>();
    }

  private:
    void buildIndirectFunctionSets(Module &M);

    unsigned processSCC(DSGraph &FG, std::vector<DSGraph*> &Stack,
                        unsigned &NextID, 
                        std::map<DSGraph*, unsigned> &ValMap);
    void processGraph(DSGraph &FG);

    DSGraph &getOrCreateGraph(Function &F);
  };

}; // end PA namespace

}; // end llvm namespace
