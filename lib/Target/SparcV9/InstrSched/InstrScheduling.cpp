//***************************************************************************
// File:
//	InstrScheduling.cpp
// 
// Purpose:
//	
// History:
//	7/23/01	 -  Vikram Adve  -  Created
//***************************************************************************

#include "llvm/CodeGen/InstrScheduling.h"
#include "SchedPriorities.h"
#include "llvm/Analysis/LiveVar/BBLiveVar.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Instruction.h"
#include <hash_set>
#include <algorithm>
#include <iterator>

cl::Enum<enum SchedDebugLevel_t> SchedDebugLevel("dsched", cl::NoFlags,
  "enable instruction scheduling debugging information",
  clEnumValN(Sched_NoDebugInfo,      "n", "disable debug output"),
  clEnumValN(Sched_PrintMachineCode, "y", "print machine code after scheduling"),
  clEnumValN(Sched_PrintSchedTrace,  "t", "print trace of scheduling actions"),
  clEnumValN(Sched_PrintSchedGraphs, "g", "print scheduling graphs"), 0);


class InstrSchedule;
class SchedulingManager;
class DelaySlotInfo;

static void	ForwardListSchedule	(SchedulingManager& S);

static void	RecordSchedule		(const BasicBlock* bb,
					 const SchedulingManager& S);

static unsigned	ChooseOneGroup		(SchedulingManager& S);

static void	MarkSuccessorsReady	(SchedulingManager& S,
					 const SchedGraphNode* node);

static unsigned	FindSlotChoices		(SchedulingManager& S,
					 DelaySlotInfo*& getDelaySlotInfo);

static void	AssignInstructionsToSlots(class SchedulingManager& S,
					  unsigned maxIssue);

static void	ScheduleInstr		(class SchedulingManager& S,
					 const SchedGraphNode* node,
					 unsigned int slotNum,
					 cycles_t curTime);

static bool	ViolatesMinimumGap	(const SchedulingManager& S,
					 MachineOpCode opCode,
					 const cycles_t inCycle);

static bool	ConflictsWithChoices	(const SchedulingManager& S,
					 MachineOpCode opCode);

static void	ChooseInstructionsForDelaySlots(SchedulingManager& S,
					 const BasicBlock* bb,
					 SchedGraph* graph);

static bool	NodeCanFillDelaySlot	(const SchedulingManager& S,
					 const SchedGraphNode* node,
					 const SchedGraphNode* brNode,
					 bool nodeIsPredecessor);

static void	MarkNodeForDelaySlot	(SchedulingManager& S,
					 SchedGraphNode* node,
					 const SchedGraphNode* brNode,
					 bool nodeIsPredecessor);

//************************* Internal Data Types *****************************/


//----------------------------------------------------------------------
// class InstrGroup:
// 
// Represents a group of instructions scheduled to be issued
// in a single cycle.
//----------------------------------------------------------------------

class InstrGroup: public NonCopyable {
public:
  inline const SchedGraphNode* operator[](unsigned int slotNum) const {
    assert(slotNum  < group.size());
    return group[slotNum];
  }
  
private:
  friend class InstrSchedule;
  
  inline void	addInstr(const SchedGraphNode* node, unsigned int slotNum) {
    assert(slotNum < group.size());
    group[slotNum] = node;
  }
  
  /*ctor*/	InstrGroup(unsigned int nslots)
    : group(nslots, NULL) {}
  
  /*ctor*/	InstrGroup();		// disable: DO NOT IMPLEMENT
  
private:
  vector<const SchedGraphNode*> group;
};


//----------------------------------------------------------------------
// class ScheduleIterator:
// 
// Iterates over the machine instructions in the for a single basic block.
// The schedule is represented by an InstrSchedule object.
//----------------------------------------------------------------------

template<class _NodeType>
class ScheduleIterator: public std::forward_iterator<_NodeType, ptrdiff_t> {
private:
  unsigned cycleNum;
  unsigned slotNum;
  const InstrSchedule& S;
public:
  typedef ScheduleIterator<_NodeType> _Self;
  
  /*ctor*/ inline ScheduleIterator(const InstrSchedule& _schedule,
				   unsigned _cycleNum,
				   unsigned _slotNum)
    : cycleNum(_cycleNum), slotNum(_slotNum), S(_schedule) {
    skipToNextInstr(); 
  }
  
  /*ctor*/ inline ScheduleIterator(const _Self& x)
    : cycleNum(x.cycleNum), slotNum(x.slotNum), S(x.S) {}
  
  inline bool operator==(const _Self& x) const {
    return (slotNum == x.slotNum && cycleNum== x.cycleNum && &S==&x.S);
  }
  
  inline bool operator!=(const _Self& x) const { return !operator==(x); }
  
  inline _NodeType* operator*() const {
    assert(cycleNum < S.groups.size());
    return (*S.groups[cycleNum])[slotNum];
  }
  inline _NodeType* operator->() const { return operator*(); }
  
         _Self& operator++();				// Preincrement
  inline _Self operator++(int) {			// Postincrement
    _Self tmp(*this); ++*this; return tmp; 
  }
  
  static _Self begin(const InstrSchedule& _schedule);
  static _Self end(  const InstrSchedule& _schedule);
  
private:
  inline _Self& operator=(const _Self& x); // DISABLE -- DO NOT IMPLEMENT
  void	skipToNextInstr();
};


//----------------------------------------------------------------------
// class InstrSchedule:
// 
// Represents the schedule of machine instructions for a single basic block.
//----------------------------------------------------------------------

class InstrSchedule: public NonCopyable {
private:
  const unsigned int nslots;
  unsigned int numInstr;
  vector<InstrGroup*> groups;		// indexed by cycle number
  vector<cycles_t> startTime;		// indexed by node id
  
public: // iterators
  typedef ScheduleIterator<SchedGraphNode> iterator;
  typedef ScheduleIterator<const SchedGraphNode> const_iterator;
  
        iterator begin();
  const_iterator begin() const;
        iterator end();
  const_iterator end()   const;
  
public: // constructors and destructor
  /*ctor*/		InstrSchedule	(unsigned int _nslots,
					 unsigned int _numNodes);
  /*dtor*/		~InstrSchedule	();
  
public: // accessor functions to query chosen schedule
  const SchedGraphNode* getInstr	(unsigned int slotNum,
					 cycles_t c) const {
    const InstrGroup* igroup = this->getIGroup(c);
    return (igroup == NULL)? NULL : (*igroup)[slotNum];
  }
  
