//===- Dominators.cpp - Dominator Calculation -----------------------------===//
//
// This file implements simple dominator construction algorithms for finding
// forward dominators.  Postdominators are available in libanalysis, but are not
// included in libvmcore, because it's not needed.  Forward dominators are
// needed to support the Verifier pass.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/Dominators.h"
#include "llvm/Support/CFG.h"
#include "llvm/Assembly/Writer.h"
#include "Support/DepthFirstIterator.h"
#include "Support/SetOperations.h"
using std::set;

//===----------------------------------------------------------------------===//
//  DominatorSet Implementation
//===----------------------------------------------------------------------===//

static RegisterAnalysis<DominatorSet>
A("domset", "Dominator Set Construction", true);

// dominates - Return true if A dominates B.  This performs the special checks
// neccesary if A and B are in the same basic block.
//
bool DominatorSetBase::dominates(Instruction *A, Instruction *B) const {
  BasicBlock *BBA = A->getParent(), *BBB = B->getParent();
  if (BBA != BBB) return dominates(BBA, BBB);
  
  // Loop through the basic block until we find A or B.
  BasicBlock::iterator I = BBA->begin();
  for (; &*I != A && &*I != B; ++I) /*empty*/;
  
  // A dominates B if it is found first in the basic block...
  return &*I == A;
}


void DominatorSet::calculateDominatorsFromBlock(BasicBlock *RootBB) {
  bool Changed;
  Doms[RootBB].insert(RootBB);  // Root always dominates itself...
  do {
    Changed = false;

    DomSetType WorkingSet;
    df_iterator<BasicBlock*> It = df_begin(RootBB), End = df_end(RootBB);
    for ( ; It != End; ++It) {
      BasicBlock *BB = *It;
      pred_iterator PI = pred_begin(BB), PEnd = pred_end(BB);
      if (PI != PEnd) {                // Is there SOME predecessor?
	// Loop until we get to a predecessor that has had it's dom set filled
	// in at least once.  We are guaranteed to have this because we are
	// traversing the graph in DFO and have handled start nodes specially.
	//
	while (Doms[*PI].empty()) ++PI;
	WorkingSet = Doms[*PI];

	for (++PI; PI != PEnd; ++PI) { // Intersect all of the predecessor sets
	  DomSetType &PredSet = Doms[*PI];
	  if (PredSet.size())
	    set_intersect(WorkingSet, PredSet);
	}
      }
	
      WorkingSet.insert(BB);           // A block always dominates itself
      DomSetType &BBSet = Doms[BB];
      if (BBSet != WorkingSet) {
	BBSet.swap(WorkingSet);        // Constant time operation!
	Changed = true;                // The sets changed.
      }
      WorkingSet.clear();              // Clear out the set for next iteration
    }
  } while (Changed);
}



// runOnFunction - This method calculates the forward dominator sets for the
// specified function.
//
bool DominatorSet::runOnFunction(Function &F) {
  Doms.clear();   // Reset from the last time we were run...
  Root = &F.getEntryNode();
  assert(pred_begin(Root) == pred_end(Root) &&
	 "Root node has predecessors in function!");

  // Calculate dominator sets for the reachable basic blocks...
  calculateDominatorsFromBlock(Root);

  // Every basic block in the function should at least dominate themselves, and
  // thus every basic block should have an entry in Doms.  The one case where we
  // miss this is when a basic block is unreachable.  To get these we now do an
  // extra pass over the function, calculating dominator information for
  // unreachable blocks.
  //
  for (Function::iterator I = F.begin(), E = F.end(); I != E; ++I)
    if (Doms[I].empty()) {
      calculateDominatorsFromBlock(I);
    }

  return false;
}


static std::ostream &operator<<(std::ostream &o, const set<BasicBlock*> &BBs) {
  for (set<BasicBlock*>::const_iterator I = BBs.begin(), E = BBs.end();
       I != E; ++I) {
    o << "  ";
    WriteAsOperand(o, *I, false);
    o << "\n";
   }
  return o;
}

