//===- BottomUpClosure.cpp - Compute bottom-up interprocedural closure ----===//
//
// This file implements the BUDataStructures class, which represents the
// Bottom-Up Interprocedural closure of the data structure graph over the
// program.  This is useful for applications like pool allocation, but **not**
// applications like alias analysis.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/DataStructure.h"
#include "llvm/Analysis/DSGraph.h"
#include "llvm/Module.h"
#include "Support/Statistic.h"
using std::map;

static RegisterAnalysis<BUDataStructures>
X("budatastructure", "Bottom-up Data Structure Analysis Closure");

namespace DataStructureAnalysis { // TODO: FIXME: Eliminate
  // isPointerType - Return true if this first class type is big enough to hold
  // a pointer.
  //
  bool isPointerType(const Type *Ty);
}
using namespace DataStructureAnalysis;


// releaseMemory - If the pass pipeline is done with this pass, we can release
// our memory... here...
//
void BUDataStructures::releaseMemory() {
  // Delete all call site information
  CallSites.clear();

  for (map<const Function*, DSGraph*>::iterator I = DSInfo.begin(),
         E = DSInfo.end(); I != E; ++I)
    delete I->second;

  // Empty map so next time memory is released, data structures are not
  // re-deleted.
  DSInfo.clear();
}

// run - Calculate the bottom up data structure graphs for each function in the
// program.
//
bool BUDataStructures::run(Module &M) {
  // Simply calculate the graphs for each function...
  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I)
    if (!I->isExternal())
      calculateGraph(*I);
  return false;
}

// ResolveArguments - Resolve the formal and actual arguments for a function
// call.
//
static void ResolveArguments(DSCallSite &Call, Function &F,
                             map<Value*, DSNodeHandle> &ValueMap) {
  // Resolve all of the function arguments...
  Function::aiterator AI = F.abegin();
  for (unsigned i = 0, e = Call.getNumPtrArgs(); i != e; ++i, ++AI) {
    // Advance the argument iterator to the first pointer argument...
    while (!isPointerType(AI->getType())) ++AI;
    
    // Add the link from the argument scalar to the provided value
    ValueMap[AI].mergeWith(Call.getPtrArg(i));
  }
}