  inline InstrGroup*	getIGroup	(cycles_t c) {
    if (c >= groups.size())
      groups.resize(c+1);
    if (groups[c] == NULL)
      groups[c] = new InstrGroup(nslots);
    return groups[c];
  }
  
  inline const InstrGroup* getIGroup	(cycles_t c) const {
    assert(c < groups.size());
    return groups[c];
  }
  
  inline cycles_t	getStartTime	(unsigned int nodeId) const {
    assert(nodeId < startTime.size());
    return startTime[nodeId];
  }
  
  unsigned int		getNumInstructions() const {
    return numInstr;
  }
  
  inline void		scheduleInstr	(const SchedGraphNode* node,
					 unsigned int slotNum,
					 cycles_t cycle) {
    InstrGroup* igroup = this->getIGroup(cycle);
    assert((*igroup)[slotNum] == NULL &&  "Slot already filled?");
    igroup->addInstr(node, slotNum);
    assert(node->getNodeId() < startTime.size());
    startTime[node->getNodeId()] = cycle;
    ++numInstr;
  }
  
private:
  friend class iterator;
  friend class const_iterator;
  /*ctor*/	InstrSchedule	();	// Disable: DO NOT IMPLEMENT.
};


/*ctor*/
InstrSchedule::InstrSchedule(unsigned int _nslots, unsigned int _numNodes)
  : nslots(_nslots),
    numInstr(0),
    groups(2 * _numNodes / _nslots),		// 2 x lower-bound for #cycles
    startTime(_numNodes, (cycles_t) -1)		// set all to -1
{
}


/*dtor*/
InstrSchedule::~InstrSchedule()
{
  for (unsigned c=0, NC=groups.size(); c < NC; c++)
    if (groups[c] != NULL)
      delete groups[c];			// delete InstrGroup objects
}


template<class _NodeType>
inline 
void
ScheduleIterator<_NodeType>::skipToNextInstr()
{
  while(cycleNum < S.groups.size() && S.groups[cycleNum] == NULL)
    ++cycleNum;			// skip cycles with no instructions
  
  while (cycleNum < S.groups.size() &&
	 (*S.groups[cycleNum])[slotNum] == NULL)
    {
      ++slotNum;
      if (slotNum == S.nslots)
	{
	  ++cycleNum;
	  slotNum = 0;
	  while(cycleNum < S.groups.size() && S.groups[cycleNum] == NULL)
	    ++cycleNum;			// skip cycles with no instructions
	}
    }
}

template<class _NodeType>
inline 
ScheduleIterator<_NodeType>&
ScheduleIterator<_NodeType>::operator++()	// Preincrement
{
  ++slotNum;
  if (slotNum == S.nslots)
    {
      ++cycleNum;
      slotNum = 0;
    }
  skipToNextInstr(); 
  return *this;
}

template<class _NodeType>
ScheduleIterator<_NodeType>
ScheduleIterator<_NodeType>::begin(const InstrSchedule& _schedule)
{
  return _Self(_schedule, 0, 0);
}

template<class _NodeType>
ScheduleIterator<_NodeType>
ScheduleIterator<_NodeType>::end(const InstrSchedule& _schedule)
{
  return _Self(_schedule, _schedule.groups.size(), 0);
}

InstrSchedule::iterator
InstrSchedule::begin()
{
  return iterator::begin(*this);
}

InstrSchedule::const_iterator
InstrSchedule::begin() const
{
  return const_iterator::begin(*this);
}

InstrSchedule::iterator
InstrSchedule::end()
{
  return iterator::end(*this);
}

InstrSchedule::const_iterator
InstrSchedule::end() const
{
  return const_iterator::end(  *this);
}


//----------------------------------------------------------------------
// class DelaySlotInfo:
// 
// Record information about delay slots for a single branch instruction.
// Delay slots are simply indexed by slot number 1 ... numDelaySlots
//----------------------------------------------------------------------

class DelaySlotInfo: public NonCopyable {
private:
  const SchedGraphNode* brNode;
  unsigned int ndelays;
  vector<const SchedGraphNode*> delayNodeVec;
  cycles_t delayedNodeCycle;
  unsigned int delayedNodeSlotNum;
  
public:
  /*ctor*/	DelaySlotInfo		(const SchedGraphNode* _brNode,
					 unsigned _ndelays)
    : brNode(_brNode), ndelays(_ndelays),
      delayedNodeCycle(0), delayedNodeSlotNum(0) {}
  
  inline unsigned getNumDelays	() {
    return ndelays;
  }
  
  inline const vector<const SchedGraphNode*>& getDelayNodeVec() {
    return delayNodeVec;
  }
  
  inline void	addDelayNode		(const SchedGraphNode* node) {
    delayNodeVec.push_back(node);
    assert(delayNodeVec.size() <= ndelays && "Too many delay slot instrs!");
  }
  
  inline void	recordChosenSlot	(cycles_t cycle, unsigned slotNum) {
    delayedNodeCycle = cycle;
    delayedNodeSlotNum = slotNum;
  }
  
  void		scheduleDelayedNode	(SchedulingManager& S);
};


//----------------------------------------------------------------------
// class SchedulingManager:
// 
// Represents the schedule of machine instructions for a single basic block.
//----------------------------------------------------------------------

class SchedulingManager: public NonCopyable {
public: // publicly accessible data members
  const unsigned int nslots;
  const MachineSchedInfo& schedInfo;
  SchedPriorities& schedPrio;
  InstrSchedule isched;
  
private:
  unsigned int totalInstrCount;
  cycles_t curTime;
  cycles_t nextEarliestIssueTime;		// next cycle we can issue
  vector<hash_set<const SchedGraphNode*> > choicesForSlot; // indexed by slot#
  vector<const SchedGraphNode*> choiceVec;	// indexed by node ptr
  vector<int> numInClass;			// indexed by sched class
  vector<cycles_t> nextEarliestStartTime;	// indexed by opCode
  hash_map<const SchedGraphNode*, DelaySlotInfo*> delaySlotInfoForBranches;
						// indexed by branch node ptr 
  
public:
  /*ctor*/	SchedulingManager	(const TargetMachine& _target,
					 const MachineSchedInfo &schedinfo, 
					 const SchedGraph* graph,
					 SchedPriorities& schedPrio);
  /*dtor*/	~SchedulingManager	() {}
  
  //----------------------------------------------------------------------
  // Simplify access to the machine instruction info
  //----------------------------------------------------------------------
  
  inline const MachineInstrInfo& getInstrInfo	() const {
    return schedInfo.getInstrInfo();
  }
  
