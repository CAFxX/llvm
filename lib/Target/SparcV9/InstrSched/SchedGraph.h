/* -*-C++-*-
 ****************************************************************************
 * File:
 *	SchedGraph.h
 * 
 * Purpose:
 *	Scheduling graph based on SSA graph plus extra dependence edges
 *	capturing dependences due to machine resources (machine registers,
 *	CC registers, and any others).
 * 
 * Strategy:
 *	This graph tries to leverage the SSA graph as much as possible,
 *	but captures the extra dependences through a common interface.
 * 
 * History:
 *	7/20/01	 -  Vikram Adve  -  Created
 ***************************************************************************/

#ifndef LLVM_CODEGEN_SCHEDGRAPH_H
#define LLVM_CODEGEN_SCHEDGRAPH_H

#include "llvm/CodeGen/MachineInstr.h"
#include "Support/NonCopyable.h"
#include "Support/HashExtras.h"
#include "Support/GraphTraits.h"

class Value;
class Instruction;
class TerminatorInst;
class BasicBlock;
class Method;
class TargetMachine;
class SchedGraphEdge; 
class SchedGraphNode; 
class SchedGraph; 
class RegToRefVecMap;
class ValueToDefVecMap;
class RefVec;
class MachineInstr;
class MachineCodeForBasicBlock;


/******************** Exported Data Types and Constants ********************/

typedef int ResourceId;
const ResourceId InvalidRID        = -1;
const ResourceId MachineCCRegsRID  = -2; // use +ve numbers for actual regs
const ResourceId MachineIntRegsRID = -3; // use +ve numbers for actual regs
const ResourceId MachineFPRegsRID  = -4; // use +ve numbers for actual regs


//*********************** Public Class Declarations ************************/

class SchedGraphEdge: public NonCopyable {
public:
  enum SchedGraphEdgeDepType {
    CtrlDep, MemoryDep, ValueDep, MachineRegister, MachineResource
  };
  enum DataDepOrderType {
    TrueDep = 0x1, AntiDep=0x2, OutputDep=0x4, NonDataDep=0x8
  };
  
protected:
  SchedGraphNode*	src;
  SchedGraphNode*	sink;
  SchedGraphEdgeDepType depType;
  unsigned int          depOrderType;
  int			minDelay; // cached latency (assumes fixed target arch)
  
  union {
    const Value* val;
    int          machineRegNum;
    ResourceId   resourceId;
  };
  
public:	
  // For all constructors, if minDelay is unspecified, minDelay is
  // set to _src->getLatency().
  // constructor for CtrlDep or MemoryDep edges, selected by 3rd argument
  /*ctor*/		SchedGraphEdge(SchedGraphNode* _src,
				       SchedGraphNode* _sink,
				       SchedGraphEdgeDepType _depType,
				       unsigned int     _depOrderType,
				       int _minDelay = -1);
  
  // constructor for explicit value dependence (may be true/anti/output)
  /*ctor*/		SchedGraphEdge(SchedGraphNode* _src,
				       SchedGraphNode* _sink,
				       const Value*    _val,
				       unsigned int     _depOrderType,
				       int _minDelay = -1);
  
  // constructor for machine register dependence
  /*ctor*/		SchedGraphEdge(SchedGraphNode* _src,
				       SchedGraphNode* _sink,
				       unsigned int    _regNum,
				       unsigned int     _depOrderType,
				       int _minDelay = -1);
  
  // constructor for any other machine resource dependences.
  // DataDepOrderType is always NonDataDep.  It it not an argument to
  // avoid overloading ambiguity with previous constructor.
  /*ctor*/		SchedGraphEdge(SchedGraphNode* _src,
				       SchedGraphNode* _sink,
				       ResourceId      _resourceId,
				       int _minDelay = -1);
  
  /*dtor*/		~SchedGraphEdge();
  
  SchedGraphNode*	getSrc		() const { return src; }
  SchedGraphNode*	getSink		() const { return sink; }
  int			getMinDelay	() const { return minDelay; }
  SchedGraphEdgeDepType getDepType	() const { return depType; }
  
  const Value*		getValue	() const {
    assert(depType == ValueDep); return val;
  }
  int			getMachineReg	() const {
    assert(depType == MachineRegister); return machineRegNum;
  }
  int			getResourceId	() const {
    assert(depType == MachineResource); return resourceId;
  }
  
public:
  // 
  // Debugging support
  // 
  friend std::ostream& operator<<(std::ostream& os, const SchedGraphEdge& edge);
  
  void		dump	(int indent=0) const;
    
private:
  // disable default ctor
  /*ctor*/		SchedGraphEdge();	// DO NOT IMPLEMENT
};



