//===- TopDownClosure.cpp - Compute the top-down interprocedure closure ---===//
//
// This file implements the TDDataStructures class, which represents the
// Top-down Interprocedural closure of the data structure graph over the
// program.  This is useful (but not strictly necessary?) for applications
// like pointer analysis.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/DataStructure.h"
#include "llvm/Analysis/DSGraph.h"
#include "llvm/Module.h"
#include "llvm/DerivedTypes.h"
#include "Support/Statistic.h"
#include <set>

static RegisterAnalysis<TDDataStructures>
Y("tddatastructure", "Top-down Data Structure Analysis Closure");

// releaseMemory - If the pass pipeline is done with this pass, we can release
// our memory... here...
//
void TDDataStructures::releaseMemory() {
  BUMaps.clear();
  for (std::map<const Function*, DSGraph*>::iterator I = DSInfo.begin(),
         E = DSInfo.end(); I != E; ++I)
    delete I->second;

  // Empty map so next time memory is released, data structures are not
  // re-deleted.
  DSInfo.clear();
}

// run - Calculate the top down data structure graphs for each function in the
// program.
//
bool TDDataStructures::run(Module &M) {
  BUDataStructures &BU = getAnalysis<BUDataStructures>();

  // Calculate the CallSitesForFunction mapping from the BU info...
  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I)
    if (!I->isExternal())
      if (const std::vector<DSCallSite> *CS = BU.getCallSites(*I))
        for (unsigned i = 0, e = CS->size(); i != e; ++i)
          if (Function *F = (*CS)[i].getResolvingCaller())
            CallSitesForFunction[F].push_back(&(*CS)[i]);

  // Next calculate the graphs for each function...
  for (Module::reverse_iterator I = M.rbegin(), E = M.rend(); I != E; ++I)
    if (!I->isExternal())
      calculateGraph(*I);

  // Destroy the temporary mapping...
  CallSitesForFunction.clear();
  return false;
}

/// ResolveCallSite - This method is used to link the actual arguments together
/// with the formal arguments for a function call in the top-down closure.  This
/// method assumes that the call site arguments have been mapped into nodes
/// local to the specified graph.
///
void TDDataStructures::ResolveCallSite(DSGraph &Graph,
                                       const DSCallSite &CallSite) {
  // Resolve all of the function formal arguments...
  Function &F = Graph.getFunction();
  Function::aiterator AI = F.abegin();

  for (unsigned i = 0, e = CallSite.getNumPtrArgs(); i != e; ++i, ++AI) {
    // Advance the argument iterator to the first pointer argument...
    while (!DS::isPointerType(AI->getType())) ++AI;
    
    // TD ...Merge the formal arg scalar with the actual arg node
    DSNodeHandle &NodeForFormal = Graph.getNodeForValue(AI);
    assert(NodeForFormal.getNode() && "Pointer argument has no dest node!");
    NodeForFormal.mergeWith(CallSite.getPtrArg(i));
  }
  
  // Merge returned node in the caller with the "return" node in callee
  if (CallSite.getRetVal().getNode() && Graph.getRetNode().getNode())
    Graph.getRetNode().mergeWith(CallSite.getRetVal());
}