void DominatorSetBase::print(std::ostream &o) const {
  for (const_iterator I = begin(), E = end(); I != E; ++I)
    o << "=============================--------------------------------\n"
      << "\nDominator Set For Basic Block\n" << I->first
      << "-------------------------------\n" << I->second << "\n";
}

//===----------------------------------------------------------------------===//
//  ImmediateDominators Implementation
//===----------------------------------------------------------------------===//

static RegisterAnalysis<ImmediateDominators>
C("idom", "Immediate Dominators Construction", true);

// calcIDoms - Calculate the immediate dominator mapping, given a set of
// dominators for every basic block.
void ImmediateDominatorsBase::calcIDoms(const DominatorSetBase &DS) {
  // Loop over all of the nodes that have dominators... figuring out the IDOM
  // for each node...
  //
  for (DominatorSet::const_iterator DI = DS.begin(), DEnd = DS.end(); 
       DI != DEnd; ++DI) {
    BasicBlock *BB = DI->first;
    const DominatorSet::DomSetType &Dominators = DI->second;
    unsigned DomSetSize = Dominators.size();
    if (DomSetSize == 1) continue;  // Root node... IDom = null

    // Loop over all dominators of this node.  This corresponds to looping over
    // nodes in the dominator chain, looking for a node whose dominator set is
    // equal to the current nodes, except that the current node does not exist
    // in it.  This means that it is one level higher in the dom chain than the
    // current node, and it is our idom!
    //
    DominatorSet::DomSetType::const_iterator I = Dominators.begin();
    DominatorSet::DomSetType::const_iterator End = Dominators.end();
    for (; I != End; ++I) {   // Iterate over dominators...
      // All of our dominators should form a chain, where the number of elements
      // in the dominator set indicates what level the node is at in the chain.
      // We want the node immediately above us, so it will have an identical 
      // dominator set, except that BB will not dominate it... therefore it's
      // dominator set size will be one less than BB's...
      //
      if (DS.getDominators(*I).size() == DomSetSize - 1) {
	IDoms[BB] = *I;
	break;
      }
    }
  }
}

void ImmediateDominatorsBase::print(std::ostream &o) const {
  for (const_iterator I = begin(), E = end(); I != E; ++I)
    o << "=============================--------------------------------\n"
      << "\nImmediate Dominator For Basic Block\n" << *I->first
      << "is: \n" << *I->second << "\n";
}


//===----------------------------------------------------------------------===//
//  DominatorTree Implementation
//===----------------------------------------------------------------------===//

static RegisterAnalysis<DominatorTree>
E("domtree", "Dominator Tree Construction", true);

// DominatorTreeBase::reset - Free all of the tree node memory.
//
void DominatorTreeBase::reset() { 
  for (NodeMapType::iterator I = Nodes.begin(), E = Nodes.end(); I != E; ++I)
    delete I->second;
  Nodes.clear();
}

void DominatorTreeBase::Node2::setIDom(Node2 *NewIDom) {
  assert(IDom && "No immediate dominator?");
  if (IDom != NewIDom) {
    std::vector<Node*>::iterator I =
      std::find(IDom->Children.begin(), IDom->Children.end(), this);
    assert(I != IDom->Children.end() &&
           "Not in immediate dominator children set!");
    // I am no longer your child...
    IDom->Children.erase(I);

    // Switch to new dominator
    IDom = NewIDom;
    IDom->Children.push_back(this);
  }
}