  //----------------------------------------------------------------------
  // Interface for checking and updating the current time
  //----------------------------------------------------------------------
  
  inline cycles_t	getTime			() const {
    return curTime;
  }
  
  inline cycles_t	getEarliestIssueTime() const {
    return nextEarliestIssueTime;
  }
  
  inline cycles_t	getEarliestStartTimeForOp(MachineOpCode opCode) const {
    assert(opCode < (int) nextEarliestStartTime.size());
    return nextEarliestStartTime[opCode];
  }
  
  // Update current time to specified cycle
  inline void	updateTime		(cycles_t c) {
    curTime = c;
    schedPrio.updateTime(c);
  }
  
  //----------------------------------------------------------------------
  // Functions to manage the choices for the current cycle including:
  // -- a vector of choices by priority (choiceVec)
  // -- vectors of the choices for each instruction slot (choicesForSlot[])
  // -- number of choices in each sched class, used to check issue conflicts
  //    between choices for a single cycle
  //----------------------------------------------------------------------
  
  inline unsigned int getNumChoices	() const {
    return choiceVec.size();
  }
  
  inline unsigned getNumChoicesInClass	(const InstrSchedClass& sc) const {
    assert(sc < (int) numInClass.size() && "Invalid op code or sched class!");
    return numInClass[sc];
  }
  
  inline const SchedGraphNode* getChoice(unsigned int i) const {
    // assert(i < choiceVec.size());	don't check here.
    return choiceVec[i];
  }
  
  inline hash_set<const SchedGraphNode*>& getChoicesForSlot(unsigned slotNum) {
    assert(slotNum < nslots);
    return choicesForSlot[slotNum];
  }
  
  inline void	addChoice		(const SchedGraphNode* node) {
    // Append the instruction to the vector of choices for current cycle.
    // Increment numInClass[c] for the sched class to which the instr belongs.
    choiceVec.push_back(node);
    const InstrSchedClass& sc = schedInfo.getSchedClass(node->getMachineInstr()->getOpCode());
    assert(sc < (int) numInClass.size());
    numInClass[sc]++;
  }
  
  inline void	addChoiceToSlot		(unsigned int slotNum,
					 const SchedGraphNode* node) {
    // Add the instruction to the choice set for the specified slot
    assert(slotNum < nslots);
    choicesForSlot[slotNum].insert(node);
  }
  
  inline void	resetChoices		() {
    choiceVec.clear();
    for (unsigned int s=0; s < nslots; s++)
      choicesForSlot[s].clear();
    for (unsigned int c=0; c < numInClass.size(); c++)
      numInClass[c] = 0;
  }
  
  //----------------------------------------------------------------------
  // Code to query and manage the partial instruction schedule so far
  //----------------------------------------------------------------------
  
  inline unsigned int	getNumScheduled	() const {
    return isched.getNumInstructions();
  }
  
  inline unsigned int	getNumUnscheduled() const {
    return totalInstrCount - isched.getNumInstructions();
  }
  
  inline bool		isScheduled	(const SchedGraphNode* node) const {
    return (isched.getStartTime(node->getNodeId()) >= 0);
  }
  
  inline void	scheduleInstr		(const SchedGraphNode* node,
					 unsigned int slotNum,
					 cycles_t cycle)
  {
    assert(! isScheduled(node) && "Instruction already scheduled?");
    
    // add the instruction to the schedule
    isched.scheduleInstr(node, slotNum, cycle);
    
    // update the earliest start times of all nodes that conflict with `node'
    // and the next-earliest time anything can issue if `node' causes bubbles
    updateEarliestStartTimes(node, cycle);
    
    // remove the instruction from the choice sets for all slots
    for (unsigned s=0; s < nslots; s++)
      choicesForSlot[s].erase(node);
    
    // and decrement the instr count for the sched class to which it belongs
    const InstrSchedClass& sc = schedInfo.getSchedClass(node->getMachineInstr()->getOpCode());
    assert(sc < (int) numInClass.size());
    numInClass[sc]--;
  }

  //----------------------------------------------------------------------
  // Create and retrieve delay slot info for delayed instructions
  //----------------------------------------------------------------------
  
  inline DelaySlotInfo* getDelaySlotInfoForInstr(const SchedGraphNode* bn,
						 bool createIfMissing=false)
  {
    DelaySlotInfo* dinfo;
    hash_map<const SchedGraphNode*, DelaySlotInfo* >::const_iterator
      I = delaySlotInfoForBranches.find(bn);
    if (I == delaySlotInfoForBranches.end())
      {
	if (createIfMissing)
	  {
	    dinfo = new DelaySlotInfo(bn,
			  getInstrInfo().getNumDelaySlots(bn->getMachineInstr()->getOpCode()));
	    delaySlotInfoForBranches[bn] = dinfo;
	  }
	else
	  dinfo = NULL;
      }
    else
      dinfo = (*I).second;
    
    return dinfo;
  }
  
private:
  /*ctor*/	SchedulingManager	();	// Disable: DO NOT IMPLEMENT.
  void		updateEarliestStartTimes(const SchedGraphNode* node,
					 cycles_t schedTime);
};


/*ctor*/
SchedulingManager::SchedulingManager(const TargetMachine& target,
				     const MachineSchedInfo &schedinfo,
				     const SchedGraph* graph,
				     SchedPriorities& _schedPrio)
  : nslots(schedinfo.getMaxNumIssueTotal()),
    schedInfo(schedinfo),
    schedPrio(_schedPrio),
    isched(nslots, graph->getNumNodes()),
    totalInstrCount(graph->getNumNodes() - 2),
    nextEarliestIssueTime(0),
    choicesForSlot(nslots),
    numInClass(schedinfo.getNumSchedClasses(), 0),	// set all to 0
    nextEarliestStartTime(target.getInstrInfo().getNumRealOpCodes(),
			  (cycles_t) 0)				// set all to 0
{
  updateTime(0);
  
  // Note that an upper bound on #choices for each slot is = nslots since
  // we use this vector to hold a feasible set of instructions, and more
  // would be infeasible. Reserve that much memory since it is probably small.
  for (unsigned int i=0; i < nslots; i++)
    choicesForSlot[i].resize(nslots);
}


