//===-- llvm/Support/PostOrderIterator.h - Generic PO iterator ---*- C++ -*--=//
//
// This file builds on the Support/GraphTraits.h file to build a generic graph
// post order iterator.  This should work over any graph type that has a
// GraphTraits specialization.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_POSTORDER_ITERATOR_H
#define LLVM_SUPPORT_POSTORDER_ITERATOR_H

#include "llvm/Support/GraphTraits.h"
#include <iterator>
#include <stack>
#include <set>

template<class GraphT, class GT = GraphTraits<GraphT> >
class po_iterator : public std::forward_iterator<typename GT::NodeType,
                                                 ptrdiff_t> {
  typedef typename GT::NodeType          NodeType;
  typedef typename GT::ChildIteratorType ChildItTy;

  set<NodeType *>   Visited;    // All of the blocks visited so far...
  // VisitStack - Used to maintain the ordering.  Top = current block
  // First element is basic block pointer, second is the 'next child' to visit
  stack<pair<NodeType *, ChildItTy> > VisitStack;

  void traverseChild() {
    while (VisitStack.top().second != GT::child_end(VisitStack.top().first)) {
      NodeType *BB = *VisitStack.top().second++;
      if (!Visited.count(BB)) {  // If the block is not visited...
	Visited.insert(BB);
	VisitStack.push(make_pair(BB, GT::child_begin(BB)));
      }
    }
  }

  inline po_iterator(NodeType *BB) {
    Visited.insert(BB);
    VisitStack.push(make_pair(BB, GT::child_begin(BB)));
    traverseChild();
  }
  inline po_iterator() { /* End is when stack is empty */ }
public:
  typedef po_iterator<GraphT, GT> _Self;

  // Provide static "constructors"...
  static inline _Self begin(GraphT G) { return _Self(GT::getEntryNode(G)); }
  static inline _Self end  (GraphT G) { return _Self(); }

  inline bool operator==(const _Self& x) const { 
    return VisitStack == x.VisitStack;
  }
  inline bool operator!=(const _Self& x) const { return !operator==(x); }

  inline pointer operator*() const { 
    return VisitStack.top().first;
  }

  // This is a nonstandard operator-> that dereferences the pointer an extra
  // time... so that you can actually call methods ON the BasicBlock, because
  // the contained type is a pointer.  This allows BBIt->getTerminator() f.e.
  //
  inline NodeType *operator->() const { return operator*(); }

  inline _Self& operator++() {   // Preincrement
    VisitStack.pop();
    if (!VisitStack.empty())
      traverseChild();
    return *this; 
  }

  inline _Self operator++(int) { // Postincrement
    _Self tmp = *this; ++*this; return tmp; 
  }
};

// Provide global constructors that automatically figure out correct types...
//
template <class T>
po_iterator<T> po_begin(T G) { return po_iterator<T>::begin(G); }
template <class T>
po_iterator<T> po_end  (T G) { return po_iterator<T>::end(G); }

// Provide global definitions of inverse post order iterators...
template <class T>
struct ipo_iterator : public po_iterator<Inverse<T> > {
  ipo_iterator(const po_iterator<Inverse<T> > &V) :po_iterator<Inverse<T> >(V){}
};

template <class T>
ipo_iterator<T> ipo_begin(T G, bool Reverse = false) {
  return ipo_iterator<T>::begin(G, Reverse);
}

template <class T>
ipo_iterator<T> ipo_end(T G){
  return ipo_iterator<T>::end(G);
}


//===--------------------------------------------------------------------===//
// Reverse Post Order CFG iterator code
//===--------------------------------------------------------------------===//
// 
// This is used to visit basic blocks in a method in reverse post order.  This
// class is awkward to use because I don't know a good incremental algorithm to
// computer RPO from a graph.  Because of this, the construction of the 
// ReversePostOrderTraversal object is expensive (it must walk the entire graph
// with a postorder iterator to build the data structures).  The moral of this
// story is: Don't create more ReversePostOrderTraversal classes than neccesary.
//
// This class should be used like this:
// {
//   cfg::ReversePostOrderTraversal RPOT(MethodPtr);   // Expensive to create
//   for (cfg::rpo_iterator I = RPOT.begin(); I != RPOT.end(); ++I) {
//      ...
//   }
//   for (cfg::rpo_iterator I = RPOT.begin(); I != RPOT.end(); ++I) {
//      ...
//   }
// }
//

typedef reverse_iterator<vector<BasicBlock*>::iterator> rpo_iterator;
// TODO: FIXME: ReversePostOrderTraversal is not generic!
class ReversePostOrderTraversal {
  vector<BasicBlock*> Blocks;       // Block list in normal PO order
  inline void Initialize(BasicBlock *BB) {
    copy(po_begin(BB), po_end(BB), back_inserter(Blocks));
  }
public:
  inline ReversePostOrderTraversal(Method *M) {
    Initialize(M->front());
  }
  inline ReversePostOrderTraversal(BasicBlock *BB) {
    Initialize(BB);
  }

  // Because we want a reverse post order, use reverse iterators from the vector
  inline rpo_iterator begin() { return Blocks.rbegin(); }
  inline rpo_iterator end()   { return Blocks.rend(); }
};

#endif
