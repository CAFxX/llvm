//===- DSGraph.h - Represent a collection of data structures ----*- C++ -*-===//
//
// This header defines the data structure graph.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_DSGRAPH_H
#define LLVM_ANALYSIS_DSGRAPH_H

#include "llvm/Analysis/DSNode.h"

//===----------------------------------------------------------------------===//
/// DSGraph - The graph that represents a function.
///
class DSGraph {
  Function *Func;          // Func - The LLVM function this graph corresponds to
  DSGraph *GlobalsGraph;   // Pointer to the common graph of global objects

  DSNodeHandle RetNode;    // The node that gets returned...
  std::vector<DSNode*> Nodes;
  std::map<Value*, DSNodeHandle> ScalarMap;

  // FunctionCalls - This vector maintains a single entry for each call
  // instruction in the current graph.  The first entry in the vector is the
  // scalar that holds the return value for the call, the second is the function
  // scalar being invoked, and the rest are pointer arguments to the function.
  // This vector is built by the Local graph and is never modified after that.
  //
  std::vector<DSCallSite> FunctionCalls;

  // AuxFunctionCalls - This vector contains call sites that have been processed
  // by some mechanism.  In pratice, the BU Analysis uses this vector to hold
  // the _unresolved_ call sites, because it cannot modify FunctionCalls.
  //
  std::vector<DSCallSite> AuxFunctionCalls;

  void operator=(const DSGraph &); // DO NOT IMPLEMENT
public:
  DSGraph() : Func(0), GlobalsGraph(0) {}      // Create a new, empty, DSGraph.
  DSGraph(Function &F, DSGraph *GlobalsGraph); // Compute the local DSGraph

  // Copy ctor - If you want to capture the node mapping between the source and
  // destination graph, you may optionally do this by specifying a map to record
  // this into.
  //
  // Note that a copied graph does not retain the GlobalsGraph pointer of the
  // source.  You need to set a new GlobalsGraph with the setGlobalsGraph
  // method.
  //
  DSGraph(const DSGraph &DSG);
  DSGraph(const DSGraph &DSG, std::map<const DSNode*, DSNodeHandle> &NodeMap);
  ~DSGraph();

  bool hasFunction() const { return Func != 0; }
  Function &getFunction() const { return *Func; }

  DSGraph *getGlobalsGraph() const { return GlobalsGraph; }
  void setGlobalsGraph(DSGraph *G) { GlobalsGraph = G; }

  /// getNodes - Get a vector of all the nodes in the graph
  /// 
  const std::vector<DSNode*> &getNodes() const { return Nodes; }
        std::vector<DSNode*> &getNodes()       { return Nodes; }

  /// addNode - Add a new node to the graph.
  ///
  void addNode(DSNode *N) { Nodes.push_back(N); }

  /// getScalarMap - Get a map that describes what the nodes the scalars in this
  /// function point to...
  ///
  std::map<Value*, DSNodeHandle> &getScalarMap() { return ScalarMap; }
  const std::map<Value*, DSNodeHandle> &getScalarMap() const {return ScalarMap;}

  /// getFunctionCalls - Return the list of call sites in the original local
  /// graph...
  ///
  const std::vector<DSCallSite> &getFunctionCalls() const {
    return FunctionCalls;
  }

  /// getAuxFunctionCalls - Get the call sites as modified by whatever passes
  /// have been run.
  ///
  std::vector<DSCallSite> &getAuxFunctionCalls() {
    return AuxFunctionCalls;
  }

  /// getNodeForValue - Given a value that is used or defined in the body of the
  /// current function, return the DSNode that it points to.
  ///
  DSNodeHandle &getNodeForValue(Value *V) { return ScalarMap[V]; }

  const DSNodeHandle &getNodeForValue(Value *V) const {
    std::map<Value*, DSNodeHandle>::const_iterator I = ScalarMap.find(V);
    assert(I != ScalarMap.end() &&
           "Use non-const lookup function if node may not be in the map");
    return I->second;
  }

  const DSNodeHandle &getRetNode() const { return RetNode; }
        DSNodeHandle &getRetNode()       { return RetNode; }

  unsigned getGraphSize() const {
    return Nodes.size();
  }

  void print(std::ostream &O) const;
  void dump() const;
  void writeGraphToFile(std::ostream &O, const std::string &GraphName) const;

  // maskNodeTypes - Apply a mask to all of the node types in the graph.  This
  // is useful for clearing out markers like Scalar or Incomplete.
  //
  void maskNodeTypes(unsigned char Mask);
  void maskIncompleteMarkers() { maskNodeTypes(~DSNode::Incomplete); }

  // markIncompleteNodes - Traverse the graph, identifying nodes that may be
  // modified by other functions that have not been resolved yet.  This marks
  // nodes that are reachable through three sources of "unknownness":
  //   Global Variables, Function Calls, and Incoming Arguments
  //
  // For any node that may have unknown components (because something outside
  // the scope of current analysis may have modified it), the 'Incomplete' flag
  // is added to the NodeType.
  //
  void markIncompleteNodes(bool markFormalArgs = true);

  // removeDeadNodes - Use a more powerful reachability analysis to eliminate
  // subgraphs that are unreachable.  This often occurs because the data
  // structure doesn't "escape" into it's caller, and thus should be eliminated
  // from the caller's graph entirely.  This is only appropriate to use when
  // inlining graphs.
  //
  void removeDeadNodes(bool KeepAllGlobals);

  // CloneFlags enum - Bits that may be passed into the cloneInto method to
  // specify how to clone the function graph.
  enum CloneFlags {
    StripAllocaBit        = 1 << 0, KeepAllocaBit     = 0 << 0,
    DontCloneCallNodes    = 1 << 1, CloneCallNodes    = 0 << 0,
    DontCloneAuxCallNodes = 1 << 2, CloneAuxCallNodes = 0 << 0,
  };

  // cloneInto - Clone the specified DSGraph into the current graph, returning
  // the Return node of the graph.  The translated ScalarMap for the old
  // function is filled into the OldValMap member.  If StripAllocas is set to
  // 'StripAllocaBit', Alloca markers are removed from the graph as the graph is
  // being cloned.
  //
  DSNodeHandle cloneInto(const DSGraph &G,
                         std::map<Value*, DSNodeHandle> &OldValMap,
                         std::map<const DSNode*, DSNodeHandle> &OldNodeMap,
                         unsigned CloneFlags = 0);

  /// mergeInGraph - The method is used for merging graphs together.  If the
  /// argument graph is not *this, it makes a clone of the specified graph, then
  /// merges the nodes specified in the call site with the formal arguments in
  /// the graph.  If the StripAlloca's argument is 'StripAllocaBit' then Alloca
  /// markers are removed from nodes.
  ///
  void mergeInGraph(DSCallSite &CS, const DSGraph &Graph, unsigned CloneFlags);

private:
  bool isNodeDead(DSNode *N);

  // removeTriviallyDeadNodes - After the graph has been constructed, this
  // method removes all unreachable nodes that are created because they got
  // merged with other nodes in the graph.  This is used as the first step of
  // removeDeadNodes.
  //
  void removeTriviallyDeadNodes(bool KeepAllGlobals = false);
};

#endif
