//===- EquivClassGraphs.cpp - Merge equiv-class graphs & inline bottom-up -===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This pass is the same as the complete bottom-up graphs, but
// with functions partitioned into equivalence classes and a single merged
// DS graph for all functions in an equivalence class.  After this merging,
// graphs are inlined bottom-up on the SCCs of the final (CBU) call graph.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "ECGraphs"
#include "EquivClassGraphs.h"
#include "llvm/Module.h"
#include "llvm/Pass.h"
#include "llvm/Analysis/DataStructure/DSGraph.h"
#include "llvm/Analysis/DataStructure/DataStructure.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/Debug.h"
#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/EquivalenceClasses.h"
#include "llvm/ADT/STLExtras.h"
using namespace llvm;

namespace {
  RegisterAnalysis<PA::EquivClassGraphs> X("equivdatastructure",
                    "Equivalence-class Bottom-up Data Structure Analysis");
  Statistic<> NumEquivBUInlines("equivdatastructures",
                                "Number of graphs inlined");
  Statistic<> NumFoldGraphInlines("Inline equiv-class graphs bottom up",
                                  "Number of graphs inlined");
}

#ifndef NDEBUG
template<typename GT>
static void CheckAllGraphs(Module *M, GT &ECGraphs) {
  DSGraph &GG = ECGraphs.getGlobalsGraph();

  for (Module::iterator I = M->begin(), E = M->end(); I != E; ++I)
    if (!I->isExternal()) {
      DSGraph &G = ECGraphs.getDSGraph(*I);

      DSGraph::NodeMapTy GlobalsGraphNodeMapping;
      for (DSScalarMap::global_iterator I = G.getScalarMap().global_begin(),
             E = G.getScalarMap().global_end(); I != E; ++I)
        DSGraph::computeNodeMapping(G.getNodeForValue(*I),
                                    GG.getNodeForValue(*I),
                                    GlobalsGraphNodeMapping);
    } 
}
#endif

// getSomeCalleeForCallSite - Return any one callee function at a call site.
// 
Function *PA::EquivClassGraphs::
getSomeCalleeForCallSite(const CallSite &CS) const {
  Function *thisFunc = CS.getCaller();
  assert(thisFunc && "getSomeCalleeForCallSite(): Not a valid call site?");
  DSGraph &DSG = getDSGraph(*thisFunc);
  DSNode *calleeNode = DSG.getNodeForValue(CS.getCalledValue()).getNode();
  std::map<DSNode*, Function *>::const_iterator I =
    OneCalledFunction.find(calleeNode);
  return (I == OneCalledFunction.end())? NULL : I->second;
}

// runOnModule - Calculate the bottom up data structure graphs for each function
// in the program.
//
bool PA::EquivClassGraphs::runOnModule(Module &M) {
  CBU = &getAnalysis<CompleteBUDataStructures>();
  CheckAllGraphs(&M, *CBU);

  GlobalsGraph = new DSGraph(CBU->getGlobalsGraph());
  GlobalsGraph->setPrintAuxCalls();

  ActualCallees = CBU->getActualCallees();

  // Find equivalence classes of functions called from common call sites.
  // Fold the CBU graphs for all functions in an equivalence class.
  buildIndirectFunctionSets(M);

  // Stack of functions used for Tarjan's SCC-finding algorithm.
  std::vector<Function*> Stack;
  std::map<Function*, unsigned> ValMap;
  unsigned NextID = 1;

  if (Function *Main = M.getMainFunction()) {
    if (!Main->isExternal())
      processSCC(getOrCreateGraph(*Main), *Main, Stack, NextID, ValMap);
  } else {
    std::cerr << "Fold Graphs: No 'main' function found!\n";
  }
  
  for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I)
    if (!I->isExternal() && !ValMap.count(I))
      processSCC(getOrCreateGraph(*I), *I, Stack, NextID, ValMap);

  DEBUG(CheckAllGraphs(&M, *this));

  getGlobalsGraph().removeTriviallyDeadNodes();
  return false;
}