class SchedGraphNode: public NonCopyable {
private:
  unsigned int nodeId;
  const BasicBlock* bb;
  const MachineInstr* minstr;
  std::vector<SchedGraphEdge*> inEdges;
  std::vector<SchedGraphEdge*> outEdges;
  int origIndexInBB;            // original position of machine instr in BB
  int latency;
  
public:
  typedef std::vector<SchedGraphEdge*>::      iterator	       iterator;
  typedef std::vector<SchedGraphEdge*>::const_iterator         const_iterator;
  typedef std::vector<SchedGraphEdge*>::      reverse_iterator reverse_iterator;
  typedef std::vector<SchedGraphEdge*>::const_reverse_iterator const_reverse_iterator;
  
public:
  //
  // Accessor methods
  // 
  unsigned int		getNodeId	() const { return nodeId; }
  const MachineInstr*	getMachineInstr	() const { return minstr; }
  const MachineOpCode	getOpCode	() const { return minstr->getOpCode();}
  int			getLatency	() const { return latency; }
  unsigned int		getNumInEdges	() const { return inEdges.size(); }
  unsigned int		getNumOutEdges	() const { return outEdges.size(); }
  bool			isDummyNode	() const { return (minstr == NULL); }
  const BasicBlock*	getBB           () const { return bb; }
  int                   getOrigIndexInBB() const { return origIndexInBB; }
  
  //
  // Iterators
  // 
  iterator		beginInEdges	()	 { return inEdges.begin(); }
  iterator		endInEdges	()	 { return inEdges.end(); }
  iterator		beginOutEdges	()	 { return outEdges.begin(); }
  iterator		endOutEdges	()	 { return outEdges.end(); }
  
  const_iterator	beginInEdges	() const { return inEdges.begin(); }
  const_iterator	endInEdges	() const { return inEdges.end(); }
  const_iterator	beginOutEdges	() const { return outEdges.begin(); }
  const_iterator	endOutEdges	() const { return outEdges.end(); }
  
public:
  //
  // Debugging support
  // 
  friend std::ostream& operator<<(std::ostream& os, const SchedGraphNode& node);
  
  void		dump	(int indent=0) const;
  
private:
  friend class SchedGraph;		// give access for ctor and dtor
  friend class SchedGraphEdge;		// give access for adding edges
  
  void			addInEdge	(SchedGraphEdge* edge);
  void			addOutEdge	(SchedGraphEdge* edge);
  
  void			removeInEdge	(const SchedGraphEdge* edge);
  void			removeOutEdge	(const SchedGraphEdge* edge);
  
  // disable default constructor and provide a ctor for single-block graphs
  /*ctor*/		SchedGraphNode();	// DO NOT IMPLEMENT
  /*ctor*/		SchedGraphNode	(unsigned int _nodeId,
					 const BasicBlock*   _bb,
					 const MachineInstr* _minstr,
                                         int   indexInBB,
					 const TargetMachine& _target);
  /*dtor*/		~SchedGraphNode	();
};



