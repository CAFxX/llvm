//===- DominatorSet.cpp - Dominator Set Calculation --------------*- C++ -*--=//
//
// This file provides a simple class to calculate the dominator set of a method.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/Dominators.h"
#include "llvm/Analysis/SimplifyCFG.h"   // To get cfg::UnifyAllExitNodes
#include "llvm/CFG.h"
#include "llvm/Tools/STLExtras.h"
#include <algorithm>

//===----------------------------------------------------------------------===//
//  Helper Template
//===----------------------------------------------------------------------===//

// set_intersect - Identical to set_intersection, except that it works on 
// set<>'s and is nicer to use.  Functionally, this iterates through S1, 
// removing elements that are not contained in S2.
//
template <class Ty, class Ty2>
void set_intersect(set<Ty> &S1, const set<Ty2> &S2) {
  for (typename set<Ty>::iterator I = S1.begin(); I != S1.end();) {
    const Ty &E = *I;
    ++I;
    if (!S2.count(E)) S1.erase(E);   // Erase element if not in S2
  }
}

//===----------------------------------------------------------------------===//
//  DominatorBase Implementation
//===----------------------------------------------------------------------===//

bool cfg::DominatorBase::isPostDominator() const { 
  return Root != Root->getParent()->front(); 
}


//===----------------------------------------------------------------------===//
//  DominatorSet Implementation
//===----------------------------------------------------------------------===//

// DominatorSet ctor - Build either the dominator set or the post-dominator
// set for a method...
//
cfg::DominatorSet::DominatorSet(const Method *M) : DominatorBase(M->front()) {
  calcForwardDominatorSet(M);
}

