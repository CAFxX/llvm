//===- DataStructure.h - Build a Module's call graph -------------*- C++ -*--=//
//
// Implement the LLVM data structure analysis library.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_DATA_STRUCTURE_H
#define LLVM_ANALYSIS_DATA_STRUCTURE_H

#include "llvm/Pass.h"
#include <string>

class Type;
class CallInst;
class AllocationInst;
class FunctionArgument;
class DSNode;
class FunctionRepBuilder;
class GlobalValue;
class FunctionDSGraph;
class DataStructure;

// FIXME: move this somewhere private
unsigned countPointerFields(const Type *Ty);

// PointerVal - Represent a pointer to a datastructure.  The pointer points to
// a node, and can index into it.  This is used for getelementptr instructions,
// which do not affect which node a pointer points to, but does change the field
// index
//
struct PointerVal {
  DSNode *Node;
  unsigned Index;  // Index into Node->FieldLinks[]
public:
  PointerVal(DSNode *N, unsigned Idx = 0) : Node(N), Index(Idx) {}

  DSNode *getNode() const { return Node; }
  unsigned getIndex() const { return Index; }

  inline bool operator==(DSNode *N) const { return Node == N; }
  inline bool operator!=(DSNode *N) const { return Node != N; }

  inline bool operator==(const PointerVal &PV) const {
    return Node == PV.Node && Index == PV.Index;
  }
  inline bool operator!=(const PointerVal &PV) const { return !operator==(PV); }

  void print(std::ostream &O) const;
};


// PointerValSet - This class represents a list of pointer values.  The add
// method is used to add values to the set, and ensures that duplicates cannot
// happen.
//
class PointerValSet {
  std::vector<PointerVal> Vals;
  void dropRefs();
  void addRefs();
public:
  PointerValSet() {}
  PointerValSet(const PointerValSet &PVS) : Vals(PVS.Vals) { addRefs(); }
  ~PointerValSet() { dropRefs(); }
  const PointerValSet &operator=(const PointerValSet &PVS);

  const PointerVal &operator[](unsigned i) const { return Vals[i]; }

  unsigned size() const { return Vals.size(); }
  bool empty() const { return Vals.empty(); }
  void clear() { dropRefs(); Vals.clear(); }

  // add - Add the specified pointer, or contents of the specified PVS to this
  // pointer set.  If a 'Pointer' value is provided, notify the underlying data
  // structure node that the pointer is pointing to it, so that it can be
  // invalidated if neccesary later.  True is returned if the value is new to
  // this pointer.
  //
  bool add(const PointerVal &PV, Value *Pointer = 0);
  bool add(const PointerValSet &PVS, Value *Pointer = 0) {
    bool Changed = false;
    for (unsigned i = 0, e = PVS.size(); i != e; ++i)
      Changed |= add(PVS[i], Pointer);
    return Changed;
  }

  // removePointerTo - Remove a single pointer val that points to the specified
  // node...
  void removePointerTo(DSNode *Node);

  void print(std::ostream &O) const;
};


//===----------------------------------------------------------------------===//
// DSNode - Base class for all data structure nodes...
//
// This class keeps track of its type, the pointer fields in the data structure,
// and a list of LLVM values that are pointing to this node.
//
class DSNode {
  friend class FunctionDSGraph;
  const Type *Ty;
  std::vector<PointerValSet> FieldLinks;
  std::vector<Value*> Pointers;   // Values pointing to me...
  std::vector<PointerValSet*> Referrers;
  
  DSNode(const DSNode &);         // DO NOT IMPLEMENT
  void operator=(const DSNode &); // DO NOT IMPLEMENT
public:
  enum NodeTy {
    NewNode, CallNode, ShadowNode, ArgNode, GlobalNode
  } NodeType;

  DSNode(enum NodeTy NT, const Type *T);
  virtual ~DSNode() {
    dropAllReferences();
    assert(Referrers.empty() && "Referrers to dead node exist!");
  }

  unsigned getNumLinks() const { return FieldLinks.size(); }
  PointerValSet &getLink(unsigned i) {
    assert(i < getNumLinks() && "Field links access out of range...");
    return FieldLinks[i];
  }

  // addReferrer - Keep the referrer set up to date...
  void addReferrer(PointerValSet *PVS) { Referrers.push_back(PVS); }
  void removeReferrer(PointerValSet *PVS);
  const std::vector<PointerValSet*> &getReferrers() const { return Referrers; }