class SchedGraph :
  public NonCopyable,
  private std::hash_map<const MachineInstr*, SchedGraphNode*>
{
private:
  std::vector<const BasicBlock*> bbVec; // basic blocks included in the graph
  SchedGraphNode* graphRoot;		// the root and leaf are not inserted
  SchedGraphNode* graphLeaf;		//  in the hash_map (see getNumNodes())
  
  typedef std::hash_map<const MachineInstr*, SchedGraphNode*> map_base;
public:
  using map_base::iterator;
  using map_base::const_iterator;
  
public:
  //
  // Accessor methods
  //
  const std::vector<const BasicBlock*>& getBasicBlocks() const { return bbVec; }
  const unsigned int		   getNumNodes()    const { return size()+2; }
  SchedGraphNode*		   getRoot()	    const { return graphRoot; }
  SchedGraphNode*		   getLeaf()	    const { return graphLeaf; }
  
  SchedGraphNode* getGraphNodeForInstr(const MachineInstr* minstr) const {
    const_iterator onePair = this->find(minstr);
    return (onePair != this->end())? (*onePair).second : NULL;
  }
  
  //
  // Delete nodes or edges from the graph.
  // 
  void		eraseNode		(SchedGraphNode* node);
  
  void		eraseIncomingEdges	(SchedGraphNode* node,
					 bool addDummyEdges = true);
  
  void		eraseOutgoingEdges	(SchedGraphNode* node,
					 bool addDummyEdges = true);
  
  void		eraseIncidentEdges	(SchedGraphNode* node,
					 bool addDummyEdges = true);
  
  //
  // Unordered iterators.
  // Return values is pair<const MachineIntr*,SchedGraphNode*>.
  //
  using map_base::begin;
  using map_base::end;

  //
  // Ordered iterators.
  // Return values is pair<const MachineIntr*,SchedGraphNode*>.
  //
  // void			   postord_init();
  // postorder_iterator	   postord_begin();
  // postorder_iterator	   postord_end();
  // const_postorder_iterator postord_begin() const;
  // const_postorder_iterator postord_end() const;
  
  //
  // Debugging support
  // 
  void		dump	() const;
  
private:
  friend class SchedGraphSet;		// give access to ctor
  
  // disable default constructor and provide a ctor for single-block graphs
  /*ctor*/	SchedGraph		();	// DO NOT IMPLEMENT
  /*ctor*/	SchedGraph		(const BasicBlock* bb,
					 const TargetMachine& target);
  /*dtor*/	~SchedGraph		();
  
  inline void	noteGraphNodeForInstr	(const MachineInstr* minstr,
					 SchedGraphNode* node)
  {
    assert((*this)[minstr] == NULL);
    (*this)[minstr] = node;
  }

  //
  // Graph builder
  //
  void  	buildGraph		(const TargetMachine& target);
  
  void          buildNodesforBB         (const TargetMachine& target,
                                         const BasicBlock* bb,
                                         std::vector<SchedGraphNode*>& memNod,
                                         RegToRefVecMap& regToRefVecMap,
                                         ValueToDefVecMap& valueToDefVecMap);
  
  void          findDefUseInfoAtInstr   (const TargetMachine& target,
                                         SchedGraphNode* node,
                                         std::vector<SchedGraphNode*>& memNode,
                                         RegToRefVecMap& regToRefVecMap,
                                         ValueToDefVecMap& valueToDefVecMap);
                                         
  void		addEdgesForInstruction(const MachineInstr& minstr,
                                     const ValueToDefVecMap& valueToDefVecMap,
                                     const TargetMachine& target);
  
  void		addCDEdges		(const TerminatorInst* term,
					 const TargetMachine& target);
  
  void		addMemEdges         (const std::vector<SchedGraphNode*>& memNod,
                                     const TargetMachine& target);
  
  void          addCallCCEdges      (const std::vector<SchedGraphNode*>& memNod,
                                     MachineCodeForBasicBlock& bbMvec,
                                     const TargetMachine& target);
    
  void		addMachineRegEdges	(RegToRefVecMap& regToRefVecMap,
					 const TargetMachine& target);
  
  void		addEdgesForValue	(SchedGraphNode* refNode,
                                         const RefVec& defVec,
                                         const Value* defValue,
                                         bool  refNodeIsDef,
					 const TargetMachine& target);
  
  void		addDummyEdges		();
};


class SchedGraphSet :  
  public NonCopyable,
  private std::hash_map<const BasicBlock*, SchedGraph*>
{
private:
  const Method* method;
  
public:
  typedef std::hash_map<const BasicBlock*, SchedGraph*> map_base;
  using map_base::iterator;
  using map_base::const_iterator;
  
public:
  /*ctor*/	SchedGraphSet		(const Method* _method,
					 const TargetMachine& target);
  /*dtor*/	~SchedGraphSet		();
  
  //
  // Accessors
  //
  SchedGraph*	getGraphForBasicBlock	(const BasicBlock* bb) const {
    const_iterator onePair = this->find(bb);
    return (onePair != this->end())? (*onePair).second : NULL;
  }
  
  //
  // Iterators
  //
  using map_base::begin;
  using map_base::end;
  
  //
  // Debugging support
  // 
  void		dump	() const;
  
private:
  inline void	noteGraphForBlock(const BasicBlock* bb, SchedGraph* graph) {
    assert((*this)[bb] == NULL);
    (*this)[bb] = graph;
  }

  //
  // Graph builder
  //
  void		buildGraphsForMethod	(const Method *method,
					 const TargetMachine& target);
};


//********************** Sched Graph Iterators *****************************/

// Ok to make it a template because it shd get instantiated at most twice:
// for <SchedGraphNode, SchedGraphNode::iterator> and
// for <const SchedGraphNode, SchedGraphNode::const_iterator>.
// 
template <class _NodeType, class _EdgeType, class _EdgeIter>
class PredIterator: public std::bidirectional_iterator<_NodeType, ptrdiff_t> {
protected:
  _EdgeIter oi;
public:
  typedef PredIterator<_NodeType, _EdgeType, _EdgeIter> _Self;
  
  inline PredIterator(_EdgeIter startEdge) : oi(startEdge) {}
  
  inline bool operator==(const _Self& x) const { return oi == x.oi; }
  inline bool operator!=(const _Self& x) const { return !operator==(x); }
  
  // operator*() differs for pred or succ iterator
  inline _NodeType* operator*() const { return (*oi)->getSrc(); }
  inline	 _NodeType* operator->() const { return operator*(); }
  