// buildIndirectFunctionSets - Iterate over the module looking for indirect
// calls to functions.  If a call site can invoke any functions [F1, F2... FN],
// unify the N functions together in the FuncECs set.
//
void PA::EquivClassGraphs::buildIndirectFunctionSets(Module &M) {
  const ActualCalleesTy& AC = CBU->getActualCallees();
  
  // Loop over all of the indirect calls in the program.  If a call site can
  // call multiple different functions, we need to unify all of the callees into
  // the same equivalence class.
  Instruction *LastInst = 0;
  Function *FirstFunc = 0;
  for (ActualCalleesTy::const_iterator I=AC.begin(), E=AC.end(); I != E; ++I) {
    if (I->second->isExternal())
      continue;                         // Ignore functions we cannot modify

    CallSite CS = CallSite::get(I->first);

    if (CS.getCalledFunction()) {       // Direct call:
      FuncECs.addElement(I->second);    // -- Make sure function has equiv class
      FirstFunc = I->second;            // -- First callee at this site
    } else {                            // Else indirect call
      // DEBUG(std::cerr << "CALLEE: " << I->second->getName()
      //       << " from : " << I->first);
      if (I->first != LastInst) {
        // This is the first callee from this call site.
        LastInst = I->first;
        FirstFunc = I->second;
	// Instead of storing the lastInst For Indirection call Sites we store
	// the DSNode for the function ptr arguemnt
	Function *thisFunc = LastInst->getParent()->getParent();
        DSGraph &TFG = CBU->getDSGraph(*thisFunc);
	DSNode *calleeNode = TFG.getNodeForValue(CS.getCalledValue()).getNode();
        OneCalledFunction[calleeNode] = FirstFunc;
        FuncECs.addElement(I->second);
      } else {
        // This is not the first possible callee from a particular call site.
        // Union the callee in with the other functions.
        FuncECs.unionSetsWith(FirstFunc, I->second);
#ifndef NDEBUG
	Function *thisFunc = LastInst->getParent()->getParent();
        DSGraph &TFG = CBU->getDSGraph(*thisFunc);
	DSNode *calleeNode = TFG.getNodeForValue(CS.getCalledValue()).getNode();
        assert(OneCalledFunction.count(calleeNode) > 0 && "Missed a call?");
#endif
      }
    }

    // Now include all functions that share a graph with any function in the
    // equivalence class.  More precisely, if F is in the class, and G(F) is
    // its graph, then we include all other functions that are also in G(F).
    // Currently, that is just the functions in the same call-graph-SCC as F.
    // 
    DSGraph& funcDSGraph = CBU->getDSGraph(*I->second);
    const DSGraph::ReturnNodesTy &RetNodes = funcDSGraph.getReturnNodes();
    for (DSGraph::ReturnNodesTy::const_iterator RI=RetNodes.begin(),
           RE=RetNodes.end(); RI != RE; ++RI)
      FuncECs.unionSetsWith(FirstFunc, RI->first);
  }

  // Now that all of the equivalences have been built, merge the graphs for
  // each equivalence class.
  //
  std::set<Function*> &leaderSet = FuncECs.getLeaderSet();
  DEBUG(std::cerr << "\nIndirect Function Equivalence Sets:\n");
  for (std::set<Function*>::iterator LI = leaderSet.begin(),
	 LE = leaderSet.end(); LI != LE; ++LI) {

    Function* LF = *LI;
    const std::set<Function*>& EqClass = FuncECs.getEqClass(LF);

#ifndef NDEBUG
    if (EqClass.size() > 1) {
      DEBUG(std::cerr <<"  Equivalence set for leader " <<LF->getName()<<" = ");
      for (std::set<Function*>::const_iterator EqI = EqClass.begin(),
             EqEnd = EqClass.end(); EqI != EqEnd; ++EqI)
        DEBUG(std::cerr << " " << (*EqI)->getName() << ",");
      DEBUG(std::cerr << "\n");
    }
#endif

    if (EqClass.size() > 1) {
      // This equiv class has multiple functions: merge their graphs.  First,
      // clone the CBU graph for the leader and make it the common graph for the
      // equivalence graph.
      DSGraph *mergedG = &getOrCreateGraph(*LF);

      // Record the argument nodes for use in merging later below.
      std::vector<DSNodeHandle> ArgNodes;  

      for (Function::aiterator AI1 = LF->abegin(); AI1 != LF->aend(); ++AI1)
        if (DS::isPointerType(AI1->getType()))
          ArgNodes.push_back(mergedG->getNodeForValue(AI1));
      
      // Merge in the graphs of all other functions in this equiv. class.  Note
      // that two or more functions may have the same graph, and it only needs
      // to be merged in once.  Use a set to find repetitions.
      std::set<DSGraph*> GraphsMerged;
      for (std::set<Function*>::const_iterator EqI = EqClass.begin(),
             EqEnd = EqClass.end(); EqI != EqEnd; ++EqI) {
        Function* F = *EqI;
        DSGraph*& FG = DSInfo[F];

        if (F == LF || FG == mergedG)
          continue;
        
        // Record the "folded" graph for the function.
        FG = mergedG;
        
        // Clone this member of the equivalence class into mergedG
        DSGraph* CBUGraph = &CBU->getDSGraph(*F); 
        if (GraphsMerged.count(CBUGraph) > 0)
          continue;
        
        GraphsMerged.insert(CBUGraph);
        DSGraph::NodeMapTy NodeMap;    

        mergedG->cloneInto(*CBUGraph, mergedG->getScalarMap(),
                           mergedG->getReturnNodes(), NodeMap, 0);

        // Merge the return nodes of all functions together.
        mergedG->getReturnNodes()[LF].mergeWith(mergedG->getReturnNodes()[F]);

        // Merge the function arguments with all argument nodes found so far.
        // If there are extra function args, add them to the vector of argNodes
        Function::aiterator AI2 = F->abegin(), AI2end = F->aend();
        for (unsigned arg=0, numArgs = ArgNodes.size();
             arg != numArgs && AI2 != AI2end; ++AI2, ++arg)
          if (DS::isPointerType(AI2->getType()))
            ArgNodes[arg].mergeWith(mergedG->getNodeForValue(AI2));

        for ( ; AI2 != AI2end; ++AI2)
          if (DS::isPointerType(AI2->getType()))
            ArgNodes.push_back(mergedG->getNodeForValue(AI2));
        DEBUG(mergedG->AssertGraphOK());
      }
    }
  }
  DEBUG(std::cerr << "\n");
}