void
SchedulingManager::updateEarliestStartTimes(const SchedGraphNode* node,
					    cycles_t schedTime)
{
  if (schedInfo.numBubblesAfter(node->getMachineInstr()->getOpCode()) > 0)
    { // Update next earliest time before which *nothing* can issue.
      nextEarliestIssueTime = max(nextEarliestIssueTime,
		  curTime + 1 + schedInfo.numBubblesAfter(node->getMachineInstr()->getOpCode()));
    }
  
  const vector<MachineOpCode>*
    conflictVec = schedInfo.getConflictList(node->getMachineInstr()->getOpCode());
  
  if (conflictVec != NULL)
    for (unsigned i=0; i < conflictVec->size(); i++)
      {
	MachineOpCode toOp = (*conflictVec)[i];
	cycles_t est = schedTime + schedInfo.getMinIssueGap(node->getMachineInstr()->getOpCode(),
							    toOp);
	assert(toOp < (int) nextEarliestStartTime.size());
	if (nextEarliestStartTime[toOp] < est)
	  nextEarliestStartTime[toOp] = est;
      }
}

//************************* External Functions *****************************/


//---------------------------------------------------------------------------
// Function: ScheduleInstructionsWithSSA
// 
// Purpose:
//   Entry point for instruction scheduling on SSA form.
//   Schedules the machine instructions generated by instruction selection.
//   Assumes that register allocation has not been done, i.e., operands
//   are still in SSA form.
//---------------------------------------------------------------------------

bool ScheduleInstructionsWithSSA(Method* method, const TargetMachine &target,
				 const MachineSchedInfo &schedInfo) {
  SchedGraphSet graphSet(method, target);	
  
  if (SchedDebugLevel >= Sched_PrintSchedGraphs)
    {
      cout << endl << "*** SCHEDULING GRAPHS FOR INSTRUCTION SCHEDULING"
	   << endl;
      graphSet.dump();
    }
  
  for (SchedGraphSet::const_iterator GI=graphSet.begin();
       GI != graphSet.end(); ++GI)
    {
      SchedGraph* graph = (*GI).second;
      const vector<const BasicBlock*>& bbvec = graph->getBasicBlocks();
      assert(bbvec.size() == 1 && "Cannot schedule multiple basic blocks");
      const BasicBlock* bb = bbvec[0];
      
      if (SchedDebugLevel >= Sched_PrintSchedTrace)
	cout << endl << "*** TRACE OF INSTRUCTION SCHEDULING OPERATIONS\n\n";
      
      SchedPriorities schedPrio(method, graph);	     // expensive!
      SchedulingManager S(target, schedInfo, graph, schedPrio);
      
      ChooseInstructionsForDelaySlots(S, bb, graph); // modifies graph
      
      ForwardListSchedule(S);			     // computes schedule in S
      
      RecordSchedule((*GI).first, S);		     // records schedule in BB
    }
  
  if (SchedDebugLevel >= Sched_PrintMachineCode)
    {
      cout << endl
	   << "*** Machine instructions after INSTRUCTION SCHEDULING" << endl;
      PrintMachineInstructions(method);
    }
  
  return false;					 // no reason to fail yet
}


// Check minimum gap requirements relative to instructions scheduled in
// previous cycles.
// Note that we do not need to consider `nextEarliestIssueTime' here because
// that is also captured in the earliest start times for each opcode.
// 
static inline bool
ViolatesMinimumGap(const SchedulingManager& S,
		   MachineOpCode opCode,
		   const cycles_t inCycle)
{
  return (inCycle < S.getEarliestStartTimeForOp(opCode));
}


// Check if the instruction would conflict with instructions already
// chosen for the current cycle
// 
static inline bool
ConflictsWithChoices(const SchedulingManager& S,
		     MachineOpCode opCode)
{
  // Check if the instruction must issue by itself, and some feasible
  // choices have already been made for this cycle
  if (S.getNumChoices() > 0 && S.schedInfo.isSingleIssue(opCode))
    return true;
  
  // For each class that opCode belongs to, check if there are too many
  // instructions of that class.
  // 
  const InstrSchedClass sc = S.schedInfo.getSchedClass(opCode);
  return (S.getNumChoicesInClass(sc) == S.schedInfo.getMaxIssueForClass(sc));
}


// Check if any issue restrictions would prevent the instruction from
// being issued in the current cycle
// 
bool
instrIsFeasible(const SchedulingManager& S,
		MachineOpCode opCode)
{
  // skip the instruction if it cannot be issued due to issue restrictions
  // caused by previously issued instructions
  if (ViolatesMinimumGap(S, opCode, S.getTime()))
    return false;
  
  // skip the instruction if it cannot be issued due to issue restrictions
  // caused by previously chosen instructions for the current cycle
  if (ConflictsWithChoices(S, opCode))
    return false;
  
  return true;
}

//************************* Internal Functions *****************************/


static void
ForwardListSchedule(SchedulingManager& S)
{
  unsigned N;
  const SchedGraphNode* node;
  
  S.schedPrio.initialize();
  
  while ((N = S.schedPrio.getNumReady()) > 0)
    {
      // Choose one group of instructions for a cycle.  This will
      // advance S.getTime() to the first cycle instructions can be issued.
      // It may also schedule delay slot instructions in later cycles,
      // but those are ignored here because they are outside the graph.
      // 
      unsigned numIssued = ChooseOneGroup(S);
      assert(numIssued > 0 && "Deadlock in list scheduling algorithm?");
      
      // Notify the priority manager of scheduled instructions and mark
      // any successors that may now be ready
      // 
      const InstrGroup* igroup = S.isched.getIGroup(S.getTime());
      for (unsigned int s=0; s < S.nslots; s++)
	if ((node = (*igroup)[s]) != NULL)
	  {
	    S.schedPrio.issuedReadyNodeAt(S.getTime(), node);
	    MarkSuccessorsReady(S, node);
	  }
      
      // Move to the next the next earliest cycle for which
      // an instruction can be issued, or the next earliest in which
      // one will be ready, or to the next cycle, whichever is latest.
      // 
      S.updateTime(max(S.getTime() + 1,
		       max(S.getEarliestIssueTime(),
			   S.schedPrio.getEarliestReadyTime())));
    }
}


