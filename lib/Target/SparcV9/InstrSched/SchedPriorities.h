// -*-C++-*-
//***************************************************************************
// File:
//	SchedPriorities.h
// 
// Purpose:
//	Encapsulate heuristics for instruction scheduling.
// 
// Strategy:
//    Priority ordering rules:
//    (1) Max delay, which is the order of the heap S.candsAsHeap.
//    (2) Instruction that frees up a register.
//    (3) Instruction that has the maximum number of dependent instructions.
//    Note that rules 2 and 3 are only used if issue conflicts prevent
//    choosing a higher priority instruction by rule 1.
// 
// History:
//	7/30/01	 -  Vikram Adve  -  Created
//**************************************************************************/

#ifndef LLVM_CODEGEN_SCHEDPRIORITIES_H
#define LLVM_CODEGEN_SCHEDPRIORITIES_H

#include "SchedGraph.h"
#include "llvm/CodeGen/InstrScheduling.h"
#include "llvm/Target/MachineSchedInfo.h"
#include "Support/CommandLine.h"
#include <list>
#include <ext/hash_set>
#include <iostream>
class Method;
class MachineInstr;
class SchedulingManager;
class MethodLiveVarInfo;

//---------------------------------------------------------------------------
// Debug option levels for instruction scheduling

enum SchedDebugLevel_t {
  Sched_NoDebugInfo,
  Sched_PrintMachineCode, 
  Sched_PrintSchedTrace,
  Sched_PrintSchedGraphs,
};

extern cl::Enum<SchedDebugLevel_t> SchedDebugLevel;

//---------------------------------------------------------------------------
// Function: instrIsFeasible
// 
// Purpose:
//   Used by the priority analysis to filter out instructions
//   that are not feasible to issue in the current cycle.
//   Should only be used during schedule construction..
//---------------------------------------------------------------------------

bool instrIsFeasible(const SchedulingManager &S, MachineOpCode opCode);



struct NodeDelayPair {
  const SchedGraphNode* node;
  cycles_t delay;
  NodeDelayPair(const SchedGraphNode* n, cycles_t d) :  node(n), delay(d) {}
  inline bool operator<(const NodeDelayPair& np) { return delay < np.delay; }
};

inline bool
NDPLessThan(const NodeDelayPair* np1, const NodeDelayPair* np2)
{
  return np1->delay < np2->delay;
}

class NodeHeap: public std::list<NodeDelayPair*>, public NonCopyable {
public:
  typedef std::list<NodeDelayPair*>::iterator iterator;
  typedef std::list<NodeDelayPair*>::const_iterator const_iterator;
  
public:
  NodeHeap() : _size(0) {}
  
  inline unsigned       size() const { return _size; }
  
  const SchedGraphNode* getNode	(const_iterator i) const { return (*i)->node; }
  cycles_t		getDelay(const_iterator i) const { return (*i)->delay;}
  
  inline void		makeHeap() { 
    // make_heap(begin(), end(), NDPLessThan);
  }
  
  inline iterator	findNode(const SchedGraphNode* node) {
    for (iterator I=begin(); I != end(); ++I)
      if (getNode(I) == node)
	return I;
    return end();
  }
  
  inline void	  removeNode	(const SchedGraphNode* node) {
    iterator ndpPtr = findNode(node);
    if (ndpPtr != end())
      {
	delete *ndpPtr;
	erase(ndpPtr);
	--_size;
      }
  };
  
  void		  insert(const SchedGraphNode* node, cycles_t delay) {
    NodeDelayPair* ndp = new NodeDelayPair(node, delay);
    if (_size == 0 || front()->delay < delay)
      push_front(ndp);
    else
      {
	iterator I=begin();
	for ( ; I != end() && getDelay(I) >= delay; ++I)
	  ;
	std::list<NodeDelayPair*>::insert(I, ndp);
      }
    _size++;
  }
private:
  unsigned int _size;
};


class SchedPriorities: public NonCopyable {
public:
  SchedPriorities(const Method *M, const SchedGraph *G, MethodLiveVarInfo &LVI);
                  
  
  // This must be called before scheduling begins.
  void		initialize		();
  
  cycles_t	getTime			() const { return curTime; }
  cycles_t	getEarliestReadyTime	() const { return earliestReadyTime; }
  unsigned	getNumReady		() const { return candsAsHeap.size(); }
  bool		nodeIsReady		(const SchedGraphNode* node) const {
    return (candsAsSet.find(node) != candsAsSet.end());
  }
  
  void		issuedReadyNodeAt	(cycles_t curTime,
					 const SchedGraphNode* node);
  
  void		insertReady		(const SchedGraphNode* node);
  
  void		updateTime		(cycles_t /*unused*/);
  
  const SchedGraphNode* getNextHighest	(const SchedulingManager& S,
					 cycles_t curTime);
					// choose next highest priority instr
  
private:
  typedef NodeHeap::iterator candIndex;
  
private:
  cycles_t curTime;
  const SchedGraph* graph;
  MethodLiveVarInfo &methodLiveVarInfo;
  std::hash_map<const MachineInstr*, bool> lastUseMap;
  std::vector<cycles_t> nodeDelayVec;
  std::vector<cycles_t> earliestForNode;
  cycles_t earliestReadyTime;
  NodeHeap candsAsHeap;				// candidate nodes, ready to go
  std::hash_set<const SchedGraphNode*> candsAsSet;//same entries as candsAsHeap,
						//   but as set for fast lookup
  std::vector<candIndex> mcands;                // holds pointers into cands
  candIndex nextToTry;				// next cand after the last
						//   one tried in this cycle
  
  int		chooseByRule1		(std::vector<candIndex>& mcands);
  int		chooseByRule2		(std::vector<candIndex>& mcands);
  int		chooseByRule3		(std::vector<candIndex>& mcands);
  
  void		findSetWithMaxDelay	(std::vector<candIndex>& mcands,
					 const SchedulingManager& S);
  
  void		computeDelays		(const SchedGraph* graph);
  
  void		initializeReadyHeap	(const SchedGraph* graph);
  
  bool		instructionHasLastUse	(MethodLiveVarInfo& methodLiveVarInfo,
					 const SchedGraphNode* graphNode);
  
  // NOTE: The next two return references to the actual vector entries.
  //       Use with care.
  cycles_t&	getNodeDelayRef		(const SchedGraphNode* node) {
    assert(node->getNodeId() < nodeDelayVec.size());
    return nodeDelayVec[node->getNodeId()];
  }
  cycles_t&	getEarliestForNodeRef	(const SchedGraphNode* node) {
    assert(node->getNodeId() < earliestForNode.size());
    return earliestForNode[node->getNodeId()];
  }
};


inline void SchedPriorities::updateTime(cycles_t c) {
  curTime = c;
  nextToTry = candsAsHeap.begin();
  mcands.clear();
}

inline std::ostream &operator<<(std::ostream &os, const NodeDelayPair* nd) {
  return os << "Delay for node " << nd->node->getNodeId()
	    << " = " << (long)nd->delay << "\n";
}

#endif