  inline _EdgeType* getEdge() const { return *(oi); }
  
  inline _Self &operator++() { ++oi; return *this; }    // Preincrement
  inline _Self operator++(int) {                      	// Postincrement
    _Self tmp(*this); ++*this; return tmp; 
  }
  
  inline _Self &operator--() { --oi; return *this; }    // Predecrement
  inline _Self operator--(int) {                       	// Postdecrement
    _Self tmp = *this; --*this; return tmp;
  }
};

template <class _NodeType, class _EdgeType, class _EdgeIter>
class SuccIterator: public std::bidirectional_iterator<_NodeType, ptrdiff_t> {
protected:
  _EdgeIter oi;
public:
  typedef SuccIterator<_NodeType, _EdgeType, _EdgeIter> _Self;
  
  inline SuccIterator(_EdgeIter startEdge) : oi(startEdge) {}
  
  inline bool operator==(const _Self& x) const { return oi == x.oi; }
  inline bool operator!=(const _Self& x) const { return !operator==(x); }
  
  inline _NodeType* operator*() const { return (*oi)->getSink(); }
  inline	 _NodeType* operator->() const { return operator*(); }
  
  inline _EdgeType* getEdge() const { return *(oi); }
  
  inline _Self &operator++() { ++oi; return *this; }    // Preincrement
  inline _Self operator++(int) {                      	// Postincrement
    _Self tmp(*this); ++*this; return tmp; 
  }
  
  inline _Self &operator--() { --oi; return *this; }    // Predecrement
  inline _Self operator--(int) {                       	// Postdecrement
    _Self tmp = *this; --*this; return tmp;
  }
};

// 
// sg_pred_iterator
// sg_pred_const_iterator
//
typedef PredIterator<SchedGraphNode, SchedGraphEdge, SchedGraphNode::iterator>
    sg_pred_iterator;
typedef PredIterator<const SchedGraphNode, const SchedGraphEdge,SchedGraphNode::const_iterator>
    sg_pred_const_iterator;

inline sg_pred_iterator       pred_begin(      SchedGraphNode *N) {
  return sg_pred_iterator(N->beginInEdges());
}
inline sg_pred_iterator       pred_end(        SchedGraphNode *N) {
  return sg_pred_iterator(N->endInEdges());
}
inline sg_pred_const_iterator pred_begin(const SchedGraphNode *N) {
  return sg_pred_const_iterator(N->beginInEdges());
}
inline sg_pred_const_iterator pred_end(  const SchedGraphNode *N) {
  return sg_pred_const_iterator(N->endInEdges());
}


// 
// sg_succ_iterator
// sg_succ_const_iterator
//
typedef SuccIterator<SchedGraphNode, SchedGraphEdge, SchedGraphNode::iterator>
    sg_succ_iterator;
typedef SuccIterator<const SchedGraphNode, const SchedGraphEdge,SchedGraphNode::const_iterator>
    sg_succ_const_iterator;

inline sg_succ_iterator       succ_begin(      SchedGraphNode *N) {
  return sg_succ_iterator(N->beginOutEdges());
}
inline sg_succ_iterator       succ_end(        SchedGraphNode *N) {
  return sg_succ_iterator(N->endOutEdges());
}
inline sg_succ_const_iterator succ_begin(const SchedGraphNode *N) {
  return sg_succ_const_iterator(N->beginOutEdges());
}
inline sg_succ_const_iterator succ_end(  const SchedGraphNode *N) {
  return sg_succ_const_iterator(N->endOutEdges());
}

// Provide specializations of GraphTraits to be able to use graph iterators on
// the scheduling graph!
//
template <> struct GraphTraits<SchedGraph*> {
  typedef SchedGraphNode NodeType;
  typedef sg_succ_iterator ChildIteratorType;

  static inline NodeType *getEntryNode(SchedGraph *SG) { return SG->getRoot(); }
  static inline ChildIteratorType child_begin(NodeType *N) { 
    return succ_begin(N); 
  }
  static inline ChildIteratorType child_end(NodeType *N) { 
    return succ_end(N);
  }
};

template <> struct GraphTraits<const SchedGraph*> {
  typedef const SchedGraphNode NodeType;
  typedef sg_succ_const_iterator ChildIteratorType;

  static inline NodeType *getEntryNode(const SchedGraph *SG) {
    return SG->getRoot();
  }
  static inline ChildIteratorType child_begin(NodeType *N) { 
    return succ_begin(N); 
  }
  static inline ChildIteratorType child_end(NodeType *N) { 
    return succ_end(N);
  }
};


std::ostream &operator<<(std::ostream& os, const SchedGraphEdge& edge);
std::ostream &operator<<(std::ostream &os, const SchedGraphNode& node);

#endif
