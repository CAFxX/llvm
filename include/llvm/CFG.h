//===-- llvm/CFG.h - CFG definitions and useful classes ----------*- C++ -*--=//
//
// This file contains the class definitions useful for operating on the control
// flow graph.
//
// Currently it contains functionality for these three applications:
//
//  1. Iterate over the predecessors of a basic block:
//     pred_iterator, pred_const_iterator, pred_begin, pred_end
//  2. Iterate over the successors of a basic block:
//     succ_iterator, succ_const_iterator, succ_begin, succ_end
//  3. Iterate over the basic blocks of a method in depth first ordering or 
//     reverse depth first order.  df_iterator, df_const_iterator, 
//     df_begin, df_end.  df_begin takes an arg to specify reverse or not.
//  4. Iterator over the basic blocks of a method in post order.
//  5. Iterator over a method in reverse post order.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CFG_H
#define LLVM_CFG_H

#include "llvm/CFGdecls.h"      // See this file for concise interface info
#include "llvm/Method.h"
#include "llvm/BasicBlock.h"
#include "llvm/InstrTypes.h"
#include "llvm/Type.h"
#include <iterator>
#include <stack>
#include <set>

namespace cfg {

//===----------------------------------------------------------------------===//
//                                Implementation
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Basic Block Predecessor Iterator
//

template <class _Ptr,  class _USE_iterator> // Predecessor Iterator
class PredIterator : public std::bidirectional_iterator<_Ptr, ptrdiff_t> {
  _Ptr *BB;
  _USE_iterator It;
public:
  typedef PredIterator<_Ptr,_USE_iterator> _Self;
  
  inline void advancePastConstPool() {
    // TODO: This is bad
    // Loop to ignore constant pool references
    while (It != BB->use_end() && 
	   ((!(*It)->isInstruction()) ||
	    !(((Instruction*)(*It))->isTerminator())))
      ++It;
  }
  
  inline PredIterator(_Ptr *bb) : BB(bb), It(bb->use_begin()) {
    advancePastConstPool();
  }
  inline PredIterator(_Ptr *bb, bool) : BB(bb), It(bb->use_end()) {}
  
  inline bool operator==(const _Self& x) const { return It == x.It; }
  inline bool operator!=(const _Self& x) const { return !operator==(x); }
  
  inline pointer operator*() const { 
    return (*It)->castInstructionAsserting()->getParent(); 
  }
  inline pointer *operator->() const { return &(operator*()); }
  
  inline _Self& operator++() {   // Preincrement
    ++It; advancePastConstPool();
    return *this; 
  }
  
  inline _Self operator++(int) { // Postincrement
    _Self tmp = *this; ++*this; return tmp; 
  }
  
  inline _Self& operator--() { --It; return *this; }  // Predecrement
  inline _Self operator--(int) { // Postdecrement
    _Self tmp = *this; --*this; return tmp;
  }
};

inline pred_iterator       pred_begin(      BasicBlock *BB) {
  return pred_iterator(BB);
}
inline pred_const_iterator pred_begin(const BasicBlock *BB) {
  return pred_const_iterator(BB);
}
inline pred_iterator       pred_end(      BasicBlock *BB) {
  return pred_iterator(BB,true);
}
inline pred_const_iterator pred_end(const BasicBlock *BB) {
  return pred_const_iterator(BB,true);
}


//===----------------------------------------------------------------------===//
// Basic Block Successor Iterator
//

template <class _Term, class _BB>           // Successor Iterator
class SuccIterator : public std::bidirectional_iterator<_BB, ptrdiff_t> {
  const _Term Term;
  unsigned idx;
public:
  typedef SuccIterator<_Term, _BB> _Self;
  // TODO: This can be random access iterator, need operator+ and stuff tho
  
  inline SuccIterator(_Term T) : Term(T), idx(0) {         // begin iterator
    assert(T && "getTerminator returned null!");
  }
  inline SuccIterator(_Term T, bool)                       // end iterator
    : Term(T), idx(Term->getNumSuccessors()) {
    assert(T && "getTerminator returned null!");
  }
  
  inline bool operator==(const _Self& x) const { return idx == x.idx; }
  inline bool operator!=(const _Self& x) const { return !operator==(x); }
  
  inline pointer operator*() const { return Term->getSuccessor(idx); }
  inline pointer operator->() const { return operator*(); }
  
  inline _Self& operator++() { ++idx; return *this; } // Preincrement
  inline _Self operator++(int) { // Postincrement
    _Self tmp = *this; ++*this; return tmp; 
  }
  