// 
// For now, just assume we are scheduling within a single basic block.
// Get the machine instruction vector for the basic block and clear it,
// then append instructions in scheduled order.
// Also, re-insert the dummy PHI instructions that were at the beginning
// of the basic block, since they are not part of the schedule.
//   
static void
RecordSchedule(const BasicBlock* bb, const SchedulingManager& S)
{
  if (S.isched.getNumInstructions() == 0)
    return;				// empty basic block!
  
  MachineCodeForBasicBlock& mvec = bb->getMachineInstrVec();
  unsigned int oldSize = mvec.size(); 
  
  // First find the dummy instructions at the start of the basic block
  const MachineInstrInfo& mii = S.schedInfo.getInstrInfo();
  MachineCodeForBasicBlock::iterator I = mvec.begin();
  for ( ; I != mvec.end(); ++I)
    if (! mii.isDummyPhiInstr((*I)->getOpCode()))
      break;
  
  // Erase all except the dummy PHI instructions from mvec, and
  // pre-allocate create space for the ones we will be put back in.
  mvec.erase(I, mvec.end());
  mvec.reserve(mvec.size() + S.isched.getNumInstructions());
  
  InstrSchedule::const_iterator NIend = S.isched.end();
  for (InstrSchedule::const_iterator NI = S.isched.begin(); NI != NIend; ++NI)
    mvec.push_back(const_cast<MachineInstr*>((*NI)->getMachineInstr()));
}


static unsigned
ChooseOneGroup(SchedulingManager& S)
{
  assert(S.schedPrio.getNumReady() > 0
	 && "Don't get here without ready instructions.");
  
  DelaySlotInfo* getDelaySlotInfo;
  
  // Choose up to `nslots' feasible instructions and their possible slots.
  unsigned numIssued = FindSlotChoices(S, getDelaySlotInfo);
  
  while (numIssued == 0)
    {
      S.updateTime(S.getTime()+1);
      numIssued = FindSlotChoices(S, getDelaySlotInfo);
    }
  
  AssignInstructionsToSlots(S, numIssued);
  
  if (getDelaySlotInfo != NULL)
    getDelaySlotInfo->scheduleDelayedNode(S); 
  
  // Print trace of scheduled instructions before newly ready ones
  if (SchedDebugLevel >= Sched_PrintSchedTrace)
    {
      cout << "    Cycle " << S.getTime() << " : Scheduled instructions:\n";
      const InstrGroup* igroup = S.isched.getIGroup(S.getTime());
      for (unsigned int s=0; s < S.nslots; s++)
	{
	  cout << "        ";
	  if ((*igroup)[s] != NULL)
	    cout << * ((*igroup)[s])->getMachineInstr() << endl;
	  else
	    cout << "<none>" << endl;
	}
    }
  
  return numIssued;
}


static void
MarkSuccessorsReady(SchedulingManager& S, const SchedGraphNode* node)
{
  // Check if any successors are now ready that were not already marked
  // ready before, and that have not yet been scheduled.
  // 
  for (sg_succ_const_iterator SI = succ_begin(node); SI !=succ_end(node); ++SI)
    if (! (*SI)->isDummyNode()
	&& ! S.isScheduled(*SI)
	&& ! S.schedPrio.nodeIsReady(*SI))
      {// successor not scheduled and not marked ready; check *its* preds.
	
	bool succIsReady = true;
	for (sg_pred_const_iterator P=pred_begin(*SI); P != pred_end(*SI); ++P)
	  if (! (*P)->isDummyNode()
	      && ! S.isScheduled(*P))
	    {
	      succIsReady = false;
	      break;
	    }
	
	if (succIsReady)	// add the successor to the ready list
	  S.schedPrio.insertReady(*SI);
      }
}