DSGraph &BUDataStructures::calculateGraph(Function &F) {
  // Make sure this graph has not already been calculated, or that we don't get
  // into an infinite loop with mutually recursive functions.
  //
  DSGraph *&Graph = DSInfo[&F];
  if (Graph) return *Graph;

  // Copy the local version into DSInfo...
  Graph = new DSGraph(getAnalysis<LocalDataStructures>().getDSGraph(F));

#if 0
  // Populate the GlobalsGraph with globals from this one.
  Graph->GlobalsGraph->cloneGlobals(*Graph, /*cloneCalls*/ false);
#endif

  // Start resolving calls...
  std::vector<DSCallSite> &FCs = Graph->getFunctionCalls();

  DEBUG(std::cerr << "  [BU] Inlining: " << F.getName() << "\n");

  bool Inlined;
  do {
    Inlined = false;

    for (unsigned i = 0; i != FCs.size(); ++i) {
      // Copy the call, because inlining graphs may invalidate the FCs vector.
      DSCallSite Call = FCs[i];

      // If the function list is complete...
      if ((Call.getCallee().getNode()->NodeType & DSNode::Incomplete)==0) {
        // Start inlining all of the functions we can... some may not be
        // inlinable if they are external...
        //
        std::vector<GlobalValue*> Callees =
          Call.getCallee().getNode()->getGlobals();

        // Loop over the functions, inlining whatever we can...
        for (unsigned c = 0; c != Callees.size(); ++c) {
          // Must be a function type, so this cast MUST succeed.
          Function &FI = cast<Function>(*Callees[c]);

          if (&FI == &F) {
            // Self recursion... simply link up the formal arguments with the
            // actual arguments...
            DEBUG(std::cerr << "\t[BU] Self Inlining: " << F.getName() << "\n");

            // Handle the return value if present...
            Graph->getRetNode().mergeWith(Call.getRetVal());

            // Resolve the arguments in the call to the actual values...
            ResolveArguments(Call, F, Graph->getValueMap());

            // Erase the entry in the callees vector
            Callees.erase(Callees.begin()+c--);

          } else if (!FI.isExternal()) {
            DEBUG(std::cerr << "\t[BU] In " << F.getName() << " inlining: "
                  << FI.getName() << "\n");
            
            // Get the data structure graph for the called function, closing it
            // if possible (which is only impossible in the case of mutual
            // recursion...
            //
            DSGraph &GI = calculateGraph(FI);  // Graph to inline

            DEBUG(std::cerr << "\t\t[BU] Got graph for " << FI.getName()
                  << " in: " << F.getName() << "\n");

            // Record that the original DSCallSite was a call site of FI.
            // This may or may not have been known when the DSCallSite was
            // originally created.
            std::vector<DSCallSite> &CallSitesForFunc = CallSites[&FI];
            CallSitesForFunc.push_back(Call);
            CallSitesForFunc.back().setResolvingCaller(&F);
            CallSitesForFunc.back().setCallee(0);

            // Clone the callee's graph into the current graph, keeping
            // track of where scalars in the old graph _used_ to point,
            // and of the new nodes matching nodes of the old graph.
            map<Value*, DSNodeHandle> OldValMap;
            map<const DSNode*, DSNode*> OldNodeMap;

            // The clone call may invalidate any of the vectors in the data
            // structure graph.  Strip locals and don't copy the list of callers
            DSNodeHandle RetVal = Graph->cloneInto(GI, OldValMap, OldNodeMap,
                                                   /*StripScalars*/   true,
                                                   /*StripAllocas*/   true);

            // Resolve the arguments in the call to the actual values...
            ResolveArguments(Call, FI, OldValMap);

            // Handle the return value if present...
            RetVal.mergeWith(Call.getRetVal());

            // Erase the entry in the Callees vector
            Callees.erase(Callees.begin()+c--);

          } else if (FI.getName() == "printf" || FI.getName() == "sscanf" ||
                     FI.getName() == "fprintf" || FI.getName() == "open" ||
                     FI.getName() == "sprintf") {
            // FIXME: These special cases (eg printf) should go away when we can
            // define functions that take a variable number of arguments.

            // FIXME: at the very least, this should update mod/ref info
            // Erase the entry in the globals vector
            Callees.erase(Callees.begin()+c--);
          }
        }

        if (Callees.empty()) {         // Inlined all of the function calls?
          // Erase the call if it is resolvable...
          FCs.erase(FCs.begin()+i--);  // Don't skip a the next call...
          Inlined = true;
        } else if (Callees.size() !=
                   Call.getCallee().getNode()->getGlobals().size()) {
          // Was able to inline SOME, but not all of the functions.  Construct a
          // new global node here.
          //
          assert(0 && "Unimpl!");
          Inlined = true;
        }
      }
    }

    // Recompute the Incomplete markers.  If there are any function calls left
    // now that are complete, we must loop!
    if (Inlined) {
      Graph->maskIncompleteMarkers();
      Graph->markIncompleteNodes();
      Graph->removeDeadNodes(/*KeepAllGlobals*/ true, /*KeepCalls*/ true);
    }
  } while (Inlined && !FCs.empty());

  Graph->maskIncompleteMarkers();
  Graph->markIncompleteNodes();
  Graph->removeTriviallyDeadNodes(false);
  Graph->removeDeadNodes(/*KeepAllGlobals*/ true, /*KeepCalls*/ true);

  DEBUG(std::cerr << "  [BU] Done inlining: " << F.getName() << " ["
        << Graph->getGraphSize() << "+" << Graph->getFunctionCalls().size()
        << "]\n");

  return *Graph;
}
