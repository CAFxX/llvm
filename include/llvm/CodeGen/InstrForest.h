/* $Id$ -*-c++-*-
 ****************************************************************************
 * File:
 *	InstrForest.h
 * 
 * Purpose:
 *	Convert SSA graph to instruction trees for instruction selection.
 * 
 * Strategy:
 *  The basic idea is that we would like to group instructions into a single
 *  tree if one or more of them might be potentially combined into a single
 *  complex instruction in the target machine.
 *  Since this grouping is completely machine-independent, it is as
 *  aggressive as possible.  In particular, we group two instructions
 *  O and I if:
 *  (1) Instruction O computes an operand of instruction I, and
 *  (2) O and I are part of the same basic block, and
 *  (3) O has only a single use, viz., I.
 * 
 * History:
 *	6/28/01	 -  Vikram Adve  -  Created
 ***************************************************************************/

#ifndef LLVM_CODEGEN_INSTRFOREST_H
#define LLVM_CODEGEN_INSTRFOREST_H

#include "llvm/Support/NonCopyable.h"
#include "llvm/Support/HashExtras.h"
#include "llvm/Instruction.h"
#include <hash_map>
#include <hash_set>

class ConstPoolVal;
class BasicBlock;
class Method;
class InstrTreeNode;
class InstrForest;

//--------------------------------------------------------------------------
// OpLabel values for special-case nodes created for instruction selection.
// All op-labels not defined here are identical to the instruction
// opcode returned by Instruction::getInstType()
//--------------------------------------------------------------------------

const int  InvalidOp	=  -1;
const int  VRegListOp   =  97;
const int  VRegNodeOp	=  98;
const int  ConstantNodeOp= 99;
const int  LabelNodeOp	= 100;

const int  RetValueOp	= 100 + Instruction::Ret;
const int  BrCondOp	= 100 + Instruction::Br;

const int  SetCCOp	= 100 + Instruction::SetEQ;

const int  AllocaN	= 100 + Instruction::Alloca;		// 121
const int  LoadIdx	= 100 + Instruction::Load;		// 122
const int  GetElemPtrIdx= 100 + Instruction::GetElementPtr;	// 124

const int  ToBoolTy	= 100 + Instruction::Cast;		// 126
const int  ToUByteTy	= ToBoolTy +  1;
const int  ToSByteTy	= ToBoolTy +  2;
const int  ToUShortTy	= ToBoolTy +  3;
const int  ToShortTy	= ToBoolTy +  4;
const int  ToUIntTy	= ToBoolTy +  5;
const int  ToIntTy	= ToBoolTy +  6;
const int  ToULongTy	= ToBoolTy +  7;
const int  ToLongTy	= ToBoolTy +  8;
const int  ToFloatTy	= ToBoolTy +  9;
const int  ToDoubleTy	= ToBoolTy + 10;
const int  ToArrayTy	= ToBoolTy + 11;
const int  ToPointerTy	= ToBoolTy + 12;

//-------------------------------------------------------------------------
// Data types needed by BURG and implemented by us
//-------------------------------------------------------------------------

typedef int OpLabel;
typedef int StateLabel;

//-------------------------------------------------------------------------
// Declarations of data and functions created by BURG
//-------------------------------------------------------------------------

extern short*		burm_nts[];
  
extern StateLabel	burm_label	(InstrTreeNode* p);
  
extern StateLabel	burm_state	(OpLabel op, StateLabel leftState,
					 StateLabel rightState);

extern StateLabel	burm_rule	(StateLabel state, int goalNT);
  
extern InstrTreeNode**  burm_kids	(InstrTreeNode* p, int eruleno,
					 InstrTreeNode* kids[]);
  
extern void		printcover	(InstrTreeNode*, int, int);
extern void		printtree	(InstrTreeNode*);
extern int		treecost	(InstrTreeNode*, int, int);
extern void		printMatches	(InstrTreeNode*);


//------------------------------------------------------------------------ 
// class InstrTreeNode
// 
// A single tree node in the instruction tree used for
// instruction selection via BURG.
//------------------------------------------------------------------------ 

class InstrTreeNode : public NonCopyableV {
public:
  enum InstrTreeNodeType { NTInstructionNode,
			   NTVRegListNode,
			   NTVRegNode,
			   NTConstNode,
			   NTLabelNode };
  
