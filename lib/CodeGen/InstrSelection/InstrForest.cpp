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
//---------------------------------------------------------------------------

#include "llvm/CodeGen/InstrForest.h"
#include "llvm/CodeGen/MachineCodeForInstruction.h"
#include "llvm/Function.h"
#include "llvm/iTerminators.h"
#include "llvm/iMemory.h"
#include "llvm/Constant.h"
#include "llvm/Type.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "Support/STLExtras.h"
#include <alloca.h>
#include <iostream>
using std::cerr;
using std::vector;

//------------------------------------------------------------------------ 
// class InstrTreeNode
//------------------------------------------------------------------------ 

void
InstrTreeNode::dump(int dumpChildren, int indent) const
{
  dumpNode(indent);
  
  if (dumpChildren)
    {
      if (LeftChild)
	LeftChild->dump(dumpChildren, indent+1);
      if (RightChild)
	RightChild->dump(dumpChildren, indent+1);
    }
}


InstructionNode::InstructionNode(Instruction* I)
  : InstrTreeNode(NTInstructionNode, I),
    codeIsFoldedIntoParent(false)
{
  opLabel = I->getOpcode();

  // Distinguish special cases of some instructions such as Ret and Br
  // 
  if (opLabel == Instruction::Ret && cast<ReturnInst>(I)->getReturnValue())
    {
      opLabel = RetValueOp;              	 // ret(value) operation
    }
  else if (opLabel ==Instruction::Br && !cast<BranchInst>(I)->isUnconditional())
    {
      opLabel = BrCondOp;		// br(cond) operation
    }
  else if (opLabel >= Instruction::SetEQ && opLabel <= Instruction::SetGT)
    {
      opLabel = SetCCOp;		// common label for all SetCC ops
    }
  else if (opLabel == Instruction::Alloca && I->getNumOperands() > 0)
    {
      opLabel = AllocaN;		 // Alloca(ptr, N) operation
    }
  else if ((opLabel == Instruction::Load ||
	    opLabel == Instruction::GetElementPtr) &&
	   cast<MemAccessInst>(I)->hasIndices())
    {
      opLabel = opLabel + 100;		 // load/getElem with index vector
    }
  else if (opLabel == Instruction::And ||
           opLabel == Instruction::Or ||
           opLabel == Instruction::Xor ||
           opLabel == Instruction::Not)
    {
      // Distinguish bitwise operators from logical operators!
      if (I->getType() != Type::BoolTy)
        opLabel = opLabel + 100;	 // bitwise operator
    }
  else if (opLabel == Instruction::Cast)
    {
      const Type *ITy = I->getType();
      switch(ITy->getPrimitiveID())
	{
	case Type::BoolTyID:    opLabel = ToBoolTy;    break;
	case Type::UByteTyID:   opLabel = ToUByteTy;   break;
	case Type::SByteTyID:   opLabel = ToSByteTy;   break;
	case Type::UShortTyID:  opLabel = ToUShortTy;  break;
	case Type::ShortTyID:   opLabel = ToShortTy;   break;
	case Type::UIntTyID:    opLabel = ToUIntTy;    break;
	case Type::IntTyID:     opLabel = ToIntTy;     break;
	case Type::ULongTyID:   opLabel = ToULongTy;   break;
	case Type::LongTyID:    opLabel = ToLongTy;    break;
	case Type::FloatTyID:   opLabel = ToFloatTy;   break;
	case Type::DoubleTyID:  opLabel = ToDoubleTy;  break;
	case Type::ArrayTyID:   opLabel = ToArrayTy;   break;
	case Type::PointerTyID: opLabel = ToPointerTy; break;
	default:
	  // Just use `Cast' opcode otherwise. It's probably ignored.
	  break;
	}
    }
}


void
InstructionNode::dumpNode(int indent) const
{
  for (int i=0; i < indent; i++)
    cerr << "    ";
  
  cerr << getInstruction()->getOpcodeName();
  const MachineCodeForInstruction &mvec =
    MachineCodeForInstruction::get(getInstruction());

  if (mvec.size() > 0)
    cerr << "\tMachine Instructions:  ";

  for (unsigned int i=0; i < mvec.size(); ++i) {
    mvec[i]->dump(0);
    if (i < mvec.size() - 1)
      cerr << ";  ";
  }
  
  cerr << "\n";
}


void
VRegListNode::dumpNode(int indent) const
{
  for (int i=0; i < indent; i++)
    cerr << "    ";
  
  cerr << "List" << "\n";
}


void
VRegNode::dumpNode(int indent) const
{
  for (int i=0; i < indent; i++)
    cerr << "    ";
  
  cerr << "VReg " << getValue() << "\t(type "
       << (int) getValue()->getValueType() << ")" << "\n";
}

void
ConstantNode::dumpNode(int indent) const
{
  for (int i=0; i < indent; i++)
    cerr << "    ";
  
  cerr << "Constant " << getValue() << "\t(type "
       << (int) getValue()->getValueType() << ")" << "\n";
}

void
LabelNode::dumpNode(int indent) const
{
  for (int i=0; i < indent; i++)
    cerr << "    ";
  
  cerr << "Label " << getValue() << "\n";
}

//------------------------------------------------------------------------
// class InstrForest
// 
// A forest of instruction trees, usually for a single method.
//------------------------------------------------------------------------ 

InstrForest::InstrForest(Function *F)
{
  for (Function::iterator BB = F->begin(), FE = F->end(); BB != FE; ++BB) {
    for(BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E; ++I)
      buildTreeForInstruction(I);
  }
}

InstrForest::~InstrForest()
{
  for_each(treeRoots.begin(), treeRoots.end(), deleter<InstructionNode>);
}

