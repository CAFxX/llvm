// $Id$
//---------------------------------------------------------------------------
// File:
//	InstrForest.cpp
// 
// Purpose:
//	Convert SSA graph to instruction trees for instruction selection.
// 
// Strategy:
//  The key goal is to group instructions into a single
//  tree if one or more of them might be potentially combined into a single
//  complex instruction in the target machine.
//  Since this grouping is completely machine-independent, we do it as
//  aggressive as possible to exploit any possible taret instructions.
//  In particular, we group two instructions O and I if:
//      (1) Instruction O computes an operand used by instruction I,
//  and (2) O and I are part of the same basic block,
//  and (3) O has only a single use, viz., I.
// 
// History:
//	6/28/01	 -  Vikram Adve  -  Created
// 
//---------------------------------------------------------------------------

//*************************** User Include Files ***************************/

#include "llvm/CodeGen/InstrForest.h"
#include "llvm/Method.h"
#include "llvm/iTerminators.h"
#include "llvm/iMemory.h"
#include "llvm/ConstPoolVals.h"
#include "llvm/BasicBlock.h"
#include "llvm/CodeGen/MachineInstr.h"


//------------------------------------------------------------------------ 
// class InstrTreeNode
//------------------------------------------------------------------------ 


InstrTreeNode::InstrTreeNode(InstrTreeNodeType nodeType, Value* _val)
  : treeNodeType(nodeType), val(_val) {
  LeftChild   = 0;
  RightChild  = 0;
  Parent      = 0;
  opLabel     = InvalidOp;
}

void InstrTreeNode::dump(int dumpChildren, int indent) const {
  dumpNode(indent);
  
  if (dumpChildren)
    {
      if (leftChild())
	leftChild()->dump(dumpChildren, indent+1);
      if (rightChild())
	rightChild()->dump(dumpChildren, indent+1);
    }
}


InstructionNode::InstructionNode(Instruction* _instr)
  : InstrTreeNode(NTInstructionNode, _instr)
{
  OpLabel opLabel = _instr->getOpcode();

  // Distinguish special cases of some instructions such as Ret and Br
  // 
  if (opLabel == Instruction::Ret && ((ReturnInst*) _instr)->getReturnValue())
    {
      opLabel = RetValueOp;		 // ret(value) operation
    }
  else if (opLabel == Instruction::Br && ! ((BranchInst*) _instr)->isUnconditional())
    {
      opLabel = BrCondOp;		// br(cond) operation
    }
  else if (opLabel >= Instruction::SetEQ && opLabel <= Instruction::SetGT)
    {
      opLabel = SetCCOp;		// common label for all SetCC ops
    }
  else if (opLabel == Instruction::Alloca && _instr->getNumOperands() > 0)
    {
      opLabel = AllocaN;		 // Alloca(ptr, N) operation
    }
  else if ((opLabel == Instruction::Load ||
	    opLabel == Instruction::GetElementPtr)
	   && ((MemAccessInst*)_instr)->getFirstOffsetIdx() > 0)
    {
      opLabel = opLabel + 100;		 // load/getElem with index vector
    }
  else if (opLabel == Instruction::Cast)
    {
      const Type* instrValueType = _instr->getType();
      switch(instrValueType->getPrimitiveID())
	{
	case Type::BoolTyID:	opLabel = ToBoolTy;  break;
	case Type::UByteTyID:	opLabel = ToUByteTy; break;
	case Type::SByteTyID:	opLabel = ToSByteTy; break;
	case Type::UShortTyID:	opLabel = ToUShortTy; break;
	case Type::ShortTyID:	opLabel = ToShortTy; break;
	case Type::UIntTyID:	opLabel = ToUIntTy; break;
	case Type::IntTyID:	opLabel = ToIntTy; break;
	case Type::ULongTyID:	opLabel = ToULongTy; break;
	case Type::LongTyID:	opLabel = ToLongTy; break;
	case Type::FloatTyID:	opLabel = ToFloatTy; break;
	case Type::DoubleTyID:	opLabel = ToDoubleTy; break;
	default:
	  if (instrValueType->isArrayType())
	    opLabel = ToArrayTy;
	  else if (instrValueType->isPointerType())
	    opLabel = ToPointerTy;
	  else
	    ; // Just use `Cast' opcode otherwise. It's probably ignored.
	  break;
	}
    }
  
  this->opLabel = opLabel;
}