DSGraph &TDDataStructures::calculateGraph(Function &F) {
  // Make sure this graph has not already been calculated, or that we don't get
  // into an infinite loop with mutually recursive functions.
  //
  DSGraph *&Graph = DSInfo[&F];
  if (Graph) return *Graph;

  BUDataStructures &BU = getAnalysis<BUDataStructures>();
  DSGraph &BUGraph = BU.getDSGraph(F);
  
  // Copy the BU graph, keeping a mapping from the BUGraph to the current Graph
  std::map<const DSNode*, DSNode*> BUNodeMap;
  Graph = new DSGraph(BUGraph, BUNodeMap);

  // We only need the BUMap entries for the nodes that are used in call sites.
  // Calculate which nodes are needed.
  std::set<const DSNode*> NeededNodes;
  std::map<const Function*, std::vector<const DSCallSite*> >::iterator CSFFI
    = CallSitesForFunction.find(&F);
  if (CSFFI == CallSitesForFunction.end()) {
    BUNodeMap.clear();  // No nodes are neccesary
  } else {
    std::vector<const DSCallSite*> &CSV = CSFFI->second;
    for (unsigned i = 0, e = CSV.size(); i != e; ++i) {
      NeededNodes.insert(CSV[i]->getRetVal().getNode());
      for (unsigned j = 0, je = CSV[i]->getNumPtrArgs(); j != je; ++j)
        NeededNodes.insert(CSV[i]->getPtrArg(j).getNode());
    }
  }

  // Loop through te BUNodeMap, keeping only the nodes that are "Needed"
  for (std::map<const DSNode*, DSNode*>::iterator I = BUNodeMap.begin();
       I != BUNodeMap.end(); )
    if (NeededNodes.count(I->first) && I->first)  // Keep needed nodes...
      ++I;
    else {
      std::map<const DSNode*, DSNode*>::iterator J = I++;
      BUNodeMap.erase(J);
    }

  NeededNodes.clear();  // We are done with this temporary data structure

  // Convert the mapping from a node-to-node map into a node-to-nodehandle map
  BUNodeMapTy &BUMap = BUMaps[&F];
  BUMap.insert(BUNodeMap.begin(), BUNodeMap.end());
  BUNodeMap.clear();   // We are done with the temporary map.

  const std::vector<DSCallSite> *CallSitesP = BU.getCallSites(F);
  if (CallSitesP == 0) {
    DEBUG(std::cerr << "  [TD] No callers for: " << F.getName() << "\n");
    return *Graph;  // If no call sites, the graph is the same as the BU graph!
  }

  // Loop over all call sites of this function, merging each one into this
  // graph.
  //
  DEBUG(std::cerr << "  [TD] Inlining callers for: " << F.getName() << "\n");
  const std::vector<DSCallSite> &CallSites = *CallSitesP;
  for (unsigned c = 0, ce = CallSites.size(); c != ce; ++c) {
    const DSCallSite &CallSite = CallSites[c];
    Function &Caller = *CallSite.getResolvingCaller();
    assert(&Caller && !Caller.isExternal() &&
           "Externals function cannot 'call'!");
    
    DEBUG(std::cerr << "\t [TD] Inlining caller #" << c << " '"
          << Caller.getName() << "' into callee: " << F.getName() << "\n");
    
    // Self recursion is not tracked in BU pass...
    assert(&Caller != &F && "This cannot happen!\n");

    // Recursively compute the graph for the Caller.  It should be fully
    // resolved except if there is mutual recursion...
    //
    DSGraph &CG = calculateGraph(Caller);  // Graph to inline
    
    DEBUG(std::cerr << "\t\t[TD] Got graph for " << Caller.getName()
          << " in: " << F.getName() << "\n");
    
    // Translate call site from having links into the BU graph
    DSCallSite CallSiteInCG(CallSite, BUMaps[&Caller]);

    // These two maps keep track of where scalars in the old graph _used_
    // to point to, and of new nodes matching nodes of the old graph.
    std::map<Value*, DSNodeHandle> OldValMap;
    std::map<const DSNode*, DSNode*> OldNodeMap;

    // FIXME: Eventually use DSGraph::mergeInGraph here...
    // Graph->mergeInGraph(CallSiteInCG, CG, false);
    
    // Clone the Caller's graph into the current graph, keeping
    // track of where scalars in the old graph _used_ to point...
    // Do this here because it only needs to happens once for each Caller!
    // Strip scalars but not allocas since they are alive in callee.
    // 
    DSNodeHandle RetVal = Graph->cloneInto(CG, OldValMap, OldNodeMap,
                                           /*StripAllocas*/ false);
    ResolveCallSite(*Graph, DSCallSite(CallSiteInCG, OldNodeMap));
  }

  // Recompute the Incomplete markers and eliminate unreachable nodes.
  Graph->maskIncompleteMarkers();
  Graph->markIncompleteNodes(/*markFormals*/ !F.hasInternalLinkage()
                             /*&& FIXME: NEED TO CHECK IF ALL CALLERS FOUND!*/);
  Graph->removeDeadNodes(/*KeepAllGlobals*/ false, /*KeepCalls*/ false);

  DEBUG(std::cerr << "  [TD] Done inlining callers for: " << F.getName() << " ["
        << Graph->getGraphSize() << "+" << Graph->getFunctionCalls().size()
        << "]\n");

  return *Graph;
}
