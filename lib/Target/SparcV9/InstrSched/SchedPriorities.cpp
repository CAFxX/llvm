// $Id$ -*-C++-*-
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

#include "SchedPriorities.h"
#include "Support/PostOrderIterator.h"


SchedPriorities::SchedPriorities(const Method* method,
				 const SchedGraph* _graph)
  : curTime(0),
    graph(_graph),
    methodLiveVarInfo(method),				 // expensive!
    lastUseMap(),
    nodeDelayVec(_graph->getNumNodes(),INVALID_LATENCY), //make errors obvious
    earliestForNode(_graph->getNumNodes(), 0),
    earliestReadyTime(0),
    candsAsHeap(),
    candsAsSet(),
    mcands(),
    nextToTry(candsAsHeap.begin())
{
  methodLiveVarInfo.analyze();
  computeDelays(graph);
}


void
SchedPriorities::initialize()
{
  initializeReadyHeap(graph);
}


void
SchedPriorities::computeDelays(const SchedGraph* graph)
{
  po_iterator<const SchedGraph*> poIter = po_begin(graph), poEnd =po_end(graph);
  for ( ; poIter != poEnd; ++poIter)
    {
      const SchedGraphNode* node = *poIter;
      cycles_t nodeDelay;
      if (node->beginOutEdges() == node->endOutEdges())
	nodeDelay = node->getLatency();
      else
	{
	  // Iterate over the out-edges of the node to compute delay
	  nodeDelay = 0;
	  for (SchedGraphNode::const_iterator E=node->beginOutEdges();
	       E != node->endOutEdges(); ++E)
	    {
	      cycles_t sinkDelay = getNodeDelayRef((*E)->getSink());
	      nodeDelay = max(nodeDelay, sinkDelay + (*E)->getMinDelay());
	    }
	}
      getNodeDelayRef(node) = nodeDelay;
    }
}


void
SchedPriorities::initializeReadyHeap(const SchedGraph* graph)
{
  const SchedGraphNode* graphRoot = graph->getRoot();
  assert(graphRoot->getMachineInstr() == NULL && "Expect dummy root");
  
  // Insert immediate successors of dummy root, which are the actual roots
  sg_succ_const_iterator SEnd = succ_end(graphRoot);
  for (sg_succ_const_iterator S = succ_begin(graphRoot); S != SEnd; ++S)
    this->insertReady(*S);
  
#undef TEST_HEAP_CONVERSION
#ifdef TEST_HEAP_CONVERSION
  cout << "Before heap conversion:" << endl;
  copy(candsAsHeap.begin(), candsAsHeap.end(),
       ostream_iterator<NodeDelayPair*>(cout,"\n"));
#endif
  
  candsAsHeap.makeHeap();
  
#ifdef TEST_HEAP_CONVERSION
  cout << "After heap conversion:" << endl;
  copy(candsAsHeap.begin(), candsAsHeap.end(),
       ostream_iterator<NodeDelayPair*>(cout,"\n"));
#endif
}


void
SchedPriorities::issuedReadyNodeAt(cycles_t curTime,
				   const SchedGraphNode* node)
{
  candsAsHeap.removeNode(node);
  candsAsSet.erase(node);
  mcands.clear(); // ensure reset choices is called before any more choices
  
  if (earliestReadyTime == getEarliestForNodeRef(node))
    {// earliestReadyTime may have been due to this node, so recompute it
      earliestReadyTime = HUGE_LATENCY;
      for (NodeHeap::const_iterator I=candsAsHeap.begin();
	   I != candsAsHeap.end(); ++I)
	if (candsAsHeap.getNode(I))
	  earliestReadyTime = min(earliestReadyTime, 
				getEarliestForNodeRef(candsAsHeap.getNode(I)));
    }
  
  // Now update ready times for successors
  for (SchedGraphNode::const_iterator E=node->beginOutEdges();
       E != node->endOutEdges(); ++E)
    {
      cycles_t& etime = getEarliestForNodeRef((*E)->getSink());
      etime = max(etime, curTime + (*E)->getMinDelay());
    }    
}


//----------------------------------------------------------------------
// Priority ordering rules:
// (1) Max delay, which is the order of the heap S.candsAsHeap.
// (2) Instruction that frees up a register.
// (3) Instruction that has the maximum number of dependent instructions.
// Note that rules 2 and 3 are only used if issue conflicts prevent
// choosing a higher priority instruction by rule 1.
//----------------------------------------------------------------------

inline int
SchedPriorities::chooseByRule1(vector<candIndex>& mcands)
{
  return (mcands.size() == 1)? 0	// only one choice exists so take it
			     : -1;	// -1 indicates multiple choices
}