  // BASIC TREE NODE START
  InstrTreeNode* LeftChild;
  InstrTreeNode* RightChild;
  InstrTreeNode* Parent;
  OpLabel        opLabel;
  StateLabel     state;
  // BASIC TREE NODE END

protected:
  InstrTreeNodeType treeNodeType;
  Value*	   val;
  
public:
  /*ctor*/		InstrTreeNode	(InstrTreeNodeType nodeType,
					 Value* _val);
  /*dtor*/ virtual	~InstrTreeNode	() {}
  
  InstrTreeNodeType	getNodeType	() const { return treeNodeType; }
  
  Value*		getValue	() const { return val; }
  
  inline OpLabel	getOpLabel	() const { return opLabel; }
  
  inline InstrTreeNode*	leftChild	() const {
    return LeftChild;
  }
  
  // If right child is a list node, recursively get its *left* child
  inline InstrTreeNode* rightChild() const {
    return (!RightChild ? 0 : 
	    (RightChild->getOpLabel() == VRegListOp
	     ? RightChild->LeftChild : RightChild));
  }
  
  inline InstrTreeNode *parent() const {
    return Parent;
  }
  
  void			dump		(int dumpChildren,
					 int indent) const;
  
protected:
  virtual void		dumpNode	(int indent) const = 0;

  friend class InstrForest;
};


class InstructionNode: public InstrTreeNode {
public:
  /*ctor*/	InstructionNode		(Instruction* _instr);
  Instruction*	getInstruction		() const {
    assert(treeNodeType == NTInstructionNode);
    return (Instruction*) val;
  }
protected:
  virtual void		dumpNode	(int indent) const;
};


class VRegListNode: public InstrTreeNode {
public:
  /*ctor*/		VRegListNode	();
protected:
  virtual void		dumpNode	(int indent) const;
};


class VRegNode: public InstrTreeNode {
public:
  /*ctor*/		VRegNode	(Value* _val);
protected:
  virtual void		dumpNode	(int indent) const;
};


class ConstantNode: public InstrTreeNode {
public:
  /*ctor*/		ConstantNode	(ConstPoolVal* constVal);
  ConstPoolVal*		getConstVal	() const { return (ConstPoolVal*) val;}
protected:
  virtual void		dumpNode	( int indent) const;
};


class LabelNode: public InstrTreeNode {
public:
  /*ctor*/		LabelNode	(BasicBlock* _bblock);
  BasicBlock*		getBasicBlock	() const { return (BasicBlock*) val;}
protected:
  virtual void		dumpNode	(int indent) const;
};


//------------------------------------------------------------------------
// class InstrForest
// 
// A forest of instruction trees, usually for a single method.
//
// Methods:
//     buildTreesForMethod()	Builds the forest of trees for a method
//     getTreeNodeForInstr()	Returns the tree node for an Instruction
//     getRootSet()		Returns a set of root nodes for all the trees
// 
//------------------------------------------------------------------------ 

class InstrForest :
  public NonCopyable,
  private hash_map<const Instruction*, InstructionNode*> {
  
private:
  hash_set<InstructionNode*> treeRoots;
  
public:
  /*ctor*/	InstrForest		()	    {}
  /*dtor*/	~InstrForest		()	    {}
  
  void		buildTreesForMethod	(Method *method);
				    
  inline InstructionNode*
  getTreeNodeForInstr(Instruction* instr)
  {
    return (*this)[instr];
  }
  
  inline const hash_set<InstructionNode*> &getRootSet() const {
    return treeRoots;
  }
  
  void		dump			() const;
  
private:
  //
  // Private methods for buidling the instruction forest
  //
  void		setLeftChild		(InstrTreeNode* parent,
					 InstrTreeNode* child);
  
  void		setRightChild		(InstrTreeNode* parent,
					 InstrTreeNode* child);
  
  void		setParent		(InstrTreeNode* child,
					 InstrTreeNode* parent);
  
  void		noteTreeNodeForInstr	(Instruction* instr,
					 InstructionNode* treeNode);
  
  InstructionNode* buildTreeForInstruction(Instruction* instr);
};


/***************************************************************************/

#endif
