/* Title:   IGNode.h
   Author:  Ruchira Sasanka
   Date:    July 25, 01
   Purpose: Represents a node in an interference graph. 
   Notes:

   For efficiency, the AdjList is updated only once - ie. we can add but not
   remove nodes from AdjList. 

   The removal of nodes from IG is simulated by decrementing the CurDegree.
   If this node is put on stack (that is removed from IG), the CurDegree of all
   the neighbors are decremented and this node is marked OnSack. Hence
   the effective neighbors in the AdjList are the ones that do not have the
   OnStack flag set (therefore, they are in the IG).

   The methods that modify/use the CurDegree Must be called only
   fter all modifications to the IG are over (i.e., all neighbors are fixed).

   The vector representation the most efficient one for adj list.
   Though nodes are removed when coalsing is done, we access it in sequence
   for far many times when coloring (colorNode()).

*/

#ifndef IG_NODE_H
#define IG_NODE_H


#include "llvm/CodeGen/RegAllocCommon.h"
#include "llvm/CodeGen/LiveRange.h"

class IGNode
{
 private:

  const int Index;            // index within IGNodeList 

  bool OnStack;               // this has been pushed on to stack for coloring

  vector<IGNode *> AdjList;   // adjacency list for this live range


  // set by InterferenceGraph::setCurDegreeOfIGNodes() after calculating
  // all adjacency lists.
  // Decremented when a neighbor is pushed on to the stack. 
  // After that, never incremented/set again nor used.
  int CurDegree;     

  LiveRange *const ParentLR;            // parent LR (cannot be a const)


 public:

  inline unsigned int getIndex() const 
    { return Index; }

  // adjLists must be updated only once.  However, the CurDegree can be changed
  inline void addAdjIGNode( IGNode *const AdjNode) 
    { AdjList.push_back(AdjNode);  } 

  inline IGNode * getAdjIGNode(unsigned int ind) const 
    { assert ( ind < AdjList.size()); return AdjList[ ind ]; }

  // delete a node in AdjList - node must be in the list
  // should not be called often
  void delAdjIGNode(const IGNode *const Node); 

  inline unsigned int getNumOfNeighbors() const 
    { return AdjList.size() ; }


  inline bool isOnStack() const 
    { return OnStack; }

  // remove form IG and pushes on to stack (reduce the degree of neighbors)
  void pushOnStack(); 

  // CurDegree is the effective number of neighbors when neighbors are
  // pushed on to the stack during the coloring phase. Must be called
  // after all modifications to the IG are over (i.e., all neighbors are
  // fixed).

  inline void setCurDegree() 
    { assert( CurDegree == -1);   CurDegree = AdjList.size(); }

  inline int getCurDegree() const 
    { return CurDegree; }

  // called when a neigh is pushed on to stack
  inline void decCurDegree() 
    { assert( CurDegree > 0 ); --CurDegree; }


  // The following methods call the methods in ParentLR
  // They are added to this class for convenience
  // If many of these are called within a single scope,
  // consider calling the methods directly on LR


  inline void setRegClass(RegClass *const RC) 
    { ParentLR->setRegClass(RC);  }

  inline RegClass *const getRegClass() const 
    { return ParentLR->getRegClass(); } 

  inline bool hasColor() const 
    { return ParentLR->hasColor();  }

  inline unsigned int getColor() const 
    { return ParentLR->getColor();  }

  inline void setColor(unsigned int Col) 
    { ParentLR->setColor(Col);  }

  inline void markForSpill() 
    { ParentLR->markForSpill();  }

  inline void markForSaveAcrossCalls() 
    { ParentLR->markForSaveAcrossCalls();  }

  // inline void markForLoadFromStack() 
  //  { ParentLR->markForLoadFromStack();  }


  inline unsigned int getNumOfCallInterferences() const 
    { return ParentLR->getNumOfCallInterferences(); } 

  inline LiveRange *getParentLR() const 
    { return ParentLR; }

  inline Type::PrimitiveID getTypeID() const 
    { return ParentLR->getTypeID(); }



  //---- constructor and destructor ----


  IGNode(LiveRange *const LR, unsigned int index);

  ~IGNode() { }                         // an empty destructor


};







#endif
