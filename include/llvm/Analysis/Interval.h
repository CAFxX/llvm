//===- llvm/Analysis/Interval.h - Interval Class Declaration -----*- C++ -*--=//
//
// This file contains the declaration of the cfg::Interval class, which
// represents a set of CFG nodes and is a portion of an interval partition.
// 
// Intervals have some interesting and useful properties, including the
// following:
//    1. The header node of an interval dominates all of the elements of the
//       interval
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_INTERVAL_H
#define LLVM_INTERVAL_H

#include <vector>

class BasicBlock;

namespace cfg {

//===----------------------------------------------------------------------===//
//
// Interval Class - An Interval is a set of nodes defined such that every node
// in the interval has all of its predecessors in the interval (except for the
// header)
//
class Interval {
  // HeaderNode - The header BasicBlock, which dominates all BasicBlocks in this
  // interval.  Also, any loops in this interval must go through the HeaderNode.
  //
  BasicBlock *HeaderNode;
public:
  typedef std::vector<BasicBlock*>::iterator succ_iterator;
  typedef std::vector<BasicBlock*>::iterator pred_iterator;
  typedef std::vector<BasicBlock*>::iterator node_iterator;

  inline Interval(BasicBlock *Header) : HeaderNode(Header) {
    Nodes.push_back(Header);
  }

  inline Interval(const Interval &I) // copy ctor
    : HeaderNode(I.HeaderNode), Nodes(I.Nodes), Successors(I.Successors) {}

  inline BasicBlock *getHeaderNode() const { return HeaderNode; }

  // Nodes - The basic blocks in this interval.
  //
  std::vector<BasicBlock*> Nodes;

  // Successors - List of BasicBlocks that are reachable directly from nodes in
  // this interval, but are not in the interval themselves.
  // These nodes neccesarily must be header nodes for other intervals.
  //
  std::vector<BasicBlock*> Successors;

  // Predecessors - List of BasicBlocks that have this Interval's header block
  // as one of their successors.
  //
  std::vector<BasicBlock*> Predecessors;

  // contains - Find out if a basic block is in this interval
  inline bool contains(BasicBlock *BB) const {
    for (unsigned i = 0; i < Nodes.size(); ++i)
      if (Nodes[i] == BB) return true;
    return false;
    // I don't want the dependency on <algorithm>
    //return find(Nodes.begin(), Nodes.end(), BB) != Nodes.end();
  }

  // isSuccessor - find out if a basic block is a successor of this Interval
  inline bool isSuccessor(BasicBlock *BB) const {
    for (unsigned i = 0; i < Successors.size(); ++i)
      if (Successors[i] == BB) return true;
    return false;
    // I don't want the dependency on <algorithm>
    //return find(Successors.begin(), Successors.end(), BB) != Successors.end();
  }

  // Equality operator.  It is only valid to compare two intervals from the same
  // partition, because of this, all we have to check is the header node for 
  // equality.
  //
  inline bool operator==(const Interval &I) const {
    return HeaderNode == I.HeaderNode;
  }

  // isLoop - Find out if there is a back edge in this interval...
  bool isLoop() const;
};

}    // End namespace cfg

// succ_begin/succ_end - define methods so that Intervals may be used
// just like BasicBlocks can with the succ_* functions, and *::succ_iterator.
//
inline cfg::Interval::succ_iterator succ_begin(cfg::Interval *I) {
  return I->Successors.begin();
}
inline cfg::Interval::succ_iterator succ_end(cfg::Interval *I)   {
  return I->Successors.end();
}
  
// pred_begin/pred_end - define methods so that Intervals may be used
// just like BasicBlocks can with the pred_* functions, and *::pred_iterator.
//
inline cfg::Interval::pred_iterator pred_begin(cfg::Interval *I) {
  return I->Predecessors.begin();
}
inline cfg::Interval::pred_iterator pred_end(cfg::Interval *I)   {
  return I->Predecessors.end();
}


#endif