DSGraph &PA::EquivClassGraphs::getOrCreateGraph(Function &F) {
  // Has the graph already been created?
  DSGraph *&Graph = DSInfo[&F];
  if (Graph) return *Graph;

  DSGraph &CBUGraph = CBU->getDSGraph(F);

  // Copy the CBU graph...
  Graph = new DSGraph(CBUGraph);           // updates the map via reference
  Graph->setGlobalsGraph(&getGlobalsGraph());
  Graph->setPrintAuxCalls();

  // Make sure to update the DSInfo map for all functions in the graph!
  for (DSGraph::ReturnNodesTy::iterator I = Graph->getReturnNodes().begin();
       I != Graph->getReturnNodes().end(); ++I)
    if (I->first != &F) {
      DSGraph *&FG = DSInfo[I->first];
      assert(FG == NULL || FG == &CBU->getDSGraph(*I->first) &&
             "Merging function in SCC twice?");
      FG = Graph;
    }

  return *Graph;
}


unsigned PA::EquivClassGraphs::processSCC(DSGraph &FG, Function &F,
                                          std::vector<Function*> &Stack,
                                          unsigned &NextID, 
                                          std::map<Function*,unsigned> &ValMap){
  DEBUG(std::cerr << "    ProcessSCC for function " << F.getName() << "\n");

  std::map<Function*, unsigned>::iterator It = ValMap.lower_bound(&F);
  if (It != ValMap.end() && It->first == &F)
    return It->second;

  unsigned Min = NextID++, MyID = Min;
  ValMap[&F] = Min;
  Stack.push_back(&F);

  // The edges out of the current node are the call site targets...
  for (unsigned i = 0, e = FG.getFunctionCalls().size(); i != e; ++i) {
    Instruction *Call = FG.getFunctionCalls()[i].getCallSite().getInstruction();

    // Loop over all of the actually called functions...
    ActualCalleesTy::const_iterator I, E;
    for (tie(I, E) = getActualCallees().equal_range(Call); I != E; ++I)
      if (!I->second->isExternal()) {
        // Process the callee as necessary.
        unsigned M = processSCC(getOrCreateGraph(*I->second), *I->second,
                                Stack, NextID, ValMap);
        if (M < Min) Min = M;
      }
  }

  assert(ValMap[&F] == MyID && "SCC construction assumption wrong!");
  if (Min != MyID)
    return Min;         // This is part of a larger SCC!

  // If this is a new SCC, process it now.
  bool IsMultiNodeSCC = false;
  while (Stack.back() != &F) {
    DSGraph *NG = &getOrCreateGraph(*Stack.back());
    ValMap[Stack.back()] = ~0U;

    // Since all SCCs must be the same as those found in CBU, we do not need to
    // do any merging.  Make sure all functions in the SCC share the same graph.
    assert(NG == &FG && "ECG discovered different SCC's than the CBU pass?");
    
    Stack.pop_back();
    IsMultiNodeSCC = true;
  }

  Stack.pop_back();

  processGraph(FG, F);
  ValMap[&F] = ~0U;
  return MyID;
}