// calcForwardDominatorSet - This method calculates the forward dominator sets
// for the specified method.
//
void cfg::DominatorSet::calcForwardDominatorSet(const Method *M) {
  assert(Root && M && "Can't build dominator set of null method!");
  bool Changed;
  do {
    Changed = false;

    DomSetType WorkingSet;
    df_const_iterator It = df_begin(M), End = df_end(M);
    for ( ; It != End; ++It) {
      const BasicBlock *BB = *It;
      pred_const_iterator PI = pred_begin(BB), PEnd = pred_end(BB);
      if (PI != PEnd) {                // Is there SOME predecessor?
	// Loop until we get to a predecessor that has had it's dom set filled
	// in at least once.  We are guaranteed to have this because we are
	// traversing the graph in DFO and have handled start nodes specially.
	//
	while (Doms[*PI].size() == 0) ++PI;
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

// Postdominator set constructor.  This ctor converts the specified method to
// only have a single exit node (return stmt), then calculates the post
// dominance sets for the method.
//
cfg::DominatorSet::DominatorSet(Method *M, bool PostDomSet)
  : DominatorBase(M->front()) {
  if (!PostDomSet) { calcForwardDominatorSet(M); return; }

  Root = cfg::UnifyAllExitNodes(M);
  assert(Root && "TODO: Don't handle case where there are no exit nodes yet!");

  bool Changed;
  do {
    Changed = false;

    set<const BasicBlock*> Visited;
    DomSetType WorkingSet;
    idf_const_iterator It = idf_begin(Root), End = idf_end(Root);
    for ( ; It != End; ++It) {
      const BasicBlock *BB = *It;
      succ_const_iterator PI = succ_begin(BB), PEnd = succ_end(BB);
      if (PI != PEnd) {                // Is there SOME predecessor?
	// Loop until we get to a successor that has had it's dom set filled
	// in at least once.  We are guaranteed to have this because we are
	// traversing the graph in DFO and have handled start nodes specially.
	//
	while (Doms[*PI].size() == 0) ++PI;
	WorkingSet = Doms[*PI];

	for (++PI; PI != PEnd; ++PI) { // Intersect all of the successor sets
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


//===----------------------------------------------------------------------===//
//  ImmediateDominators Implementation
//===----------------------------------------------------------------------===//

// calcIDoms - Calculate the immediate dominator mapping, given a set of
// dominators for every basic block.
void cfg::ImmediateDominators::calcIDoms(const DominatorSet &DS) {
  // Loop over all of the nodes that have dominators... figuring out the IDOM
  // for each node...
  //
  for (DominatorSet::const_iterator DI = DS.begin(), DEnd = DS.end(); 
       DI != DEnd; ++DI) {
    const BasicBlock *BB = DI->first;
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


//===----------------------------------------------------------------------===//
//  DominatorTree Implementation
//===----------------------------------------------------------------------===//

// DominatorTree dtor - Free all of the tree node memory.
//
cfg::DominatorTree::~DominatorTree() { 
  for (NodeMapType::iterator I = Nodes.begin(), E = Nodes.end(); I != E; ++I)
    delete I->second;
}


cfg::DominatorTree::DominatorTree(const ImmediateDominators &IDoms) 
  : DominatorBase(IDoms.getRoot()) {
  const Method *M = Root->getParent();

  Nodes[Root] = new Node(Root, 0);   // Add a node for the root...

  // Iterate over all nodes in depth first order...
  for (df_const_iterator I = df_begin(M), E = df_end(M); I != E; ++I) {
    const BasicBlock *BB = *I, *IDom = IDoms[*I];

    if (IDom != 0) {   // Ignore the root node and other nasty nodes
      // We know that the immediate dominator should already have a node, 
      // because we are traversing the CFG in depth first order!
      //
      assert(Nodes[IDom] && "No node for IDOM?");
      Node *IDomNode = Nodes[IDom];

      // Add a new tree node for this BasicBlock, and link it as a child of
      // IDomNode
      Nodes[BB] = IDomNode->addChild(new Node(BB, IDomNode));
    }
  }
}

void cfg::DominatorTree::calculate(const DominatorSet &DS) {
  Nodes[Root] = new Node(Root, 0);   // Add a node for the root...

  if (!isPostDominator()) {
    // Iterate over all nodes in depth first order...
    for (df_const_iterator I = df_begin(Root), E = df_end(Root); I != E; ++I) {
      const BasicBlock *BB = *I;
      const DominatorSet::DomSetType &Dominators = DS.getDominators(BB);
      unsigned DomSetSize = Dominators.size();
      if (DomSetSize == 1) continue;  // Root node... IDom = null
      
      // Loop over all dominators of this node.  This corresponds to looping over
      // nodes in the dominator chain, looking for a node whose dominator set is
      // equal to the current nodes, except that the current node does not exist
      // in it.  This means that it is one level higher in the dom chain than the
      // current node, and it is our idom!  We know that we have already added
      // a DominatorTree node for our idom, because the idom must be a
      // predecessor in the depth first order that we are iterating through the
      // method.
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
  } else {
    // Iterate over all nodes in depth first order...
    for (idf_const_iterator I = idf_begin(Root), E = idf_end(Root); I != E; ++I) {
      const BasicBlock *BB = *I;
      const DominatorSet::DomSetType &Dominators = DS.getDominators(BB);
      unsigned DomSetSize = Dominators.size();
      if (DomSetSize == 1) continue;  // Root node... IDom = null
      
      // Loop over all dominators of this node.  This corresponds to looping over
      // nodes in the dominator chain, looking for a node whose dominator set is
      // equal to the current nodes, except that the current node does not exist
      // in it.  This means that it is one level higher in the dom chain than the
      // current node, and it is our idom!  We know that we have already added
      // a DominatorTree node for our idom, because the idom must be a
      // predecessor in the depth first order that we are iterating through the
      // method.
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
}



//===----------------------------------------------------------------------===//
//  DominanceFrontier Implementation
//===----------------------------------------------------------------------===//

const cfg::DominanceFrontier::DomSetType &
cfg::DominanceFrontier::calcDomFrontier(const DominatorTree &DT, 
					const DominatorTree::Node *Node) {
  // Loop over CFG successors to calculate DFlocal[Node]
  const BasicBlock *BB = Node->getNode();
  DomSetType &S = Frontiers[BB];       // The new set to fill in...

  for (succ_const_iterator SI = succ_begin(BB), SE = succ_end(BB); 
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
    const DomSetType &ChildDF = calcDomFrontier(DT, IDominee);

    DomSetType::const_iterator CDFI = ChildDF.begin(), CDFE = ChildDF.end();
    for (; CDFI != CDFE; ++CDFI) {
      if (!Node->dominates(DT[*CDFI]))
	S.insert(*CDFI);
    }
  }

  return S;
}

const cfg::DominanceFrontier::DomSetType &
cfg::DominanceFrontier::calcPostDomFrontier(const DominatorTree &DT, 
					    const DominatorTree::Node *Node) {
  // Loop over CFG successors to calculate DFlocal[Node]
  const BasicBlock *BB = Node->getNode();
  DomSetType &S = Frontiers[BB];       // The new set to fill in...

  for (pred_const_iterator SI = pred_begin(BB), SE = pred_end(BB); 
       SI != SE; ++SI) {
    // Does Node immediately dominate this predeccessor?
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
    const DomSetType &ChildDF = calcPostDomFrontier(DT, IDominee);

    DomSetType::const_iterator CDFI = ChildDF.begin(), CDFE = ChildDF.end();
    for (; CDFI != CDFE; ++CDFI) {
      if (!Node->dominates(DT[*CDFI]))
	S.insert(*CDFI);
    }
  }

  return S;
}