// Choose up to `nslots' FEASIBLE instructions and assign each
// instruction to all possible slots that do not violate feasibility.
// FEASIBLE means it should be guaranteed that the set
// of chosen instructions can be issued in a single group.
// 
// Return value:
//	maxIssue : total number of feasible instructions
//	S.choicesForSlot[i=0..nslots] : set of instructions feasible in slot i
// 
static unsigned
FindSlotChoices(SchedulingManager& S,
		DelaySlotInfo*& getDelaySlotInfo)
{
  // initialize result vectors to empty
  S.resetChoices();
  
  // find the slot to start from, in the current cycle
  unsigned int startSlot = 0;
  InstrGroup* igroup = S.isched.getIGroup(S.getTime());
  for (int s = S.nslots - 1; s >= 0; s--)
    if ((*igroup)[s] != NULL)
      {
	startSlot = s+1;
	break;
      }
  
  // Make sure we pick at most one instruction that would break the group.
  // Also, if we do pick one, remember which it was.
  unsigned int indexForBreakingNode = S.nslots;
  unsigned int indexForDelayedInstr = S.nslots;
  DelaySlotInfo* delaySlotInfo = NULL;

  getDelaySlotInfo = NULL;
  
  // Choose instructions in order of priority.
  // Add choices to the choice vector in the SchedulingManager class as
  // we choose them so that subsequent choices will be correctly tested
  // for feasibility, w.r.t. higher priority choices for the same cycle.
  // 
  while (S.getNumChoices() < S.nslots - startSlot)
    {
      const SchedGraphNode* nextNode=S.schedPrio.getNextHighest(S,S.getTime());
      if (nextNode == NULL)
	break;			// no more instructions for this cycle
      
      if (S.getInstrInfo().getNumDelaySlots(nextNode->getMachineInstr()->getOpCode()) > 0)
	{
	  delaySlotInfo = S.getDelaySlotInfoForInstr(nextNode);
	  if (delaySlotInfo != NULL)
	    {
	      if (indexForBreakingNode < S.nslots)
		// cannot issue a delayed instr in the same cycle as one
		// that breaks the issue group or as another delayed instr
		nextNode = NULL;
	      else
		indexForDelayedInstr = S.getNumChoices();
	    }
	}
      else if (S.schedInfo.breaksIssueGroup(nextNode->getMachineInstr()->getOpCode()))
	{
	  if (indexForBreakingNode < S.nslots)
	    // have a breaking instruction already so throw this one away
	    nextNode = NULL;
	  else
	    indexForBreakingNode = S.getNumChoices();
	}
      
      if (nextNode != NULL)
	S.addChoice(nextNode);
      
      if (S.schedInfo.isSingleIssue(nextNode->getMachineInstr()->getOpCode()))
	{
	  assert(S.getNumChoices() == 1 &&
		 "Prioritizer returned invalid instr for this cycle!");
	  break;
	}
      
      if (indexForDelayedInstr < S.nslots)
	break;			// leave the rest for delay slots
    }
  
  assert(S.getNumChoices() <= S.nslots);
  assert(! (indexForDelayedInstr < S.nslots &&
	    indexForBreakingNode < S.nslots) && "Cannot have both in a cycle");
  
  // Assign each chosen instruction to all possible slots for that instr.
  // But if only one instruction was chosen, put it only in the first
  // feasible slot; no more analysis will be needed.
  // 
  if (indexForDelayedInstr >= S.nslots && 
      indexForBreakingNode >= S.nslots)
    { // No instructions that break the issue group or that have delay slots.
      // This is the common case, so handle it separately for efficiency.
      
      if (S.getNumChoices() == 1)
	{
	  MachineOpCode opCode = S.getChoice(0)->getMachineInstr()->getOpCode();
	  unsigned int s;
	  for (s=startSlot; s < S.nslots; s++)
	    if (S.schedInfo.instrCanUseSlot(opCode, s))
	      break;
	  assert(s < S.nslots && "No feasible slot for this opCode?");
	  S.addChoiceToSlot(s, S.getChoice(0));
	}
      else
	{
	  for (unsigned i=0; i < S.getNumChoices(); i++)
	    {
	      MachineOpCode opCode = S.getChoice(i)->getMachineInstr()->getOpCode();
	      for (unsigned int s=startSlot; s < S.nslots; s++)
		if (S.schedInfo.instrCanUseSlot(opCode, s))
		  S.addChoiceToSlot(s, S.getChoice(i));
	    }
	}
    }
  else if (indexForDelayedInstr < S.nslots)
    {
      // There is an instruction that needs delay slots.
      // Try to assign that instruction to a higher slot than any other
      // instructions in the group, so that its delay slots can go
      // right after it.
      //  

      assert(indexForDelayedInstr == S.getNumChoices() - 1 &&
	     "Instruction with delay slots should be last choice!");
      assert(delaySlotInfo != NULL && "No delay slot info for instr?");
      
      const SchedGraphNode* delayedNode = S.getChoice(indexForDelayedInstr);
      MachineOpCode delayOpCode = delayedNode->getMachineInstr()->getOpCode();
      unsigned ndelays= S.getInstrInfo().getNumDelaySlots(delayOpCode);
      
      unsigned delayedNodeSlot = S.nslots;
      int highestSlotUsed;
      
      // Find the last possible slot for the delayed instruction that leaves
      // at least `d' slots vacant after it (d = #delay slots)
      for (int s = S.nslots-ndelays-1; s >= (int) startSlot; s--)
	if (S.schedInfo.instrCanUseSlot(delayOpCode, s))
	  {
	    delayedNodeSlot = s;
	    break;
	  }
      
      highestSlotUsed = -1;
      for (unsigned i=0; i < S.getNumChoices() - 1; i++)
	{
	  // Try to assign every other instruction to a lower numbered
	  // slot than delayedNodeSlot.
	  MachineOpCode opCode = S.getChoice(i)->getMachineInstr()->getOpCode();
	  bool noSlotFound = true;
	  unsigned int s;
	  for (s=startSlot; s < delayedNodeSlot; s++)
	    if (S.schedInfo.instrCanUseSlot(opCode, s))
	      {
		S.addChoiceToSlot(s, S.getChoice(i));
		noSlotFound = false;
	      }
	  
	  // No slot before `delayedNodeSlot' was found for this opCode
	  // Use a later slot, and allow some delay slots to fall in
	  // the next cycle.
	  if (noSlotFound)
	    for ( ; s < S.nslots; s++)
	      if (S.schedInfo.instrCanUseSlot(opCode, s))
		{
		  S.addChoiceToSlot(s, S.getChoice(i));
		  break;
		}
	  
	  assert(s < S.nslots && "No feasible slot for instruction?");
	  
	  highestSlotUsed = max(highestSlotUsed, (int) s);
	}
      
      assert(highestSlotUsed <= (int) S.nslots-1 && "Invalid slot used?");
      
      // We will put the delayed node in the first slot after the
      // highest slot used.  But we just mark that for now, and
      // schedule it separately because we want to schedule the delay
      // slots for the node at the same time.
      cycles_t dcycle = S.getTime();
      unsigned int dslot = highestSlotUsed + 1;
      if (dslot == S.nslots)
	{
	  dslot = 0;
	  ++dcycle;
	}
      delaySlotInfo->recordChosenSlot(dcycle, dslot);
      getDelaySlotInfo = delaySlotInfo;
    }
  else
    { // There is an instruction that breaks the issue group.
      // For such an instruction, assign to the last possible slot in
      // the current group, and then don't assign any other instructions
      // to later slots.
      assert(indexForBreakingNode < S.nslots);
      const SchedGraphNode* breakingNode=S.getChoice(indexForBreakingNode);
      unsigned breakingSlot = INT_MAX;
      unsigned int nslotsToUse = S.nslots;
	  
      // Find the last possible slot for this instruction.
      for (int s = S.nslots-1; s >= (int) startSlot; s--)
	if (S.schedInfo.instrCanUseSlot(breakingNode->getMachineInstr()->getOpCode(), s))
	  {
	    breakingSlot = s;
	    break;
	  }
      assert(breakingSlot < S.nslots &&
	     "No feasible slot for `breakingNode'?");
      
      // Higher priority instructions than the one that breaks the group:
      // These can be assigned to all slots, but will be assigned only
      // to earlier slots if possible.
      for (unsigned i=0;
	   i < S.getNumChoices() && i < indexForBreakingNode; i++)
	{
	  MachineOpCode opCode = S.getChoice(i)->getMachineInstr()->getOpCode();
	  
	  // If a higher priority instruction cannot be assigned to
	  // any earlier slots, don't schedule the breaking instruction.
	  // 
	  bool foundLowerSlot = false;
	  nslotsToUse = S.nslots;	    // May be modified in the loop
	  for (unsigned int s=startSlot; s < nslotsToUse; s++)
	    if (S.schedInfo.instrCanUseSlot(opCode, s))
	      {
		if (breakingSlot < S.nslots && s < breakingSlot)
		  {
		    foundLowerSlot = true;
		    nslotsToUse = breakingSlot; // RESETS LOOP UPPER BOUND!
		  }
		    
		S.addChoiceToSlot(s, S.getChoice(i));
	      }
	      
	  if (!foundLowerSlot)
	    breakingSlot = INT_MAX;		// disable breaking instr
	}
      
      // Assign the breaking instruction (if any) to a single slot
      // Otherwise, just ignore the instruction.  It will simply be
      // scheduled in a later cycle.
      if (breakingSlot < S.nslots)
	{
	  S.addChoiceToSlot(breakingSlot, breakingNode);
	  nslotsToUse = breakingSlot;
	}
      else
	nslotsToUse = S.nslots;
	  
      // For lower priority instructions than the one that breaks the
      // group, only assign them to slots lower than the breaking slot.
      // Otherwise, just ignore the instruction.
      for (unsigned i=indexForBreakingNode+1; i < S.getNumChoices(); i++)
	{
	  bool foundLowerSlot = false;
	  MachineOpCode opCode = S.getChoice(i)->getMachineInstr()->getOpCode();
	  for (unsigned int s=startSlot; s < nslotsToUse; s++)
	    if (S.schedInfo.instrCanUseSlot(opCode, s))
	      S.addChoiceToSlot(s, S.getChoice(i));
	}
    } // endif (no delay slots and no breaking slots)
  
  return S.getNumChoices();
}