void
InstructionNode::dumpNode(int indent) const
{
  for (int i=0; i < indent; i++)
    cout << "    ";
  
  cout << getInstruction()->getOpcodeName();
  
  const vector<MachineInstr*>& mvec = getInstruction()->getMachineInstrVec();
  if (mvec.size() > 0)
    cout << "\tMachine Instructions:  ";
  for (unsigned int i=0; i < mvec.size(); i++)
    {
      mvec[i]->dump(0);
      if (i < mvec.size() - 1)
	cout << ";  ";
    }
  
  cout << endl;
}


VRegListNode::VRegListNode() : InstrTreeNode(NTVRegListNode, 0) {
  opLabel = VRegListOp;
}

void
VRegListNode::dumpNode(int indent) const
{
  for (int i=0; i < indent; i++)
    cout << "    ";
  
  cout << "List" << endl;
}


VRegNode::VRegNode(Value* _val) : InstrTreeNode(NTVRegNode, _val) {
  opLabel = VRegNodeOp;
}

void
VRegNode::dumpNode(int indent) const
{
  for (int i=0; i < indent; i++)
    cout << "    ";
  
  cout << "VReg " << getValue() << "\t(type "
       << (int) getValue()->getValueType() << ")" << endl;
}


ConstantNode::ConstantNode(ConstPoolVal *constVal)
  : InstrTreeNode(NTConstNode, constVal) {
  opLabel = ConstantNodeOp;
}

void
ConstantNode::dumpNode(int indent) const
{
  for (int i=0; i < indent; i++)
    cout << "    ";
  
  cout << "Constant " << getValue() << "\t(type "
       << (int) getValue()->getValueType() << ")" << endl;
}


LabelNode::LabelNode(BasicBlock *BB) : InstrTreeNode(NTLabelNode, BB) {
  opLabel = LabelNodeOp;
}

void
LabelNode::dumpNode(int indent) const
{
  for (int i=0; i < indent; i++)
    cout << "    ";
  
  cout << "Label " << getValue() << endl;
}

//------------------------------------------------------------------------
// class InstrForest
// 
// A forest of instruction trees, usually for a single method.
//------------------------------------------------------------------------ 

void
InstrForest::buildTreesForMethod(Method *method)
{
  for (Method::inst_iterator instrIter = method->inst_begin();
       instrIter != method->inst_end();
       ++instrIter)
    {
      Instruction *instr = *instrIter;
      (void) this->buildTreeForInstruction(instr);
    } 
}


void
InstrForest::dump() const
{
  for (hash_set<InstructionNode*>::const_iterator
	 treeRootIter = treeRoots.begin();
       treeRootIter != treeRoots.end();
       ++treeRootIter)
    {
      (*treeRootIter)->dump(/*dumpChildren*/ 1, /*indent*/ 0);
    }
}

inline void
InstrForest::noteTreeNodeForInstr(Instruction* instr,
				  InstructionNode* treeNode)
{
  assert(treeNode->getNodeType() == InstrTreeNode::NTInstructionNode);
  (*this)[instr] = treeNode;
  treeRoots.insert(treeNode);		// mark node as root of a new tree
}


inline void
InstrForest::setLeftChild(InstrTreeNode* parent, InstrTreeNode* child) {
  parent->LeftChild = child;
  child->Parent = parent;
  if (child->getNodeType() == InstrTreeNode::NTInstructionNode)
    treeRoots.erase((InstructionNode*) child);	// no longer a tree root
}


inline void
InstrForest::setRightChild(InstrTreeNode* parent, InstrTreeNode* child)
{
  parent->RightChild = child;
  child->Parent = parent;
  if (child->getNodeType() == InstrTreeNode::NTInstructionNode)
    treeRoots.erase((InstructionNode*) child);	// no longer a tree root
}