  inline _Self& operator--() { --idx; return *this; }  // Predecrement
  inline _Self operator--(int) { // Postdecrement
    _Self tmp = *this; --*this; return tmp;
  }
};

inline succ_iterator       succ_begin(      BasicBlock *BB) {
  return succ_iterator(BB->getTerminator());
}
inline succ_const_iterator succ_begin(const BasicBlock *BB) {
  return succ_const_iterator(BB->getTerminator());
}
inline succ_iterator       succ_end(      BasicBlock *BB) {
  return succ_iterator(BB->getTerminator(),true);
}
inline succ_const_iterator succ_end(const BasicBlock *BB) {
  return succ_const_iterator(BB->getTerminator(),true);
}


//===----------------------------------------------------------------------===//
// Graph Type Declarations
//
//             BasicBlockGraph - Represent a standard traversal of a CFG
//        ConstBasicBlockGraph - Represent a standard traversal of a const CFG
//      InverseBasicBlockGraph - Represent a inverse traversal of a CFG
// ConstInverseBasicBlockGraph - Represent a inverse traversal of a const CFG
//
// An Inverse traversal of a graph is where we chase predecessors, instead of
// successors.
//
struct BasicBlockGraph {
  typedef BasicBlock NodeType;
  typedef succ_iterator ChildIteratorType;
  static inline ChildIteratorType child_begin(NodeType *N) { 
    return succ_begin(N); 
  }
  static inline ChildIteratorType child_end(NodeType *N) { 
    return succ_end(N); 
  }
};

struct ConstBasicBlockGraph {
  typedef const BasicBlock NodeType;
  typedef succ_const_iterator ChildIteratorType;
  static inline ChildIteratorType child_begin(NodeType *N) { 
    return succ_begin(N); 
  }
  static inline ChildIteratorType child_end(NodeType *N) { 
    return succ_end(N); 
  }
};

struct InverseBasicBlockGraph {
  typedef BasicBlock NodeType;
  typedef pred_iterator ChildIteratorType;
  static inline ChildIteratorType child_begin(NodeType *N) { 
    return pred_begin(N); 
  }
  static inline ChildIteratorType child_end(NodeType *N) { 
    return pred_end(N); 
  }
};

struct ConstInverseBasicBlockGraph {
  typedef const BasicBlock NodeType;
  typedef pred_const_iterator ChildIteratorType;
  static inline ChildIteratorType child_begin(NodeType *N) { 
    return pred_begin(N); 
  }
  static inline ChildIteratorType child_end(NodeType *N) { 
    return pred_end(N); 
  }
};

struct TypeGraph {
  typedef const ::Type NodeType;
  typedef ::Type::contype_iterator ChildIteratorType;
  