static void
AssignInstructionsToSlots(class SchedulingManager& S, unsigned maxIssue)
{
  // find the slot to start from, in the current cycle
  unsigned int startSlot = 0;
  cycles_t curTime = S.getTime();
  
  assert(maxIssue > 0 && maxIssue <= S.nslots - startSlot);
  
  // If only one instruction can be issued, do so.
  if (maxIssue == 1)
    for (unsigned s=startSlot; s < S.nslots; s++)
      if (S.getChoicesForSlot(s).size() > 0)
	{// found the one instruction
	  S.scheduleInstr(*S.getChoicesForSlot(s).begin(), s, curTime);
	  return;
	}
  
  // Otherwise, choose from the choices for each slot
  // 
  InstrGroup* igroup = S.isched.getIGroup(S.getTime());
  assert(igroup != NULL && "Group creation failed?");
  
  // Find a slot that has only a single choice, and take it.
  // If all slots have 0 or multiple choices, pick the first slot with
  // choices and use its last instruction (just to avoid shifting the vector).
  unsigned numIssued;
  for (numIssued = 0; numIssued < maxIssue; numIssued++)
    {
      int chosenSlot = -1, chosenNodeIndex = -1;
      for (unsigned s=startSlot; s < S.nslots; s++)
	if ((*igroup)[s] == NULL && S.getChoicesForSlot(s).size() == 1)
	  {
	    chosenSlot = (int) s;
	    break;
	  }
      
      if (chosenSlot == -1)
	for (unsigned s=startSlot; s < S.nslots; s++)
	  if ((*igroup)[s] == NULL && S.getChoicesForSlot(s).size() > 0)
	    {
	      chosenSlot = (int) s;
	      break;
	    }
      
      if (chosenSlot != -1)
	{ // Insert the chosen instr in the chosen slot and
	  // erase it from all slots.
	  const SchedGraphNode* node= *S.getChoicesForSlot(chosenSlot).begin();
	  S.scheduleInstr(node, chosenSlot, curTime);
	}
    }
  
  assert(numIssued > 0 && "Should not happen when maxIssue > 0!");
}



//---------------------------------------------------------------------
// Code for filling delay slots for delayed terminator instructions
// (e.g., BRANCH and RETURN).  Delay slots for non-terminator
// instructions (e.g., CALL) are not handled here because they almost
// always can be filled with instructions from the call sequence code
// before a call.  That's preferable because we incur many tradeoffs here
// when we cannot find single-cycle instructions that can be reordered.
//----------------------------------------------------------------------

static void
ChooseInstructionsForDelaySlots(SchedulingManager& S,
				const BasicBlock* bb,
				SchedGraph* graph)
{
  // Look for instructions that can be used for delay slots.
  // Remove them from the graph, and mark them to be used for delay slots.
  const MachineInstrInfo& mii = S.getInstrInfo();
  const TerminatorInst* term = bb->getTerminator();
  MachineCodeForVMInstr& termMvec = term->getMachineInstrVec();
  
  // Find the first branch instr in the sequence of machine instrs for term
  // 
  unsigned first = 0;
  while (! mii.isBranch(termMvec[first]->getOpCode()))
    ++first;
  assert(first < termMvec.size() &&
	 "No branch instructions for BR?  Ok, but weird!  Delete assertion.");
  if (first == termMvec.size())
    return;
  
  SchedGraphNode* brNode = graph->getGraphNodeForInstr(termMvec[first]);
  assert(! mii.isCall(brNode->getMachineInstr()->getOpCode()) && "Call used as terminator?");
  
  unsigned ndelays = mii.getNumDelaySlots(brNode->getMachineInstr()->getOpCode());
  if (ndelays == 0)
    return;
  
  // Use vectors to remember the nodes chosen for delay slots, and the
  // NOPs that will be unused.  We cannot remove them from the graph while
  // walking through the preds and succs of the brNode here, so we
  // remember the nodes in the vectors and remove them later.
  // We use separate vectors for the single-cycle and multi-cycle nodes,
  // so that we can give preference to single-cycle nodes.
  // 
  vector<SchedGraphNode*> sdelayNodeVec;
  vector<SchedGraphNode*> mdelayNodeVec;
  vector<SchedGraphNode*> nopNodeVec;
  unsigned numUseful = 0;
  
  sdelayNodeVec.reserve(ndelays);
  
  for (sg_pred_iterator P = pred_begin(brNode);
       P != pred_end(brNode) && sdelayNodeVec.size() < ndelays; ++P)
    if (! (*P)->isDummyNode() &&
	! mii.isNop((*P)->getMachineInstr()->getOpCode()) &&
	NodeCanFillDelaySlot(S, *P, brNode, /*pred*/ true))
      {
	++numUseful;
	if (mii.maxLatency((*P)->getMachineInstr()->getOpCode()) > 1)
	  mdelayNodeVec.push_back(*P);
	else
	  sdelayNodeVec.push_back(*P);
      }
  
  // If not enough single-cycle instructions were found, select the
  // lowest-latency multi-cycle instructions and use them.
  // Note that this is the most efficient code when only 1 (or even 2)
  // values need to be selected.
  // 
  while (sdelayNodeVec.size() < ndelays && mdelayNodeVec.size() > 0)
    {
      unsigned latency;
      unsigned minLatency = mii.maxLatency(mdelayNodeVec[0]->getMachineInstr()->getOpCode());
      unsigned minIndex   = 0;
      for (unsigned i=1; i < mdelayNodeVec.size(); i++)
	if (minLatency >=
	    (latency = mii.maxLatency(mdelayNodeVec[i]->getMachineInstr()->getOpCode())))
	  {
	    minLatency = latency;
	    minIndex   = i;
	  }
      sdelayNodeVec.push_back(mdelayNodeVec[minIndex]);
      if (sdelayNodeVec.size() < ndelays) // avoid the last erase!
	mdelayNodeVec.erase(mdelayNodeVec.begin() + minIndex);
    }
  
  // Now, remove the NOPs currently in delay slots from the graph.
  // If not enough useful instructions were found, use the NOPs to
  // fill delay slots, otherwise, just discard them.
  for (sg_succ_iterator I=succ_begin(brNode); I != succ_end(brNode); ++I)
    if (! (*I)->isDummyNode()
	&& mii.isNop((*I)->getMachineInstr()->getOpCode()))
      {
	if (sdelayNodeVec.size() < ndelays)
	  sdelayNodeVec.push_back(*I);
	else
	  nopNodeVec.push_back(*I);
      }
  
  // Mark the nodes chosen for delay slots.  This removes them from the graph.
  for (unsigned i=0; i < sdelayNodeVec.size(); i++)
    MarkNodeForDelaySlot(S, sdelayNodeVec[i], brNode, true);
  
  // And remove the unused NOPs the graph.
  for (unsigned i=0; i < nopNodeVec.size(); i++)
    nopNodeVec[i]->eraseAllEdges();
}