InstructionNode*
InstrForest::buildTreeForInstruction(Instruction* instr)
{
  InstructionNode* treeNode = this->getTreeNodeForInstr(instr);
  if (treeNode != NULL)
    {// treeNode has already been constructed for this instruction
      assert(treeNode->getInstruction() == instr);
      return treeNode;
    }
  
  // Otherwise, create a new tree node for this instruction.
  // 
  treeNode = new InstructionNode(instr);
  this->noteTreeNodeForInstr(instr, treeNode);
  
  // If the instruction has more than 2 instruction operands,
  // then we need to create artificial list nodes to hold them.
  // (Note that we only not count operands that get tree nodes, and not
  // others such as branch labels for a branch or switch instruction.)
  //
  // To do this efficiently, we'll walk all operands, build treeNodes
  // for all appropriate operands and save them in an array.  We then
  // insert children at the end, creating list nodes where needed.
  // As a performance optimization, allocate a child array only
  // if a fixed array is too small.
  // 
  int numChildren = 0;
  const unsigned int MAX_CHILD = 8;
  static InstrTreeNode* fixedChildArray[MAX_CHILD];
  InstrTreeNode** childArray =
    (instr->getNumOperands() > MAX_CHILD)
    ? new (InstrTreeNode*)[instr->getNumOperands()]
    : fixedChildArray;
  
  //
  // Walk the operands of the instruction
  // 
  for (Instruction::op_iterator O=instr->op_begin(); O != instr->op_end(); ++O)
    {
      Value* operand = *O;
      
      // Check if the operand is a data value, not an branch label, type,
      // method or module.  If the operand is an address type (i.e., label
      // or method) that is used in an non-branching operation, e.g., `add'.
      // that should be considered a data value.
      
      // Check latter condition here just to simplify the next IF.
      bool includeAddressOperand =
	((operand->isBasicBlock() || operand->isMethod())
	 && !instr->isTerminator());
	 
      if (includeAddressOperand || operand->isInstruction() ||
	  operand->isConstant() || operand->isMethodArgument())
	{// This operand is a data value
	  
	  // An instruction that computes the incoming value is added as a
	  // child of the current instruction if:
	  //   the value has only a single use
	  //   AND both instructions are in the same basic block.
	  // 
	  // (Note that if the value has only a single use (viz., `instr'),
	  //  the def of the value can be safely moved just before instr
	  //  and therefore it is safe to combine these two instructions.)
	  // 
	  // In all other cases, the virtual register holding the value
	  // is used directly, i.e., made a child of the instruction node.
	  // 
	  InstrTreeNode* opTreeNode;
	  if (operand->isInstruction() && operand->use_size() == 1 &&
	      ((Instruction*)operand)->getParent() == instr->getParent())
	    {
	      // Recursively create a treeNode for it.
	      opTreeNode =this->buildTreeForInstruction((Instruction*)operand);
	    }
	  else if (ConstPoolVal *CPV = operand->castConstant())
	    {
	      // Create a leaf node for a constant
	      opTreeNode = new ConstantNode(CPV);
	    }
	  else
	    {
	      // Create a leaf node for the virtual register
	      opTreeNode = new VRegNode(operand);
	    }
	  
	  childArray[numChildren] = opTreeNode;
	  numChildren++;
	}
    }
  
  //-------------------------------------------------------------------- 
  // Add any selected operands as children in the tree.
  // Certain instructions can have more than 2 in some instances (viz.,
  // a CALL or a memory access -- LOAD, STORE, and GetElemPtr -- to an
  // array or struct). Make the operands of every such instruction into
  // a right-leaning binary tree with the operand nodes at the leaves
  // and VRegList nodes as internal nodes.
  //-------------------------------------------------------------------- 
  
  InstrTreeNode* parent = treeNode;		// new VRegListNode();
  int n;
  
  if (numChildren > 2)
    {
      unsigned instrOpcode = treeNode->getInstruction()->getOpcode();
      assert(instrOpcode == Instruction::PHINode ||
	     instrOpcode == Instruction::Call ||
	     instrOpcode == Instruction::Load ||
	     instrOpcode == Instruction::Store ||
	     instrOpcode == Instruction::GetElementPtr);
    }
  
  // Insert the first child as a direct child
  if (numChildren >= 1)
    this->setLeftChild(parent, childArray[0]);
  
  // Create a list node for children 2 .. N-1, if any
  for (n = numChildren-1; n >= 2; n--)
    { // We have more than two children
      InstrTreeNode* listNode = new VRegListNode();
      this->setRightChild(parent, listNode);
      this->setLeftChild(listNode, childArray[numChildren - n]);
      parent = listNode;
    }
  
  // Now insert the last remaining child (if any).
  if (numChildren >= 2)
    {
      assert(n == 1);
      this->setRightChild(parent, childArray[numChildren - 1]);
    }
  
  if (childArray != fixedChildArray)
    {
      delete[] childArray; 
    }
  
  return treeNode;
}