  void addPointer(Value *V) { Pointers.push_back(V); }
  const std::vector<Value*> &getPointers() const { return Pointers; }

  const Type *getType() const { return Ty; }

  void print(std::ostream &O) const;

  virtual std::string getCaption() const = 0;
  virtual const std::vector<PointerValSet> *getAuxLinks() const {
    return 0;  // Default to nothing...
  }

  DSNode *clone() const {
    DSNode *New = cloneImpl();
    // Add all of the pointers to the new node...
    for (unsigned pn = 0, pe = Pointers.size(); pn != pe; ++pn)
      New->addPointer(Pointers[pn]);
    return New;
  }


  virtual void dropAllReferences() {
    FieldLinks.clear();
  }

protected:
  virtual DSNode *cloneImpl() const = 0;
  virtual void mapNode(std::map<const DSNode*, DSNode*> &NodeMap,
                       const DSNode *Old);
};


// NewDSNode - Represent all allocation (malloc or alloca) in the program.
//
class NewDSNode : public DSNode {
  AllocationInst *Allocation;
public:
  NewDSNode(AllocationInst *V);

  virtual std::string getCaption() const;

  // Support type inquiry through isa, cast, and dyn_cast...
  static bool classof(const NewDSNode *) { return true; }
  static bool classof(const DSNode *N) { return N->NodeType == NewNode; }
protected:
  virtual NewDSNode *cloneImpl() const { return new NewDSNode(Allocation); }
};


// GlobalDSNode - Represent the memory location that a global variable occupies
//
class GlobalDSNode : public DSNode {
  GlobalValue *Val;
public:
  GlobalDSNode(GlobalValue *V);

  virtual std::string getCaption() const;

  // Support type inquiry through isa, cast, and dyn_cast...
  static bool classof(const GlobalDSNode *) { return true; }
  static bool classof(const DSNode *N) { return N->NodeType == GlobalNode; }
private:
  virtual GlobalDSNode *cloneImpl() const { return new GlobalDSNode(Val); }
};


// CallDSNode - Represent a call instruction in the program...
//
class CallDSNode : public DSNode {
  CallInst *CI;
  std::vector<PointerValSet> ArgLinks;
public:
  CallDSNode(CallInst *CI);

  CallInst *getCall() const { return CI; }

  const std::vector<PointerValSet> *getAuxLinks() const { return &ArgLinks; }
  virtual std::string getCaption() const;

  bool addArgValue(unsigned ArgNo, const PointerValSet &PVS) {
    return ArgLinks[ArgNo].add(PVS);
  }

  unsigned getNumArgs() const { return ArgLinks.size(); }
  const PointerValSet &getArgValues(unsigned ArgNo) const {
    assert(ArgNo < ArgLinks.size() && "Arg # out of range!");
    return ArgLinks[ArgNo];
  }

  virtual void dropAllReferences() {
    DSNode::dropAllReferences();
    ArgLinks.clear();
  }


  // Support type inquiry through isa, cast, and dyn_cast...
  static bool classof(const CallDSNode *) { return true; }
  static bool classof(const DSNode *N) { return N->NodeType == CallNode; }
private:
  virtual CallDSNode *cloneImpl() const { return new CallDSNode(CI); }
  virtual void mapNode(std::map<const DSNode*, DSNode*> &NodeMap,
                       const DSNode *Old);
}; 


// ArgDSNode - Represent an incoming argument to the current function...
//
class ArgDSNode : public DSNode {
  FunctionArgument *FuncArg;
public:
  ArgDSNode(FunctionArgument *MA);
  virtual std::string getCaption() const;

  // Support type inquiry through isa, cast, and dyn_cast...
  static bool classof(const ArgDSNode *) { return true; }
  static bool classof(const DSNode *N) { return N->NodeType == ArgNode; }
private:
  virtual ArgDSNode *cloneImpl() const { return new ArgDSNode(FuncArg); }
};


// ShadowDSNode - Represent a chunk of memory that we need to be able to
// address.  These are generated due to (for example) pointer type method
// arguments... if the pointer is dereferenced, we need to have a node to point
// to.  When functions are integrated into each other, shadow nodes are
// resolved.
//
class ShadowDSNode : public DSNode {
  friend class FunctionDSGraph;
  DSNode *Parent;
  Module *Mod;
  ShadowDSNode *ShadowParent;   // Nonnull if this is a synthesized node...
  std::vector<std::pair<const Type *, ShadowDSNode *> > SynthNodes;
public:
  ShadowDSNode(DSNode *Parent, Module *M);
  virtual std::string getCaption() const;