void
InstrForest::dump() const
{
  for (const_root_iterator I = roots_begin(); I != roots_end(); ++I)
    (*I)->dump(/*dumpChildren*/ 1, /*indent*/ 0);
}

inline void
InstrForest::eraseRoot(InstructionNode* node)
{
  for (RootSet::reverse_iterator RI=treeRoots.rbegin(), RE=treeRoots.rend();
       RI != RE; ++RI)
    if (*RI == node)
      treeRoots.erase(RI.base()-1);
}

inline void
InstrForest::noteTreeNodeForInstr(Instruction *instr,
				  InstructionNode *treeNode)
{
  assert(treeNode->getNodeType() == InstrTreeNode::NTInstructionNode);
  (*this)[instr] = treeNode;
  treeRoots.push_back(treeNode);	// mark node as root of a new tree
}


inline void
InstrForest::setLeftChild(InstrTreeNode *parent, InstrTreeNode *child)
{
  parent->LeftChild = child;
  child->Parent = parent;
  if (child->getNodeType() == InstrTreeNode::NTInstructionNode)
    eraseRoot((InstructionNode*) child); // no longer a tree root
}

inline void
InstrForest::setRightChild(InstrTreeNode *parent, InstrTreeNode *child)
{
  parent->RightChild = child;
  child->Parent = parent;
  if (child->getNodeType() == InstrTreeNode::NTInstructionNode)
    eraseRoot((InstructionNode*) child); // no longer a tree root
}


InstructionNode*
InstrForest::buildTreeForInstruction(Instruction *instr)
{
  InstructionNode *treeNode = getTreeNodeForInstr(instr);
  if (treeNode)
    {
      // treeNode has already been constructed for this instruction
      assert(treeNode->getInstruction() == instr);
      return treeNode;
    }
  
  // Otherwise, create a new tree node for this instruction.
  // 
  treeNode = new InstructionNode(instr);
  noteTreeNodeForInstr(instr, treeNode);
  
  if (instr->getOpcode() == Instruction::Call)
    { // Operands of call instruction
      return treeNode;
    }
  
  // If the instruction has more than 2 instruction operands,
  // then we need to create artificial list nodes to hold them.
  // (Note that we only count operands that get tree nodes, and not
  // others such as branch labels for a branch or switch instruction.)
  //
  // To do this efficiently, we'll walk all operands, build treeNodes
  // for all appropriate operands and save them in an array.  We then
  // insert children at the end, creating list nodes where needed.
  // As a performance optimization, allocate a child array only
  // if a fixed array is too small.
  // 
  int numChildren = 0;
  InstrTreeNode **childArray =
    (InstrTreeNode **)alloca(instr->getNumOperands()*sizeof(InstrTreeNode *));
  
  //
  // Walk the operands of the instruction
  // 
  for (Instruction::op_iterator O = instr->op_begin(); O!=instr->op_end(); ++O)
    {
      Value* operand = *O;
      
      // Check if the operand is a data value, not an branch label, type,
      // method or module.  If the operand is an address type (i.e., label
      // or method) that is used in an non-branching operation, e.g., `add'.
      // that should be considered a data value.
    
      // Check latter condition here just to simplify the next IF.
      bool includeAddressOperand =
	(isa<BasicBlock>(operand) || isa<Function>(operand))
	&& !instr->isTerminator();
    
      if (includeAddressOperand || isa<Instruction>(operand) ||
	  isa<Constant>(operand) || isa<Argument>(operand) ||
	  isa<GlobalVariable>(operand))
	{
	  // This operand is a data value
	
	  // An instruction that computes the incoming value is added as a
	  // child of the current instruction if:
	  //   the value has only a single use
	  //   AND both instructions are in the same basic block.
	  //   AND the current instruction is not a PHI (because the incoming
	  //		value is conceptually in a predecessor block,
	  //		even though it may be in the same static block)
	  // 
	  // (Note that if the value has only a single use (viz., `instr'),
	  //  the def of the value can be safely moved just before instr
	  //  and therefore it is safe to combine these two instructions.)
	  // 
	  // In all other cases, the virtual register holding the value
	  // is used directly, i.e., made a child of the instruction node.
	  // 
	  InstrTreeNode* opTreeNode;
	  if (isa<Instruction>(operand) && operand->use_size() == 1 &&
	      cast<Instruction>(operand)->getParent() == instr->getParent() &&
	      instr->getOpcode() != Instruction::PHINode &&
	      instr->getOpcode() != Instruction::Call)
	    {
	      // Recursively create a treeNode for it.
	      opTreeNode = buildTreeForInstruction((Instruction*)operand);
	    }
	  else if (Constant *CPV = dyn_cast<Constant>(operand))
	    {
	      // Create a leaf node for a constant
	      opTreeNode = new ConstantNode(CPV);
	    }
	  else
	    {
	      // Create a leaf node for the virtual register
	      opTreeNode = new VRegNode(operand);
	    }

	  childArray[numChildren++] = opTreeNode;
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
  
  InstrTreeNode *parent = treeNode;
  
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
    setLeftChild(parent, childArray[0]);

  int n;
  
  // Create a list node for children 2 .. N-1, if any
  for (n = numChildren-1; n >= 2; n--)
    {
      // We have more than two children
      InstrTreeNode *listNode = new VRegListNode();
      setRightChild(parent, listNode);
      setLeftChild(listNode, childArray[numChildren - n]);
      parent = listNode;
    }
  
  // Now insert the last remaining child (if any).
  if (numChildren >= 2)
    {
      assert(n == 1);
      setRightChild(parent, childArray[numChildren - 1]);
    }
  
  return treeNode;
}