bool
NodeCanFillDelaySlot(const SchedulingManager& S,
		     const SchedGraphNode* node,
		     const SchedGraphNode* brNode,
		     bool nodeIsPredecessor)
{
  assert(! node->isDummyNode());
  
  // don't put a branch in the delay slot of another branch
  if (S.getInstrInfo().isBranch(node->getMachineInstr()->getOpCode()))
    return false;
  
  // don't put a single-issue instruction in the delay slot of a branch
  if (S.schedInfo.isSingleIssue(node->getMachineInstr()->getOpCode()))
    return false;
  
  // don't put a load-use dependence in the delay slot of a branch
  const MachineInstrInfo& mii = S.getInstrInfo();
  
  for (SchedGraphNode::const_iterator EI = node->beginInEdges();
       EI != node->endInEdges(); ++EI)
    if (! (*EI)->getSrc()->isDummyNode()
	&& mii.isLoad((*EI)->getSrc()->getMachineInstr()->getOpCode())
	&& (*EI)->getDepType() == SchedGraphEdge::CtrlDep)
      return false;
  
  // for now, don't put an instruction that does not have operand
  // interlocks in the delay slot of a branch
  if (! S.getInstrInfo().hasOperandInterlock(node->getMachineInstr()->getOpCode()))
    return false;
  
  // Finally, if the instruction preceeds the branch, we make sure the
  // instruction can be reordered relative to the branch.  We simply check
  // if the instr. has only 1 outgoing edge, viz., a CD edge to the branch.
  // 
  if (nodeIsPredecessor)
    {
      bool onlyCDEdgeToBranch = true;
      for (SchedGraphNode::const_iterator OEI = node->beginOutEdges();
	   OEI != node->endOutEdges(); ++OEI)
	if (! (*OEI)->getSink()->isDummyNode()
	    && ((*OEI)->getSink() != brNode
		|| (*OEI)->getDepType() != SchedGraphEdge::CtrlDep))
	  {
	    onlyCDEdgeToBranch = false;
	    break;
	  }
      
      if (!onlyCDEdgeToBranch)
	return false;
    }
  
  return true;
}


void
MarkNodeForDelaySlot(SchedulingManager& S,
		     SchedGraphNode* node,
		     const SchedGraphNode* brNode,
		     bool nodeIsPredecessor)
{
  if (nodeIsPredecessor)
    { // If node is in the same basic block (i.e., preceeds brNode),
      // remove it and all its incident edges from the graph.
      node->eraseAllEdges();
    }
  else
    { // If the node was from a target block, add the node to the graph
      // and add a CD edge from brNode to node.
      assert(0 && "NOT IMPLEMENTED YET");
    }
  
  DelaySlotInfo* dinfo = S.getDelaySlotInfoForInstr(brNode, /*create*/ true);
  dinfo->addDelayNode(node);
}


// 
// Schedule the delayed branch and its delay slots
// 
void
DelaySlotInfo::scheduleDelayedNode(SchedulingManager& S)
{
  assert(delayedNodeSlotNum < S.nslots && "Illegal slot for branch");
  assert(S.isched.getInstr(delayedNodeSlotNum, delayedNodeCycle) == NULL
	 && "Slot for branch should be empty");
  
  unsigned int nextSlot = delayedNodeSlotNum;
  cycles_t nextTime = delayedNodeCycle;
  
  S.scheduleInstr(brNode, nextSlot, nextTime);
  
  for (unsigned d=0; d < ndelays; d++)
    {
      ++nextSlot;
      if (nextSlot == S.nslots)
	{
	  nextSlot = 0;
	  nextTime++;
	}
      
      // Find the first feasible instruction for this delay slot
      // Note that we only check for issue restrictions here.
      // We do *not* check for flow dependences but rely on pipeline
      // interlocks to resolve them.  Machines without interlocks
      // will require this code to be modified.
      for (unsigned i=0; i < delayNodeVec.size(); i++)
	{
	  const SchedGraphNode* dnode = delayNodeVec[i];
	  if ( ! S.isScheduled(dnode)
	       && S.schedInfo.instrCanUseSlot(dnode->getMachineInstr()->getOpCode(), nextSlot)
	       && instrIsFeasible(S, dnode->getMachineInstr()->getOpCode()))
	    {
	      assert(S.getInstrInfo().hasOperandInterlock(dnode->getMachineInstr()->getOpCode())
		     && "Instructions without interlocks not yet supported "
		     "when filling branch delay slots");
	      S.scheduleInstr(dnode, nextSlot, nextTime);
	      break;
	    }
	}
    }
  
  // Update current time if delay slots overflowed into later cycles.
  // Do this here because we know exactly which cycle is the last cycle
  // that contains delay slots.  The next loop doesn't compute that.
  if (nextTime > S.getTime())
    S.updateTime(nextTime);
  
  // Now put any remaining instructions in the unfilled delay slots.
  // This could lead to suboptimal performance but needed for correctness.
  nextSlot = delayedNodeSlotNum;
  nextTime = delayedNodeCycle;
  for (unsigned i=0; i < delayNodeVec.size(); i++)
    if (! S.isScheduled(delayNodeVec[i]))
      {
	do { // find the next empty slot
	  ++nextSlot;
	  if (nextSlot == S.nslots)
	    {
	      nextSlot = 0;
	      nextTime++;
	    }
	} while (S.isched.getInstr(nextSlot, nextTime) != NULL);
	
	S.scheduleInstr(delayNodeVec[i], nextSlot, nextTime);
	break;
      }
}