void DominatorTree::calculate(const DominatorSet &DS) {
  Nodes[Root] = new Node(Root, 0);   // Add a node for the root...

  // Iterate over all nodes in depth first order...
  for (df_iterator<BasicBlock*> I = df_begin(Root), E = df_end(Root);
       I != E; ++I) {
    BasicBlock *BB = *I;
    const DominatorSet::DomSetType &Dominators = DS.getDominators(BB);
    unsigned DomSetSize = Dominators.size();
    if (DomSetSize == 1) continue;  // Root node... IDom = null
      
    // Loop over all dominators of this node. This corresponds to looping over
    // nodes in the dominator chain, looking for a node whose dominator set is
    // equal to the current nodes, except that the current node does not exist
    // in it. This means that it is one level higher in the dom chain than the
    // current node, and it is our idom!  We know that we have already added
    // a DominatorTree node for our idom, because the idom must be a
    // predecessor in the depth first order that we are iterating through the
    // function.
    //
    DominatorSet::DomSetType::const_iterator I = Dominators.begin();
    DominatorSet::DomSetType::const_iterator End = Dominators.end();
    for (; I != End; ++I) {   // Iterate over dominators...
      // All of our dominators should form a chain, where the number of
      // elements in the dominator set indicates what level the node is at in
      // the chain.  We want the node immediately above us, so it will have
      // an identical dominator set, except that BB will not dominate it...
      // therefore it's dominator set size will be one less than BB's...
      //
      if (DS.getDominators(*I).size() == DomSetSize - 1) {
        // We know that the immediate dominator should already have a node, 
        // because we are traversing the CFG in depth first order!
        //
        Node *IDomNode = Nodes[*I];
        assert(IDomNode && "No node for IDOM?");
        
        // Add a new tree node for this BasicBlock, and link it as a child of
        // IDomNode
        Nodes[BB] = IDomNode->addChild(new Node(BB, IDomNode));
        break;
      }
    }
  }
}


static std::ostream &operator<<(std::ostream &o,
                                const DominatorTreeBase::Node *Node) {
  return o << Node->getNode()
           << "\n------------------------------------------\n";
}

static void PrintDomTree(const DominatorTreeBase::Node *N, std::ostream &o,
                         unsigned Lev) {
  o << "Level #" << Lev << ":  " << N;
  for (DominatorTreeBase::Node::const_iterator I = N->begin(), E = N->end(); 
       I != E; ++I) {
    PrintDomTree(*I, o, Lev+1);
  }
}

void DominatorTreeBase::print(std::ostream &o) const {
  o << "=============================--------------------------------\n"
    << "Inorder Dominator Tree:\n";
  PrintDomTree(Nodes.find(getRoot())->second, o, 1);
}


//===----------------------------------------------------------------------===//
//  DominanceFrontier Implementation
//===----------------------------------------------------------------------===//

static RegisterAnalysis<DominanceFrontier>
G("domfrontier", "Dominance Frontier Construction", true);

const DominanceFrontier::DomSetType &
DominanceFrontier::calculate(const DominatorTree &DT, 
                             const DominatorTree::Node *Node) {
  // Loop over CFG successors to calculate DFlocal[Node]
  BasicBlock *BB = Node->getNode();
  DomSetType &S = Frontiers[BB];       // The new set to fill in...

  for (succ_iterator SI = succ_begin(BB), SE = succ_end(BB);
       SI != SE; ++SI) {
    // Does Node immediately dominate this successor?
    if (DT[*SI]->getIDom() != Node)
      S.insert(*SI);
  }

  // At this point, S is DFlocal.  Now we union in DFup's of our children...
  // Loop through and visit the nodes that Node immediately dominates (Node's
  // children in the IDomTree)
  //
  for (DominatorTree::Node::const_iterator NI = Node->begin(), NE = Node->end();
       NI != NE; ++NI) {
    DominatorTree::Node *IDominee = *NI;
    const DomSetType &ChildDF = calculate(DT, IDominee);

    DomSetType::const_iterator CDFI = ChildDF.begin(), CDFE = ChildDF.end();
    for (; CDFI != CDFE; ++CDFI) {
      if (!Node->dominates(DT[*CDFI]))
	S.insert(*CDFI);
    }
  }

  return S;
}

void DominanceFrontierBase::print(std::ostream &o) const {
  for (const_iterator I = begin(), E = end(); I != E; ++I) {
    o << "=============================--------------------------------\n"
      << "\nDominance Frontier For Basic Block\n";
    WriteAsOperand(o, I->first, false);
    o << " is: \n" << I->second << "\n";
  }
}