inline int
SchedPriorities::chooseByRule2(vector<candIndex>& mcands)
{
  assert(mcands.size() >= 1 && "Should have at least one candidate here.");
  for (unsigned i=0, N = mcands.size(); i < N; i++)
    if (instructionHasLastUse(methodLiveVarInfo,
			      candsAsHeap.getNode(mcands[i])))
      return i;
  return -1;
}

inline int
SchedPriorities::chooseByRule3(vector<candIndex>& mcands)
{
  assert(mcands.size() >= 1 && "Should have at least one candidate here.");
  int maxUses = candsAsHeap.getNode(mcands[0])->getNumOutEdges();	
  int indexWithMaxUses = 0;
  for (unsigned i=1, N = mcands.size(); i < N; i++)
    {
      int numUses = candsAsHeap.getNode(mcands[i])->getNumOutEdges();
      if (numUses > maxUses)
	{
	  maxUses = numUses;
	  indexWithMaxUses = i;
	}
    }
  return indexWithMaxUses; 
}

const SchedGraphNode*
SchedPriorities::getNextHighest(const SchedulingManager& S,
				cycles_t curTime)
{
  int nextIdx = -1;
  const SchedGraphNode* nextChoice = NULL;
  
  if (mcands.size() == 0)
    findSetWithMaxDelay(mcands, S);
  
  while (nextIdx < 0 && mcands.size() > 0)
    {
      nextIdx = chooseByRule1(mcands);	 // rule 1
      
      if (nextIdx == -1)
	nextIdx = chooseByRule2(mcands); // rule 2
      
      if (nextIdx == -1)
	nextIdx = chooseByRule3(mcands); // rule 3
      
      if (nextIdx == -1)
	nextIdx = 0;			 // default to first choice by delays
      
      // We have found the next best candidate.  Check if it ready in
      // the current cycle, and if it is feasible.
      // If not, remove it from mcands and continue.  Refill mcands if
      // it becomes empty.
      nextChoice = candsAsHeap.getNode(mcands[nextIdx]);
      if (getEarliestForNodeRef(nextChoice) > curTime
	  || ! instrIsFeasible(S, nextChoice->getMachineInstr()->getOpCode()))
	{
	  mcands.erase(mcands.begin() + nextIdx);
	  nextIdx = -1;
	  if (mcands.size() == 0)
	    findSetWithMaxDelay(mcands, S);
	}
    }
  
  if (nextIdx >= 0)
    {
      mcands.erase(mcands.begin() + nextIdx);
      return nextChoice;
    }
  else
    return NULL;
}


void
SchedPriorities::findSetWithMaxDelay(vector<candIndex>& mcands,
				     const SchedulingManager& S)
{
  if (mcands.size() == 0 && nextToTry != candsAsHeap.end())
    { // out of choices at current maximum delay;
      // put nodes with next highest delay in mcands
      candIndex next = nextToTry;
      cycles_t maxDelay = candsAsHeap.getDelay(next);
      for (; next != candsAsHeap.end()
	     && candsAsHeap.getDelay(next) == maxDelay; ++next)
	mcands.push_back(next);
      
      nextToTry = next;
      
      if (SchedDebugLevel >= Sched_PrintSchedTrace)
	{
	  cout << "    Cycle " << this->getTime() << ": "
	       << "Next highest delay = " << maxDelay << " : "
	       << mcands.size() << " Nodes with this delay: ";
	  for (unsigned i=0; i < mcands.size(); i++)
	    cout << candsAsHeap.getNode(mcands[i])->getNodeId() << ", ";
	  cout << endl;
	}
    }
}


bool
SchedPriorities::instructionHasLastUse(MethodLiveVarInfo& methodLiveVarInfo,
				       const SchedGraphNode* graphNode)
{
  const MachineInstr* minstr = graphNode->getMachineInstr();
  
  hash_map<const MachineInstr*, bool>::const_iterator
    ui = lastUseMap.find(minstr);
  if (ui != lastUseMap.end())
    return (*ui).second;
  
  // else check if instruction is a last use and save it in the hash_map
  bool hasLastUse = false;
  const BasicBlock* bb = graphNode->getBB();
  const LiveVarSet* liveVars =
    methodLiveVarInfo.getLiveVarSetBeforeMInst(minstr, bb);
  
  for (MachineInstr::val_op_const_iterator vo(minstr); ! vo.done(); ++vo)
    if (liveVars->find(*vo) == liveVars->end())
      {
	hasLastUse = true;
	break;
      }
  
  lastUseMap[minstr] = hasLastUse;
  return hasLastUse;
}