/// processGraph - Process the CBU graphs for the program in bottom-up order on
/// the SCC of the __ACTUAL__ call graph.  This builds final folded CBU graphs.
void PA::EquivClassGraphs::processGraph(DSGraph &G, Function &F) {
  DEBUG(std::cerr << "    ProcessGraph for function " << F.getName() << "\n");

  hash_set<Instruction*> calls;

  DSGraph* CallerGraph = &getOrCreateGraph(F);

  // Else we need to inline some callee graph.  Visit all call sites.
  // The edges out of the current node are the call site targets...
  for (unsigned i=0, e = CallerGraph->getFunctionCalls().size(); i != e; ++i) {
    const DSCallSite &CS = CallerGraph->getFunctionCalls()[i];
    Instruction *TheCall = CS.getCallSite().getInstruction();

    assert(calls.insert(TheCall).second &&
           "Call instruction occurs multiple times in graph??");
    
    // Inline the common callee graph into the current graph, if the callee
    // graph has not changed.  Note that all callees should have the same
    // graph so we only need to do this once.
    // 
    DSGraph* CalleeGraph = NULL;
    ActualCalleesTy::const_iterator I, E;
    tie(I, E) = getActualCallees().equal_range(TheCall);
    unsigned TNum, Num;

    // Loop over all potential callees to find the first non-external callee.
    for (TNum = 0, Num = std::distance(I, E); I != E; ++I, ++TNum)
      if (!I->second->isExternal())
        break;

    // Now check if the graph has changed and if so, clone and inline it.
    if (I != E) {
      Function *CalleeFunc = I->second;
      
      // Merge the callee's graph into this graph, if not already the same.
      // Callees in the same equivalence class (which subsumes those
      // in the same SCCs) have the same graph.  Note that all recursion
      // including self-recursion have been folded in the equiv classes.
      // 
      CalleeGraph = &getOrCreateGraph(*CalleeFunc);
      if (CalleeGraph != CallerGraph) {
        ++NumFoldGraphInlines;
        CallerGraph->mergeInGraph(CS, *CalleeFunc, *CalleeGraph,
                                  DSGraph::KeepModRefBits |
                                  DSGraph::StripAllocaBit |
                                  DSGraph::DontCloneCallNodes |
                                  DSGraph::DontCloneAuxCallNodes);
        DEBUG(std::cerr << "    Inlining graph [" << i << "/" << e-1
              << ":" << TNum << "/" << Num-1 << "] for "
              << CalleeFunc->getName() << "["
              << CalleeGraph->getGraphSize() << "+"
              << CalleeGraph->getAuxFunctionCalls().size()
              << "] into '" /*<< CallerGraph->getFunctionNames()*/ << "' ["
              << CallerGraph->getGraphSize() << "+"
              << CallerGraph->getAuxFunctionCalls().size()
              << "]\n");
      }
    }

#ifndef NDEBUG
    // Now loop over the rest of the callees and make sure they have the
    // same graph as the one inlined above.
    if (CalleeGraph)
      for (++I, ++TNum; I != E; ++I, ++TNum)
        if (!I->second->isExternal())
          assert(CalleeGraph == &getOrCreateGraph(*I->second) &&
                 "Callees at a call site have different graphs?");
#endif
  }

  // Recompute the Incomplete markers.
  assert(CallerGraph->getInlinedGlobals().empty());
  CallerGraph->maskIncompleteMarkers();
  CallerGraph->markIncompleteNodes(DSGraph::MarkFormalArgs);
  
  // Delete dead nodes.  Treat globals that are unreachable but that can
  // reach live nodes as live.
  CallerGraph->removeDeadNodes(DSGraph::KeepUnreachableGlobals);

  // When this graph is finalized, clone the globals in the graph into the
  // globals graph to make sure it has everything, from all graphs.
  DSScalarMap &MainSM = CallerGraph->getScalarMap();
  ReachabilityCloner RC(*CallerGraph->getGlobalsGraph(), *CallerGraph,
                        DSGraph::StripAllocaBit);

  // Clone everything reachable from globals in the function graph into the
  // globals graph.
  for (DSScalarMap::global_iterator I = MainSM.global_begin(),
         E = MainSM.global_end(); I != E; ++I) 
    RC.getClonedNH(MainSM[*I]);

  DEBUG(std::cerr << "  -- DONE ProcessGraph for function "
                  << F.getName() << "\n");
}