  // synthesizeNode - Create a new shadow node that is to be linked into this
  // chain..
  //
  ShadowDSNode *synthesizeNode(const Type *Ty, FunctionRepBuilder *Rep);

  // Support type inquiry through isa, cast, and dyn_cast...
  static bool classof(const ShadowDSNode *) { return true; }
  static bool classof(const DSNode *N) { return N->NodeType == ShadowNode; }

private:
  ShadowDSNode(const Type *Ty, Module *M, ShadowDSNode *ShadParent);
protected:
  virtual void mapNode(std::map<const DSNode*, DSNode*> &NodeMap,
                       const DSNode *Old);
  virtual ShadowDSNode *cloneImpl() const {
    if (ShadowParent)
      return new ShadowDSNode(getType(), Mod, ShadowParent);
    else
      return new ShadowDSNode(Parent, Mod);
  }
};


// FunctionDSGraph - The graph that represents a method.
//
class FunctionDSGraph {
  Function *Func;
  std::vector<DSNode*> Nodes;
  std::vector<ShadowDSNode*> ShadowNodes;
  PointerValSet RetNode;             // Node that gets returned...
  std::map<Value*, PointerValSet> ValueMap;

  // cloneFunctionIntoSelf - Clone the specified method graph into the current
  // method graph, returning the Return's set of the graph.  If ValueMap is set
  // to true, the ValueMap of the function is cloned into this function as well
  // as the data structure graph itself.
  //
  PointerValSet cloneFunctionIntoSelf(const FunctionDSGraph &G, bool ValueMap);
  void RemoveUnreachableShadowNodes();
  void UnlinkUndistinguishableShadowNodes();
public:
  FunctionDSGraph(Function *F);
  FunctionDSGraph(const FunctionDSGraph &DSG);
  ~FunctionDSGraph();

  void computeClosure(const DataStructure &DS);

  Function *getFunction() const { return Func; }

  void printFunction(std::ostream &O, const char *Label) const;
};


// FIXME: This should be a FunctionPass.  When the pass framework sees a 'Pass'
// that uses the output of a FunctionPass, it should automatically build a map
// of output from the method pass that the pass can use.
//
class DataStructure : public Pass {
  // DSInfo, one intraprocedural and one closed graph for each method...
  typedef std::map<Function*, std::pair<FunctionDSGraph*,
                                        FunctionDSGraph*> > InfoMap;
  mutable InfoMap DSInfo;
public:
  static AnalysisID ID;            // DataStructure Analysis ID 

  DataStructure(AnalysisID id) { assert(id == ID); }
  ~DataStructure() { releaseMemory(); }

  // run - Do nothing, because methods are analyzed lazily
  virtual bool run(Module *TheModule) { return false; }

  // getDSGraph - Return the data structure graph for the specified method.
  // Since method graphs are lazily computed, we may have to create one on the
  // fly here.
  //
  FunctionDSGraph &getDSGraph(Function *F) const {
    std::pair<FunctionDSGraph*, FunctionDSGraph*> &N = DSInfo[F];
    if (N.first) return *N.first;
    return *(N.first = new FunctionDSGraph(F));
  }

  // getClosedDSGraph - Return the data structure graph for the specified
  // method. Since method graphs are lazily computed, we may have to create one
  // on the fly here. This is different than the normal DSGraph for the method
  // because any function calls that are resolvable will have the data structure
  // graphs of the called function incorporated into this function as well.
  //
  FunctionDSGraph &getClosedDSGraph(Function *F) const {
    std::pair<FunctionDSGraph*, FunctionDSGraph*> &N = DSInfo[F];
    if (N.second) return *N.second;
    N.second = new FunctionDSGraph(getDSGraph(F));
    N.second->computeClosure(*this);
    return *N.second;
  }

  // print - Print out the analysis results...
  void print(std::ostream &O, Module *M) const;

  // If the pass pipeline is done with this pass, we can release our memory...
  virtual void releaseMemory();

  // getAnalysisUsageInfo - This obviously provides a call graph
  virtual void getAnalysisUsageInfo(AnalysisSet &Required,
                                    AnalysisSet &Destroyed,
                                    AnalysisSet &Provided) {
    Provided.push_back(ID);
  }
};

#endif