  static inline ChildIteratorType child_begin(NodeType *N) { 
    return N->contype_begin(); 
  }
  static inline ChildIteratorType child_end(NodeType *N) { 
    return N->contype_end();
  }
};


//===----------------------------------------------------------------------===//
// Depth First Iterator
//

// Generic Depth First Iterator
template<class GI>
class DFIterator : public std::forward_iterator<typename GI::NodeType,
						ptrdiff_t> {
  typedef typename GI::NodeType          NodeType;
  typedef typename GI::ChildIteratorType ChildItTy;

  set<NodeType *>   Visited;    // All of the blocks visited so far...
  // VisitStack - Used to maintain the ordering.  Top = current block
  // First element is node pointer, second is the 'next child' to visit
  stack<pair<NodeType *, ChildItTy> > VisitStack;
  const bool Reverse;         // Iterate over children before self?
private:
  void reverseEnterNode() {
    pair<NodeType *, ChildItTy> &Top = VisitStack.top();
    NodeType *Node = Top.first;
    ChildItTy &It  = Top.second;
    for (; It != GI::child_end(Node); ++It) {
      NodeType *Child = *It;
      if (!Visited.count(Child)) {
	Visited.insert(Child);
	VisitStack.push(make_pair(Child, GI::child_begin(Child)));
	reverseEnterNode();
	return;
      }
    }
  }
public:
  typedef DFIterator<GI> _Self;

  inline DFIterator(NodeType *Node, bool reverse) : Reverse(reverse) {
    Visited.insert(Node);
    VisitStack.push(make_pair(Node, GI::child_begin(Node)));
    if (Reverse) reverseEnterNode();
  }
  inline DFIterator() { /* End is when stack is empty */ }

  inline bool operator==(const _Self& x) const { 
    return VisitStack == x.VisitStack;
  }
  inline bool operator!=(const _Self& x) const { return !operator==(x); }

  inline pointer operator*() const { 
    return VisitStack.top().first;
  }

  // This is a nonstandard operator-> that dereferences the pointer an extra
  // time... so that you can actually call methods ON the Node, because
  // the contained type is a pointer.  This allows BBIt->getTerminator() f.e.
  //
  inline NodeType *operator->() const { return operator*(); }

  inline _Self& operator++() {   // Preincrement
    if (Reverse) {               // Reverse Depth First Iterator
      if (VisitStack.top().second == GI::child_end(VisitStack.top().first))
	VisitStack.pop();
      if (!VisitStack.empty())
	reverseEnterNode();
    } else {                     // Normal Depth First Iterator
      do {
	pair<NodeType *, ChildItTy> &Top = VisitStack.top();
	NodeType *Node = Top.first;
	ChildItTy &It  = Top.second;

	while (It != GI::child_end(Node)) {
	  NodeType *Next = *It++;
	  if (!Visited.count(Next)) {  // Has our next sibling been visited?
	    // No, do it now.
	    Visited.insert(Next);
	    VisitStack.push(make_pair(Next, GI::child_begin(Next)));
	    return *this;
	  }
	}
	
	// Oops, ran out of successors... go up a level on the stack.
	VisitStack.pop();
      } while (!VisitStack.empty());
    }
    return *this; 
  }

  inline _Self operator++(int) { // Postincrement
    _Self tmp = *this; ++*this; return tmp; 
  }

  // nodeVisited - return true if this iterator has already visited the
  // specified node.  This is public, and will probably be used to iterate over
  // nodes that a depth first iteration did not find: ie unreachable nodes.
  //
  inline bool nodeVisited(NodeType *Node) const { 
    return Visited.count(Node) != 0;
  }
};

inline df_iterator df_begin(Method *M, bool Reverse = false) {
  return df_iterator(M->front(), Reverse);
}

inline df_const_iterator df_begin(const Method *M, bool Reverse = false) {
  return df_const_iterator(M->front(), Reverse);
}
inline df_iterator       df_end(Method*) { 
  return df_iterator(); 
}
inline df_const_iterator df_end(const Method*) {
  return df_const_iterator();
}

inline df_iterator df_begin(BasicBlock *BB, bool Reverse = false) { 
  return df_iterator(BB, Reverse);
}
inline df_const_iterator df_begin(const BasicBlock *BB, bool Reverse = false) { 
  return df_const_iterator(BB, Reverse);
}

inline df_iterator       df_end(BasicBlock*) { 
  return df_iterator(); 
}
inline df_const_iterator df_end(const BasicBlock*) {
  return df_const_iterator();
}



inline idf_iterator idf_begin(BasicBlock *BB, bool Reverse = false) { 
  return idf_iterator(BB, Reverse);
}
inline idf_const_iterator idf_begin(const BasicBlock *BB, bool Reverse = false) { 
  return idf_const_iterator(BB, Reverse);
}

inline idf_iterator       idf_end(BasicBlock*) { 
  return idf_iterator(); 
}
inline idf_const_iterator idf_end(const BasicBlock*) {
  return idf_const_iterator();
}




inline tdf_iterator tdf_begin(const Type *T, bool Reverse = false) {
  return tdf_iterator(T, Reverse);
}
inline tdf_iterator tdf_end  (const Type *T) {
  return tdf_iterator();
}




//===----------------------------------------------------------------------===//
// Post Order CFG iterator code
//

template<class BBType, class SuccItTy> 
class POIterator : public std::forward_iterator<BBType,	ptrdiff_t> {
  set<BBType *>   Visited;    // All of the blocks visited so far...
  // VisitStack - Used to maintain the ordering.  Top = current block
  // First element is basic block pointer, second is the 'next child' to visit
  stack<pair<BBType *, SuccItTy> > VisitStack;

  void traverseChild() {
    while (VisitStack.top().second != succ_end(VisitStack.top().first)) {
      BBType *BB = *VisitStack.top().second++;
      if (!Visited.count(BB)) {  // If the block is not visited...
	Visited.insert(BB);
	VisitStack.push(make_pair(BB, succ_begin(BB)));
      }
    }
  }

public:
  typedef POIterator<BBType, SuccItTy> _Self;

  inline POIterator(BBType *BB) {
    Visited.insert(BB);
    VisitStack.push(make_pair(BB, succ_begin(BB)));
    traverseChild();
  }
  inline POIterator() { /* End is when stack is empty */ }

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
  inline BBType *operator->() const { return operator*(); }

  inline _Self& operator++() {   // Preincrement
    VisitStack.pop();
    if (!VisitStack.empty())
      traverseChild();
    return *this; 
  }

  inline _Self operator++(int) { // Postincrement
    _Self tmp = *this; ++*this; return tmp; 
  }

  // Provide default begin and end methods when nothing special is needed.
  static inline _Self  begin  (BBType *BB) { return _Self(BB); }
  static inline _Self  end    (BBType *BB) { return _Self(); }
};

inline po_iterator       po_begin(      Method *M) {
  return po_iterator(M->front());
}
inline po_const_iterator po_begin(const Method *M) {
  return po_const_iterator(M->front());
}
inline po_iterator       po_end  (      Method *M) {
  return po_iterator();
}
inline po_const_iterator po_end  (const Method *M) {
  return po_const_iterator();
}

inline po_iterator       po_begin(      BasicBlock *BB) {
  return po_iterator(BB);
}
inline po_const_iterator po_begin(const BasicBlock *BB) {
  return po_const_iterator(BB);
}
inline po_iterator       po_end  (      BasicBlock *BB) {
  return po_iterator();
}
inline po_const_iterator po_end  (const BasicBlock *BB) {
  return po_const_iterator();
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

}    // End namespace cfg

#endif
