//===-- LegalizeDAG.cpp - Implement SelectionDAG::Legalize ----------------===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file implements the SelectionDAG::Legalize method.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/Target/TargetLowering.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Constants.h"
#include <iostream>
using namespace llvm;

//===----------------------------------------------------------------------===//
/// SelectionDAGLegalize - This takes an arbitrary SelectionDAG as input and
/// hacks on it until the target machine can handle it.  This involves
/// eliminating value sizes the machine cannot handle (promoting small sizes to
/// large sizes or splitting up large values into small values) as well as
/// eliminating operations the machine cannot handle.
///
/// This code also does a small amount of optimization and recognition of idioms
/// as part of its processing.  For example, if a target does not support a
/// 'setcc' instruction efficiently, but does support 'brcc' instruction, this
/// will attempt merge setcc and brc instructions into brcc's.
///
namespace {
class SelectionDAGLegalize {
  TargetLowering &TLI;
  SelectionDAG &DAG;

  /// LegalizeAction - This enum indicates what action we should take for each
  /// value type the can occur in the program.
  enum LegalizeAction {
    Legal,            // The target natively supports this value type.
    Promote,          // This should be promoted to the next larger type.
    Expand,           // This integer type should be broken into smaller pieces.
  };

  /// ValueTypeActions - This is a bitvector that contains two bits for each
  /// value type, where the two bits correspond to the LegalizeAction enum.
  /// This can be queried with "getTypeAction(VT)".
  unsigned ValueTypeActions;

  /// NeedsAnotherIteration - This is set when we expand a large integer
  /// operation into smaller integer operations, but the smaller operations are
  /// not set.  This occurs only rarely in practice, for targets that don't have
  /// 32-bit or larger integer registers.
  bool NeedsAnotherIteration;

  /// LegalizedNodes - For nodes that are of legal width, and that have more
  /// than one use, this map indicates what regularized operand to use.  This
  /// allows us to avoid legalizing the same thing more than once.
  std::map<SDOperand, SDOperand> LegalizedNodes;

  /// PromotedNodes - For nodes that are below legal width, and that have more
  /// than one use, this map indicates what promoted value to use.  This allows
  /// us to avoid promoting the same thing more than once.
  std::map<SDOperand, SDOperand> PromotedNodes;

  /// ExpandedNodes - For nodes that need to be expanded, and which have more
  /// than one use, this map indicates which which operands are the expanded
  /// version of the input.  This allows us to avoid expanding the same node
  /// more than once.
  std::map<SDOperand, std::pair<SDOperand, SDOperand> > ExpandedNodes;

  void AddLegalizedOperand(SDOperand From, SDOperand To) {
    bool isNew = LegalizedNodes.insert(std::make_pair(From, To)).second;
    assert(isNew && "Got into the map somehow?");
  }
  void AddPromotedOperand(SDOperand From, SDOperand To) {
    bool isNew = PromotedNodes.insert(std::make_pair(From, To)).second;
    assert(isNew && "Got into the map somehow?");
  }

public:

  SelectionDAGLegalize(SelectionDAG &DAG);

  /// Run - While there is still lowering to do, perform a pass over the DAG.
  /// Most regularization can be done in a single pass, but targets that require
  /// large values to be split into registers multiple times (e.g. i64 -> 4x
  /// i16) require iteration for these values (the first iteration will demote
  /// to i32, the second will demote to i16).
  void Run() {
    do {
      NeedsAnotherIteration = false;
      LegalizeDAG();
    } while (NeedsAnotherIteration);
  }

  /// getTypeAction - Return how we should legalize values of this type, either
  /// it is already legal or we need to expand it into multiple registers of
  /// smaller integer type, or we need to promote it to a larger type.
  LegalizeAction getTypeAction(MVT::ValueType VT) const {
    return (LegalizeAction)((ValueTypeActions >> (2*VT)) & 3);
  }

  /// isTypeLegal - Return true if this type is legal on this target.
  ///
  bool isTypeLegal(MVT::ValueType VT) const {
    return getTypeAction(VT) == Legal;
  }

private:
  void LegalizeDAG();

  SDOperand LegalizeOp(SDOperand O);
  void ExpandOp(SDOperand O, SDOperand &Lo, SDOperand &Hi);
  SDOperand PromoteOp(SDOperand O);

  SDOperand ExpandLibCall(const char *Name, SDNode *Node,
                          SDOperand &Hi);
  SDOperand ExpandIntToFP(bool isSigned, MVT::ValueType DestTy,
                          SDOperand Source);
  bool ExpandShift(unsigned Opc, SDOperand Op, SDOperand Amt,
                   SDOperand &Lo, SDOperand &Hi);
  void ExpandAddSub(bool isAdd, SDOperand Op, SDOperand Amt,
                    SDOperand &Lo, SDOperand &Hi);

  SDOperand getIntPtrConstant(uint64_t Val) {
    return DAG.getConstant(Val, TLI.getPointerTy());
  }
};
}


SelectionDAGLegalize::SelectionDAGLegalize(SelectionDAG &dag)
  : TLI(dag.getTargetLoweringInfo()), DAG(dag),
    ValueTypeActions(TLI.getValueTypeActions()) {
  assert(MVT::LAST_VALUETYPE <= 16 &&
         "Too many value types for ValueTypeActions to hold!");
}

void SelectionDAGLegalize::LegalizeDAG() {
  SDOperand OldRoot = DAG.getRoot();
  SDOperand NewRoot = LegalizeOp(OldRoot);
  DAG.setRoot(NewRoot);

  ExpandedNodes.clear();
  LegalizedNodes.clear();
  PromotedNodes.clear();

  // Remove dead nodes now.
  DAG.RemoveDeadNodes(OldRoot.Val);
}

SDOperand SelectionDAGLegalize::LegalizeOp(SDOperand Op) {
  assert(getTypeAction(Op.getValueType()) == Legal &&
         "Caller should expand or promote operands that are not legal!");

  // If this operation defines any values that cannot be represented in a
  // register on this target, make sure to expand or promote them.
  if (Op.Val->getNumValues() > 1) {
    for (unsigned i = 0, e = Op.Val->getNumValues(); i != e; ++i)
      switch (getTypeAction(Op.Val->getValueType(i))) {
      case Legal: break;  // Nothing to do.
      case Expand: {
        SDOperand T1, T2;
        ExpandOp(Op.getValue(i), T1, T2);
        assert(LegalizedNodes.count(Op) &&
               "Expansion didn't add legal operands!");
        return LegalizedNodes[Op];
      }
      case Promote:
        PromoteOp(Op.getValue(i));
        assert(LegalizedNodes.count(Op) &&
               "Expansion didn't add legal operands!");
        return LegalizedNodes[Op];
      }
  }

  std::map<SDOperand, SDOperand>::iterator I = LegalizedNodes.find(Op);
  if (I != LegalizedNodes.end()) return I->second;

  SDOperand Tmp1, Tmp2, Tmp3;

  SDOperand Result = Op;
  SDNode *Node = Op.Val;

  switch (Node->getOpcode()) {
  default:
    std::cerr << "NODE: "; Node->dump(); std::cerr << "\n";
    assert(0 && "Do not know how to legalize this operator!");
    abort();
  case ISD::EntryToken:
  case ISD::FrameIndex:
  case ISD::GlobalAddress:
  case ISD::ExternalSymbol:
  case ISD::ConstantPool:           // Nothing to do.
    assert(getTypeAction(Node->getValueType(0)) == Legal &&
           "This must be legal!");
    break;
  case ISD::CopyFromReg:
    Tmp1 = LegalizeOp(Node->getOperand(0));
    if (Tmp1 != Node->getOperand(0))
      Result = DAG.getCopyFromReg(cast<RegSDNode>(Node)->getReg(),
                                  Node->getValueType(0), Tmp1);
    break;
  case ISD::ImplicitDef:
    Tmp1 = LegalizeOp(Node->getOperand(0));
    if (Tmp1 != Node->getOperand(0))
      Result = DAG.getImplicitDef(Tmp1, cast<RegSDNode>(Node)->getReg());
    break;
  case ISD::Constant:
    // We know we don't need to expand constants here, constants only have one
    // value and we check that it is fine above.

    // FIXME: Maybe we should handle things like targets that don't support full
    // 32-bit immediates?
    break;
  case ISD::ConstantFP: {
    // Spill FP immediates to the constant pool if the target cannot directly
    // codegen them.  Targets often have some immediate values that can be
    // efficiently generated into an FP register without a load.  We explicitly
    // leave these constants as ConstantFP nodes for the target to deal with.

    ConstantFPSDNode *CFP = cast<ConstantFPSDNode>(Node);

    // Check to see if this FP immediate is already legal.
    bool isLegal = false;
    for (TargetLowering::legal_fpimm_iterator I = TLI.legal_fpimm_begin(),
           E = TLI.legal_fpimm_end(); I != E; ++I)
      if (CFP->isExactlyValue(*I)) {
        isLegal = true;
        break;
      }

    if (!isLegal) {
      // Otherwise we need to spill the constant to memory.
      MachineConstantPool *CP = DAG.getMachineFunction().getConstantPool();

      bool Extend = false;

      // If a FP immediate is precise when represented as a float, we put it
      // into the constant pool as a float, even if it's is statically typed
      // as a double.
      MVT::ValueType VT = CFP->getValueType(0);
      bool isDouble = VT == MVT::f64;
      ConstantFP *LLVMC = ConstantFP::get(isDouble ? Type::DoubleTy :
                                             Type::FloatTy, CFP->getValue());
      if (isDouble && CFP->isExactlyValue((float)CFP->getValue())) {
        LLVMC = cast<ConstantFP>(ConstantExpr::getCast(LLVMC, Type::FloatTy));
        VT = MVT::f32;
        Extend = true;
      }
      
      SDOperand CPIdx = DAG.getConstantPool(CP->getConstantPoolIndex(LLVMC),
                                            TLI.getPointerTy());
      if (Extend) {
        Result = DAG.getNode(ISD::EXTLOAD, MVT::f64, DAG.getEntryNode(), CPIdx,
                             MVT::f32);
      } else {
        Result = DAG.getLoad(VT, DAG.getEntryNode(), CPIdx);
      }
    }
    break;
  }
  case ISD::TokenFactor: {
    std::vector<SDOperand> Ops;
    bool Changed = false;
    for (unsigned i = 0, e = Node->getNumOperands(); i != e; ++i) {
      SDOperand Op = Node->getOperand(i);
      // Fold single-use TokenFactor nodes into this token factor as we go.
      if (Op.getOpcode() == ISD::TokenFactor && Op.hasOneUse()) {
        Changed = true;
        for (unsigned j = 0, e = Op.getNumOperands(); j != e; ++j)
          Ops.push_back(LegalizeOp(Op.getOperand(j)));
      } else {
        Ops.push_back(LegalizeOp(Op));  // Legalize the operands
        Changed |= Ops[i] != Op;
      }
    }
    if (Changed)
      Result = DAG.getNode(ISD::TokenFactor, MVT::Other, Ops);
    break;
  }

  case ISD::ADJCALLSTACKDOWN:
  case ISD::ADJCALLSTACKUP:
    Tmp1 = LegalizeOp(Node->getOperand(0));  // Legalize the chain.
    // There is no need to legalize the size argument (Operand #1)
    if (Tmp1 != Node->getOperand(0))
      Result = DAG.getNode(Node->getOpcode(), MVT::Other, Tmp1,
                           Node->getOperand(1));
    break;
  case ISD::DYNAMIC_STACKALLOC:
    Tmp1 = LegalizeOp(Node->getOperand(0));  // Legalize the chain.
    Tmp2 = LegalizeOp(Node->getOperand(1));  // Legalize the size.
    Tmp3 = LegalizeOp(Node->getOperand(2));  // Legalize the alignment.
    if (Tmp1 != Node->getOperand(0) || Tmp2 != Node->getOperand(1) ||
        Tmp3 != Node->getOperand(2))
      Result = DAG.getNode(ISD::DYNAMIC_STACKALLOC, Node->getValueType(0),
                           Tmp1, Tmp2, Tmp3);
    else
      Result = Op.getValue(0);

    // Since this op produces two values, make sure to remember that we
    // legalized both of them.
    AddLegalizedOperand(SDOperand(Node, 0), Result);
    AddLegalizedOperand(SDOperand(Node, 1), Result.getValue(1));
    return Result.getValue(Op.ResNo);

  case ISD::CALL: {
    Tmp1 = LegalizeOp(Node->getOperand(0));  // Legalize the chain.
    Tmp2 = LegalizeOp(Node->getOperand(1));  // Legalize the callee.

    bool Changed = false;
    std::vector<SDOperand> Ops;
    for (unsigned i = 2, e = Node->getNumOperands(); i != e; ++i) {
      Ops.push_back(LegalizeOp(Node->getOperand(i)));
      Changed |= Ops.back() != Node->getOperand(i);
    }

    if (Tmp1 != Node->getOperand(0) || Tmp2 != Node->getOperand(1) || Changed) {
      std::vector<MVT::ValueType> RetTyVTs;
      RetTyVTs.reserve(Node->getNumValues());
      for (unsigned i = 0, e = Node->getNumValues(); i != e; ++i)
        RetTyVTs.push_back(Node->getValueType(i));
      Result = SDOperand(DAG.getCall(RetTyVTs, Tmp1, Tmp2, Ops), 0);
    } else {
      Result = Result.getValue(0);
    }
    // Since calls produce multiple values, make sure to remember that we
    // legalized all of them.
    for (unsigned i = 0, e = Node->getNumValues(); i != e; ++i)
      AddLegalizedOperand(SDOperand(Node, i), Result.getValue(i));
    return Result.getValue(Op.ResNo);
  }
  case ISD::BR:
    Tmp1 = LegalizeOp(Node->getOperand(0));  // Legalize the chain.
    if (Tmp1 != Node->getOperand(0))
      Result = DAG.getNode(ISD::BR, MVT::Other, Tmp1, Node->getOperand(1));
    break;

  case ISD::BRCOND:
    Tmp1 = LegalizeOp(Node->getOperand(0));  // Legalize the chain.

    switch (getTypeAction(Node->getOperand(1).getValueType())) {
    case Expand: assert(0 && "It's impossible to expand bools");
    case Legal:
      Tmp2 = LegalizeOp(Node->getOperand(1)); // Legalize the condition.
      break;
    case Promote:
      Tmp2 = PromoteOp(Node->getOperand(1));  // Promote the condition.
      break;
    }
    // Basic block destination (Op#2) is always legal.
    if (Tmp1 != Node->getOperand(0) || Tmp2 != Node->getOperand(1))
      Result = DAG.getNode(ISD::BRCOND, MVT::Other, Tmp1, Tmp2,
                           Node->getOperand(2));
    break;

  case ISD::LOAD:
    Tmp1 = LegalizeOp(Node->getOperand(0));  // Legalize the chain.
    Tmp2 = LegalizeOp(Node->getOperand(1));  // Legalize the pointer.
    if (Tmp1 != Node->getOperand(0) ||
        Tmp2 != Node->getOperand(1))
      Result = DAG.getLoad(Node->getValueType(0), Tmp1, Tmp2);
    else
      Result = SDOperand(Node, 0);
    
    // Since loads produce two values, make sure to remember that we legalized
    // both of them.
    AddLegalizedOperand(SDOperand(Node, 0), Result);
    AddLegalizedOperand(SDOperand(Node, 1), Result.getValue(1));
    return Result.getValue(Op.ResNo);

  case ISD::EXTLOAD:
  case ISD::SEXTLOAD:
  case ISD::ZEXTLOAD:
    Tmp1 = LegalizeOp(Node->getOperand(0));  // Legalize the chain.
    Tmp2 = LegalizeOp(Node->getOperand(1));  // Legalize the pointer.
    if (Tmp1 != Node->getOperand(0) ||
        Tmp2 != Node->getOperand(1))
      Result = DAG.getNode(Node->getOpcode(), Node->getValueType(0), Tmp1, Tmp2,
                           cast<MVTSDNode>(Node)->getExtraValueType());
    else
      Result = SDOperand(Node, 0);
    
    // Since loads produce two values, make sure to remember that we legalized
    // both of them.
    AddLegalizedOperand(SDOperand(Node, 0), Result);
    AddLegalizedOperand(SDOperand(Node, 1), Result.getValue(1));
    return Result.getValue(Op.ResNo);

  case ISD::EXTRACT_ELEMENT:
    // Get both the low and high parts.
    ExpandOp(Node->getOperand(0), Tmp1, Tmp2);
    if (cast<ConstantSDNode>(Node->getOperand(1))->getValue())
      Result = Tmp2;  // 1 -> Hi
    else
      Result = Tmp1;  // 0 -> Lo
    break;

  case ISD::CopyToReg:
    Tmp1 = LegalizeOp(Node->getOperand(0));  // Legalize the chain.
    
    switch (getTypeAction(Node->getOperand(1).getValueType())) {
    case Legal:
      // Legalize the incoming value (must be legal).
      Tmp2 = LegalizeOp(Node->getOperand(1));
      if (Tmp1 != Node->getOperand(0) || Tmp2 != Node->getOperand(1))
        Result = DAG.getCopyToReg(Tmp1, Tmp2, cast<RegSDNode>(Node)->getReg());
      break;
    case Promote:
      Tmp2 = PromoteOp(Node->getOperand(1));
      Result = DAG.getCopyToReg(Tmp1, Tmp2, cast<RegSDNode>(Node)->getReg());
      break;
    case Expand:
      SDOperand Lo, Hi;
      ExpandOp(Node->getOperand(1), Lo, Hi);      
      unsigned Reg = cast<RegSDNode>(Node)->getReg();
      Lo = DAG.getCopyToReg(Tmp1, Lo, Reg);
      Hi = DAG.getCopyToReg(Tmp1, Hi, Reg+1);
      // Note that the copytoreg nodes are independent of each other.
      Result = DAG.getNode(ISD::TokenFactor, MVT::Other, Lo, Hi);
      assert(isTypeLegal(Result.getValueType()) &&
             "Cannot expand multiple times yet (i64 -> i16)");
      break;
    }
    break;

  case ISD::RET:
    Tmp1 = LegalizeOp(Node->getOperand(0));  // Legalize the chain.
    switch (Node->getNumOperands()) {
    case 2:  // ret val
      switch (getTypeAction(Node->getOperand(1).getValueType())) {
      case Legal:
        Tmp2 = LegalizeOp(Node->getOperand(1));
        if (Tmp1 != Node->getOperand(0) || Tmp2 != Node->getOperand(1))
          Result = DAG.getNode(ISD::RET, MVT::Other, Tmp1, Tmp2);
        break;
      case Expand: {
        SDOperand Lo, Hi;
        ExpandOp(Node->getOperand(1), Lo, Hi);
        Result = DAG.getNode(ISD::RET, MVT::Other, Tmp1, Lo, Hi);
        break;                             
      }
      case Promote:
        Tmp2 = PromoteOp(Node->getOperand(1));
        Result = DAG.getNode(ISD::RET, MVT::Other, Tmp1, Tmp2);
        break;
      }
      break;
    case 1:  // ret void
      if (Tmp1 != Node->getOperand(0))
        Result = DAG.getNode(ISD::RET, MVT::Other, Tmp1);
      break;
    default: { // ret <values>
      std::vector<SDOperand> NewValues;
      NewValues.push_back(Tmp1);
      for (unsigned i = 1, e = Node->getNumOperands(); i != e; ++i)
        switch (getTypeAction(Node->getOperand(i).getValueType())) {
        case Legal:
          NewValues.push_back(LegalizeOp(Node->getOperand(i)));
          break;
        case Expand: {
          SDOperand Lo, Hi;
          ExpandOp(Node->getOperand(i), Lo, Hi);
          NewValues.push_back(Lo);
          NewValues.push_back(Hi);
          break;                             
        }
        case Promote:
          assert(0 && "Can't promote multiple return value yet!");
        }
      Result = DAG.getNode(ISD::RET, MVT::Other, NewValues);
      break;
    }
    }
    break;
  case ISD::STORE:
    Tmp1 = LegalizeOp(Node->getOperand(0));  // Legalize the chain.
    Tmp2 = LegalizeOp(Node->getOperand(2));  // Legalize the pointer.

    // Turn 'store float 1.0, Ptr' -> 'store int 0x12345678, Ptr'
    if (ConstantFPSDNode *CFP =dyn_cast<ConstantFPSDNode>(Node->getOperand(1))){
      if (CFP->getValueType(0) == MVT::f32) {
        union {
          unsigned I;
          float    F;
        } V;
        V.F = CFP->getValue();
        Result = DAG.getNode(ISD::STORE, MVT::Other, Tmp1,
                             DAG.getConstant(V.I, MVT::i32), Tmp2);
      } else {
        assert(CFP->getValueType(0) == MVT::f64 && "Unknown FP type!");
        union {
          uint64_t I;
          double   F;
        } V;
        V.F = CFP->getValue();
        Result = DAG.getNode(ISD::STORE, MVT::Other, Tmp1,
                             DAG.getConstant(V.I, MVT::i64), Tmp2);
      }
      Op = Result;
      Node = Op.Val;
    }

    switch (getTypeAction(Node->getOperand(1).getValueType())) {
    case Legal: {
      SDOperand Val = LegalizeOp(Node->getOperand(1));
      if (Val != Node->getOperand(1) || Tmp1 != Node->getOperand(0) ||
          Tmp2 != Node->getOperand(2))
        Result = DAG.getNode(ISD::STORE, MVT::Other, Tmp1, Val, Tmp2);
      break;
    }
    case Promote:
      // Truncate the value and store the result.
      Tmp3 = PromoteOp(Node->getOperand(1));
      Result = DAG.getNode(ISD::TRUNCSTORE, MVT::Other, Tmp1, Tmp3, Tmp2,
                           Node->getOperand(1).getValueType());
      break;

    case Expand:
      SDOperand Lo, Hi;
      ExpandOp(Node->getOperand(1), Lo, Hi);

      if (!TLI.isLittleEndian())
        std::swap(Lo, Hi);

      Lo = DAG.getNode(ISD::STORE, MVT::Other, Tmp1, Lo, Tmp2);

      unsigned IncrementSize = MVT::getSizeInBits(Hi.getValueType())/8;
      Tmp2 = DAG.getNode(ISD::ADD, Tmp2.getValueType(), Tmp2,
                         getIntPtrConstant(IncrementSize));
      assert(isTypeLegal(Tmp2.getValueType()) &&
             "Pointers must be legal!");
      Hi = DAG.getNode(ISD::STORE, MVT::Other, Tmp1, Hi, Tmp2);
      Result = DAG.getNode(ISD::TokenFactor, MVT::Other, Lo, Hi);
      break;
    }
    break;
  case ISD::TRUNCSTORE:
    Tmp1 = LegalizeOp(Node->getOperand(0));  // Legalize the chain.
    Tmp3 = LegalizeOp(Node->getOperand(2));  // Legalize the pointer.

    switch (getTypeAction(Node->getOperand(1).getValueType())) {
    case Legal:
      Tmp2 = LegalizeOp(Node->getOperand(1));
      if (Tmp1 != Node->getOperand(0) || Tmp2 != Node->getOperand(1) ||
          Tmp3 != Node->getOperand(2))
        Result = DAG.getNode(ISD::TRUNCSTORE, MVT::Other, Tmp1, Tmp2, Tmp3,
                             cast<MVTSDNode>(Node)->getExtraValueType());
      break;
    case Promote:
    case Expand:
      assert(0 && "Cannot handle illegal TRUNCSTORE yet!");
    }
    break;
  case ISD::SELECT:
    switch (getTypeAction(Node->getOperand(0).getValueType())) {
    case Expand: assert(0 && "It's impossible to expand bools");
    case Legal:
      Tmp1 = LegalizeOp(Node->getOperand(0)); // Legalize the condition.
      break;
    case Promote:
      Tmp1 = PromoteOp(Node->getOperand(0));  // Promote the condition.
      break;
    }
    Tmp2 = LegalizeOp(Node->getOperand(1));   // TrueVal
    Tmp3 = LegalizeOp(Node->getOperand(2));   // FalseVal

    switch (TLI.getOperationAction(Node->getOpcode(), Tmp2.getValueType())) {
    default: assert(0 && "This action is not supported yet!");
    case TargetLowering::Legal:
      if (Tmp1 != Node->getOperand(0) || Tmp2 != Node->getOperand(1) ||
          Tmp3 != Node->getOperand(2))
        Result = DAG.getNode(ISD::SELECT, Node->getValueType(0),
                             Tmp1, Tmp2, Tmp3);
      break;
    case TargetLowering::Promote: {
      MVT::ValueType NVT =
        TLI.getTypeToPromoteTo(ISD::SELECT, Tmp2.getValueType());
      unsigned ExtOp, TruncOp;
      if (MVT::isInteger(Tmp2.getValueType())) {
        ExtOp = ISD::ZERO_EXTEND;
        TruncOp  = ISD::TRUNCATE;
      } else {
        ExtOp = ISD::FP_EXTEND;
        TruncOp  = ISD::FP_ROUND;
      }
      // Promote each of the values to the new type.
      Tmp2 = DAG.getNode(ExtOp, NVT, Tmp2);
      Tmp3 = DAG.getNode(ExtOp, NVT, Tmp3);
      // Perform the larger operation, then round down.
      Result = DAG.getNode(ISD::SELECT, NVT, Tmp1, Tmp2,Tmp3);
      Result = DAG.getNode(TruncOp, Node->getValueType(0), Result);
      break;
    }
    }
    break;
  case ISD::SETCC:
    switch (getTypeAction(Node->getOperand(0).getValueType())) {
    case Legal:
      Tmp1 = LegalizeOp(Node->getOperand(0));   // LHS
      Tmp2 = LegalizeOp(Node->getOperand(1));   // RHS
      if (Tmp1 != Node->getOperand(0) || Tmp2 != Node->getOperand(1))
        Result = DAG.getSetCC(cast<SetCCSDNode>(Node)->getCondition(),
                              Node->getValueType(0), Tmp1, Tmp2);
      break;
    case Promote:
      Tmp1 = PromoteOp(Node->getOperand(0));   // LHS
      Tmp2 = PromoteOp(Node->getOperand(1));   // RHS

      // If this is an FP compare, the operands have already been extended.
      if (MVT::isInteger(Node->getOperand(0).getValueType())) {
        MVT::ValueType VT = Node->getOperand(0).getValueType();
        MVT::ValueType NVT = TLI.getTypeToTransformTo(VT);

        // Otherwise, we have to insert explicit sign or zero extends.  Note
        // that we could insert sign extends for ALL conditions, but zero extend
        // is cheaper on many machines (an AND instead of two shifts), so prefer
        // it.
        switch (cast<SetCCSDNode>(Node)->getCondition()) {
        default: assert(0 && "Unknown integer comparison!");
        case ISD::SETEQ:
        case ISD::SETNE:
        case ISD::SETUGE:
        case ISD::SETUGT:
        case ISD::SETULE:
        case ISD::SETULT:
          // ALL of these operations will work if we either sign or zero extend
          // the operands (including the unsigned comparisons!).  Zero extend is
          // usually a simpler/cheaper operation, so prefer it.
          Tmp1 = DAG.getNode(ISD::ZERO_EXTEND_INREG, NVT, Tmp1, VT);
          Tmp2 = DAG.getNode(ISD::ZERO_EXTEND_INREG, NVT, Tmp2, VT);
          break;
        case ISD::SETGE:
        case ISD::SETGT:
        case ISD::SETLT:
        case ISD::SETLE:
          Tmp1 = DAG.getNode(ISD::SIGN_EXTEND_INREG, NVT, Tmp1, VT);
          Tmp2 = DAG.getNode(ISD::SIGN_EXTEND_INREG, NVT, Tmp2, VT);
          break;
        }

      }
      Result = DAG.getSetCC(cast<SetCCSDNode>(Node)->getCondition(),
                            Node->getValueType(0), Tmp1, Tmp2);
      break;
    case Expand: 
      SDOperand LHSLo, LHSHi, RHSLo, RHSHi;
      ExpandOp(Node->getOperand(0), LHSLo, LHSHi);
      ExpandOp(Node->getOperand(1), RHSLo, RHSHi);
      switch (cast<SetCCSDNode>(Node)->getCondition()) {
      case ISD::SETEQ:
      case ISD::SETNE:
        Tmp1 = DAG.getNode(ISD::XOR, LHSLo.getValueType(), LHSLo, RHSLo);
        Tmp2 = DAG.getNode(ISD::XOR, LHSLo.getValueType(), LHSHi, RHSHi);
        Tmp1 = DAG.getNode(ISD::OR, Tmp1.getValueType(), Tmp1, Tmp2);
        Result = DAG.getSetCC(cast<SetCCSDNode>(Node)->getCondition(), 
                              Node->getValueType(0), Tmp1,
                              DAG.getConstant(0, Tmp1.getValueType()));
        break;
      default:
        // FIXME: This generated code sucks.
        ISD::CondCode LowCC;
        switch (cast<SetCCSDNode>(Node)->getCondition()) {
        default: assert(0 && "Unknown integer setcc!");
        case ISD::SETLT:
        case ISD::SETULT: LowCC = ISD::SETULT; break;
        case ISD::SETGT:
        case ISD::SETUGT: LowCC = ISD::SETUGT; break;
        case ISD::SETLE:
        case ISD::SETULE: LowCC = ISD::SETULE; break;
        case ISD::SETGE:
        case ISD::SETUGE: LowCC = ISD::SETUGE; break;
        }
        
        // Tmp1 = lo(op1) < lo(op2)   // Always unsigned comparison
        // Tmp2 = hi(op1) < hi(op2)   // Signedness depends on operands
        // dest = hi(op1) == hi(op2) ? Tmp1 : Tmp2;

        // NOTE: on targets without efficient SELECT of bools, we can always use
        // this identity: (B1 ? B2 : B3) --> (B1 & B2)|(!B1&B3)
        Tmp1 = DAG.getSetCC(LowCC, Node->getValueType(0), LHSLo, RHSLo);
        Tmp2 = DAG.getSetCC(cast<SetCCSDNode>(Node)->getCondition(),
                            Node->getValueType(0), LHSHi, RHSHi);
        Result = DAG.getSetCC(ISD::SETEQ, Node->getValueType(0), LHSHi, RHSHi);
        Result = DAG.getNode(ISD::SELECT, Tmp1.getValueType(),
                             Result, Tmp1, Tmp2);
        break;
      }
    }
    break;

  case ISD::MEMSET:
  case ISD::MEMCPY:
  case ISD::MEMMOVE: {
    Tmp1 = LegalizeOp(Node->getOperand(0));
    Tmp2 = LegalizeOp(Node->getOperand(1));
    Tmp3 = LegalizeOp(Node->getOperand(2));
    SDOperand Tmp4 = LegalizeOp(Node->getOperand(3));
    SDOperand Tmp5 = LegalizeOp(Node->getOperand(4));

    switch (TLI.getOperationAction(Node->getOpcode(), MVT::Other)) {
    default: assert(0 && "This action not implemented for this operation!");
    case TargetLowering::Legal:
      if (Tmp1 != Node->getOperand(0) || Tmp2 != Node->getOperand(1) ||
          Tmp3 != Node->getOperand(2) || Tmp4 != Node->getOperand(3) ||
          Tmp5 != Node->getOperand(4)) {
        std::vector<SDOperand> Ops;
        Ops.push_back(Tmp1); Ops.push_back(Tmp2); Ops.push_back(Tmp3);
        Ops.push_back(Tmp4); Ops.push_back(Tmp5);
        Result = DAG.getNode(Node->getOpcode(), MVT::Other, Ops);
      }
      break;
    case TargetLowering::Expand: {
      // Otherwise, the target does not support this operation.  Lower the
      // operation to an explicit libcall as appropriate.
      MVT::ValueType IntPtr = TLI.getPointerTy();
      const Type *IntPtrTy = TLI.getTargetData().getIntPtrType();
      std::vector<std::pair<SDOperand, const Type*> > Args;

      const char *FnName = 0;
      if (Node->getOpcode() == ISD::MEMSET) {
        Args.push_back(std::make_pair(Tmp2, IntPtrTy));
        // Extend the ubyte argument to be an int value for the call.
        Tmp3 = DAG.getNode(ISD::ZERO_EXTEND, MVT::i32, Tmp3);
        Args.push_back(std::make_pair(Tmp3, Type::IntTy));
        Args.push_back(std::make_pair(Tmp4, IntPtrTy));

        FnName = "memset";
      } else if (Node->getOpcode() == ISD::MEMCPY ||
                 Node->getOpcode() == ISD::MEMMOVE) {
        Args.push_back(std::make_pair(Tmp2, IntPtrTy));
        Args.push_back(std::make_pair(Tmp3, IntPtrTy));
        Args.push_back(std::make_pair(Tmp4, IntPtrTy));
        FnName = Node->getOpcode() == ISD::MEMMOVE ? "memmove" : "memcpy";
      } else {
        assert(0 && "Unknown op!");
      }
      std::pair<SDOperand,SDOperand> CallResult =
        TLI.LowerCallTo(Tmp1, Type::VoidTy,
                        DAG.getExternalSymbol(FnName, IntPtr), Args, DAG);
      Result = LegalizeOp(CallResult.second);
      break;
    }
    case TargetLowering::Custom:
      std::vector<SDOperand> Ops;
      Ops.push_back(Tmp1); Ops.push_back(Tmp2); Ops.push_back(Tmp3);
      Ops.push_back(Tmp4); Ops.push_back(Tmp5);
      Result = DAG.getNode(Node->getOpcode(), MVT::Other, Ops);
      Result = TLI.LowerOperation(Result);
      Result = LegalizeOp(Result);
      break;
    }
    break;
  }
  case ISD::ADD_PARTS:
  case ISD::SUB_PARTS: {
    std::vector<SDOperand> Ops;
    bool Changed = false;
    for (unsigned i = 0, e = Node->getNumOperands(); i != e; ++i) {
      Ops.push_back(LegalizeOp(Node->getOperand(i)));
      Changed |= Ops.back() != Node->getOperand(i);
    }
    if (Changed)
      Result = DAG.getNode(Node->getOpcode(), Node->getValueType(0), Ops);
    break;
  }
  case ISD::ADD:
  case ISD::SUB:
  case ISD::MUL:
  case ISD::UDIV:
  case ISD::SDIV:
  case ISD::UREM:
  case ISD::SREM:
  case ISD::AND:
  case ISD::OR:
  case ISD::XOR:
  case ISD::SHL:
  case ISD::SRL:
  case ISD::SRA:
    Tmp1 = LegalizeOp(Node->getOperand(0));   // LHS
    Tmp2 = LegalizeOp(Node->getOperand(1));   // RHS
    if (Tmp1 != Node->getOperand(0) ||
        Tmp2 != Node->getOperand(1))
      Result = DAG.getNode(Node->getOpcode(), Node->getValueType(0), Tmp1,Tmp2);
    break;
  case ISD::ZERO_EXTEND:
  case ISD::SIGN_EXTEND:
  case ISD::TRUNCATE:
  case ISD::FP_EXTEND:
  case ISD::FP_ROUND:
  case ISD::FP_TO_SINT:
  case ISD::FP_TO_UINT:
  case ISD::SINT_TO_FP:
  case ISD::UINT_TO_FP:
    switch (getTypeAction(Node->getOperand(0).getValueType())) {
    case Legal:
      Tmp1 = LegalizeOp(Node->getOperand(0));
      if (Tmp1 != Node->getOperand(0))
        Result = DAG.getNode(Node->getOpcode(), Node->getValueType(0), Tmp1);
      break;
    case Expand:
      if (Node->getOpcode() == ISD::SINT_TO_FP ||
          Node->getOpcode() == ISD::UINT_TO_FP) {
        Result = ExpandIntToFP(Node->getOpcode() == ISD::SINT_TO_FP,
                               Node->getValueType(0), Node->getOperand(0));
        Result = LegalizeOp(Result);
        break;
      }
      // In the expand case, we must be dealing with a truncate, because
      // otherwise the result would be larger than the source.
      assert(Node->getOpcode() == ISD::TRUNCATE &&
             "Shouldn't need to expand other operators here!");
      ExpandOp(Node->getOperand(0), Tmp1, Tmp2);

      // Since the result is legal, we should just be able to truncate the low
      // part of the source.
      Result = DAG.getNode(ISD::TRUNCATE, Node->getValueType(0), Tmp1);
      break;

    case Promote:
      switch (Node->getOpcode()) {
      case ISD::ZERO_EXTEND:
        Result = PromoteOp(Node->getOperand(0));
        // NOTE: Any extend would work here...
        Result = DAG.getNode(ISD::ZERO_EXTEND, Op.getValueType(), Result);
        Result = DAG.getNode(ISD::ZERO_EXTEND_INREG, Op.getValueType(),
                             Result, Node->getOperand(0).getValueType());
        break;
      case ISD::SIGN_EXTEND:
        Result = PromoteOp(Node->getOperand(0));
        // NOTE: Any extend would work here...
        Result = DAG.getNode(ISD::ZERO_EXTEND, Op.getValueType(), Result);
        Result = DAG.getNode(ISD::SIGN_EXTEND_INREG, Result.getValueType(),
                             Result, Node->getOperand(0).getValueType());
        break;
      case ISD::TRUNCATE:
        Result = PromoteOp(Node->getOperand(0));
        Result = DAG.getNode(ISD::TRUNCATE, Op.getValueType(), Result);
        break;
      case ISD::FP_EXTEND:
        Result = PromoteOp(Node->getOperand(0));
        if (Result.getValueType() != Op.getValueType())
          // Dynamically dead while we have only 2 FP types.
          Result = DAG.getNode(ISD::FP_EXTEND, Op.getValueType(), Result);
        break;
      case ISD::FP_ROUND:
      case ISD::FP_TO_SINT:
      case ISD::FP_TO_UINT:
        Result = PromoteOp(Node->getOperand(0));
        Result = DAG.getNode(Node->getOpcode(), Op.getValueType(), Result);
        break;
      case ISD::SINT_TO_FP:
        Result = PromoteOp(Node->getOperand(0));
        Result = DAG.getNode(ISD::SIGN_EXTEND_INREG, Result.getValueType(),
                             Result, Node->getOperand(0).getValueType());
        Result = DAG.getNode(ISD::SINT_TO_FP, Op.getValueType(), Result);
        break;
      case ISD::UINT_TO_FP:
        Result = PromoteOp(Node->getOperand(0));
        Result = DAG.getNode(ISD::ZERO_EXTEND_INREG, Result.getValueType(),
                             Result, Node->getOperand(0).getValueType());
        Result = DAG.getNode(ISD::UINT_TO_FP, Op.getValueType(), Result);
        break;
      }
    }
    break;
  case ISD::FP_ROUND_INREG:
  case ISD::SIGN_EXTEND_INREG:
  case ISD::ZERO_EXTEND_INREG: {
    Tmp1 = LegalizeOp(Node->getOperand(0));
    MVT::ValueType ExtraVT = cast<MVTSDNode>(Node)->getExtraValueType();

    // If this operation is not supported, convert it to a shl/shr or load/store
    // pair.
    switch (TLI.getOperationAction(Node->getOpcode(), ExtraVT)) {
    default: assert(0 && "This action not supported for this op yet!");
    case TargetLowering::Legal:
      if (Tmp1 != Node->getOperand(0))
        Result = DAG.getNode(Node->getOpcode(), Node->getValueType(0), Tmp1,
                             ExtraVT);
      break;
    case TargetLowering::Expand:
      // If this is an integer extend and shifts are supported, do that.
      if (Node->getOpcode() == ISD::ZERO_EXTEND_INREG) {
        // NOTE: we could fall back on load/store here too for targets without
        // AND.  However, it is doubtful that any exist.
        // AND out the appropriate bits.
        SDOperand Mask =
          DAG.getConstant((1ULL << MVT::getSizeInBits(ExtraVT))-1,
                          Node->getValueType(0));
        Result = DAG.getNode(ISD::AND, Node->getValueType(0),
                             Node->getOperand(0), Mask);
      } else if (Node->getOpcode() == ISD::SIGN_EXTEND_INREG) {
        // NOTE: we could fall back on load/store here too for targets without
        // SAR.  However, it is doubtful that any exist.
        unsigned BitsDiff = MVT::getSizeInBits(Node->getValueType(0)) -
                            MVT::getSizeInBits(ExtraVT);
        SDOperand ShiftCst = DAG.getConstant(BitsDiff, TLI.getShiftAmountTy());
        Result = DAG.getNode(ISD::SHL, Node->getValueType(0),
                             Node->getOperand(0), ShiftCst);
        Result = DAG.getNode(ISD::SRA, Node->getValueType(0),
                             Result, ShiftCst);
      } else if (Node->getOpcode() == ISD::FP_ROUND_INREG) {
        // The only way we can lower this is to turn it into a STORETRUNC,
        // EXTLOAD pair, targetting a temporary location (a stack slot).

        // NOTE: there is a choice here between constantly creating new stack
        // slots and always reusing the same one.  We currently always create
        // new ones, as reuse may inhibit scheduling.
        const Type *Ty = MVT::getTypeForValueType(ExtraVT);
        unsigned TySize = (unsigned)TLI.getTargetData().getTypeSize(Ty);
        unsigned Align  = TLI.getTargetData().getTypeAlignment(Ty);
        MachineFunction &MF = DAG.getMachineFunction();
        int SSFI = 
          MF.getFrameInfo()->CreateStackObject((unsigned)TySize, Align);
        SDOperand StackSlot = DAG.getFrameIndex(SSFI, TLI.getPointerTy());
        Result = DAG.getNode(ISD::TRUNCSTORE, MVT::Other, DAG.getEntryNode(),
                             Node->getOperand(0), StackSlot, ExtraVT);
        Result = DAG.getNode(ISD::EXTLOAD, Node->getValueType(0),
                             Result, StackSlot, ExtraVT);
      } else {
        assert(0 && "Unknown op");
      }
      Result = LegalizeOp(Result);
      break;
    }
    break;
  }
  }

  if (!Op.Val->hasOneUse())
    AddLegalizedOperand(Op, Result);

  return Result;
}

/// PromoteOp - Given an operation that produces a value in an invalid type,
/// promote it to compute the value into a larger type.  The produced value will
/// have the correct bits for the low portion of the register, but no guarantee
/// is made about the top bits: it may be zero, sign-extended, or garbage.
SDOperand SelectionDAGLegalize::PromoteOp(SDOperand Op) {
  MVT::ValueType VT = Op.getValueType();
  MVT::ValueType NVT = TLI.getTypeToTransformTo(VT);
  assert(getTypeAction(VT) == Promote &&
         "Caller should expand or legalize operands that are not promotable!");
  assert(NVT > VT && MVT::isInteger(NVT) == MVT::isInteger(VT) &&
         "Cannot promote to smaller type!");

  std::map<SDOperand, SDOperand>::iterator I = PromotedNodes.find(Op);
  if (I != PromotedNodes.end()) return I->second;

  SDOperand Tmp1, Tmp2, Tmp3;

  SDOperand Result;
  SDNode *Node = Op.Val;

  // Promotion needs an optimization step to clean up after it, and is not
  // careful to avoid operations the target does not support.  Make sure that
  // all generated operations are legalized in the next iteration.
  NeedsAnotherIteration = true;

  switch (Node->getOpcode()) {
  default:
    std::cerr << "NODE: "; Node->dump(); std::cerr << "\n";
    assert(0 && "Do not know how to promote this operator!");
    abort();
  case ISD::Constant:
    Result = DAG.getNode(ISD::ZERO_EXTEND, NVT, Op);
    assert(isa<ConstantSDNode>(Result) && "Didn't constant fold zext?");
    break;
  case ISD::ConstantFP:
    Result = DAG.getNode(ISD::FP_EXTEND, NVT, Op);
    assert(isa<ConstantFPSDNode>(Result) && "Didn't constant fold fp_extend?");
    break;
  case ISD::CopyFromReg:
    Result = DAG.getCopyFromReg(cast<RegSDNode>(Node)->getReg(), NVT,
                                Node->getOperand(0));
    // Remember that we legalized the chain.
    AddLegalizedOperand(Op.getValue(1), Result.getValue(1));
    break;

  case ISD::SETCC:
    assert(getTypeAction(TLI.getSetCCResultTy()) == Legal &&
           "SetCC type is not legal??");
    Result = DAG.getSetCC(cast<SetCCSDNode>(Node)->getCondition(),
                          TLI.getSetCCResultTy(), Node->getOperand(0),
                          Node->getOperand(1));
    Result = LegalizeOp(Result);
    break;

  case ISD::TRUNCATE:
    switch (getTypeAction(Node->getOperand(0).getValueType())) {
    case Legal:
      Result = LegalizeOp(Node->getOperand(0));
      assert(Result.getValueType() >= NVT &&
             "This truncation doesn't make sense!");
      if (Result.getValueType() > NVT)    // Truncate to NVT instead of VT
        Result = DAG.getNode(ISD::TRUNCATE, NVT, Result);
      break;
    case Expand:
      assert(0 && "Cannot handle expand yet");
    case Promote:
      assert(0 && "Cannot handle promote-promote yet");
    }
    break;
  case ISD::SIGN_EXTEND:
  case ISD::ZERO_EXTEND:
    switch (getTypeAction(Node->getOperand(0).getValueType())) {
    case Expand: assert(0 && "BUG: Smaller reg should have been promoted!");
    case Legal:
      // Input is legal?  Just do extend all the way to the larger type.
      Result = LegalizeOp(Node->getOperand(0));
      Result = DAG.getNode(Node->getOpcode(), NVT, Result);
      break;
    case Promote:
      // Promote the reg if it's smaller.
      Result = PromoteOp(Node->getOperand(0));
      // The high bits are not guaranteed to be anything.  Insert an extend.
      if (Node->getOpcode() == ISD::SIGN_EXTEND)
        Result = DAG.getNode(ISD::SIGN_EXTEND_INREG, NVT, Result, VT);
      else
        Result = DAG.getNode(ISD::ZERO_EXTEND_INREG, NVT, Result, VT);
      break;
    }
    break;

  case ISD::FP_EXTEND:
    assert(0 && "Case not implemented.  Dynamically dead with 2 FP types!");
  case ISD::FP_ROUND:
    switch (getTypeAction(Node->getOperand(0).getValueType())) {
    case Expand: assert(0 && "BUG: Cannot expand FP regs!");
    case Promote:  assert(0 && "Unreachable with 2 FP types!");
    case Legal:
      // Input is legal?  Do an FP_ROUND_INREG.
      Result = LegalizeOp(Node->getOperand(0));
      Result = DAG.getNode(ISD::FP_ROUND_INREG, NVT, Result, VT);
      break;
    }
    break;

  case ISD::SINT_TO_FP:
  case ISD::UINT_TO_FP:
    switch (getTypeAction(Node->getOperand(0).getValueType())) {
    case Legal:
      Result = LegalizeOp(Node->getOperand(0));
      // No extra round required here.
      Result = DAG.getNode(Node->getOpcode(), NVT, Result);
      break;

    case Promote:
      Result = PromoteOp(Node->getOperand(0));
      if (Node->getOpcode() == ISD::SINT_TO_FP)
        Result = DAG.getNode(ISD::SIGN_EXTEND_INREG, Result.getValueType(),
                             Result, Node->getOperand(0).getValueType());
      else
        Result = DAG.getNode(ISD::ZERO_EXTEND_INREG, Result.getValueType(),
                             Result, Node->getOperand(0).getValueType());
      // No extra round required here.
      Result = DAG.getNode(Node->getOpcode(), NVT, Result);
      break;
    case Expand:
      Result = ExpandIntToFP(Node->getOpcode() == ISD::SINT_TO_FP, NVT,
                             Node->getOperand(0));
      Result = LegalizeOp(Result);

      // Round if we cannot tolerate excess precision.
      if (NoExcessFPPrecision)
        Result = DAG.getNode(ISD::FP_ROUND_INREG, NVT, Result, VT);
      break;
    }
    break;

  case ISD::FP_TO_SINT:
  case ISD::FP_TO_UINT:
    switch (getTypeAction(Node->getOperand(0).getValueType())) {
    case Legal:
      Tmp1 = LegalizeOp(Node->getOperand(0));
      break;
    case Promote:
      // The input result is prerounded, so we don't have to do anything
      // special.
      Tmp1 = PromoteOp(Node->getOperand(0));
      break;
    case Expand:
      assert(0 && "not implemented");
    }
    Result = DAG.getNode(Node->getOpcode(), NVT, Tmp1);
    break;

  case ISD::AND:
  case ISD::OR:
  case ISD::XOR:
  case ISD::ADD:
  case ISD::SUB:
  case ISD::MUL:
    // The input may have strange things in the top bits of the registers, but
    // these operations don't care.  They may have wierd bits going out, but
    // that too is okay if they are integer operations.
    Tmp1 = PromoteOp(Node->getOperand(0));
    Tmp2 = PromoteOp(Node->getOperand(1));
    assert(Tmp1.getValueType() == NVT && Tmp2.getValueType() == NVT);
    Result = DAG.getNode(Node->getOpcode(), NVT, Tmp1, Tmp2);

    // However, if this is a floating point operation, they will give excess
    // precision that we may not be able to tolerate.  If we DO allow excess
    // precision, just leave it, otherwise excise it.
    // FIXME: Why would we need to round FP ops more than integer ones?
    //     Is Round(Add(Add(A,B),C)) != Round(Add(Round(Add(A,B)), C))
    if (MVT::isFloatingPoint(NVT) && NoExcessFPPrecision)
      Result = DAG.getNode(ISD::FP_ROUND_INREG, NVT, Result, VT);
    break;

  case ISD::SDIV:
  case ISD::SREM:
    // These operators require that their input be sign extended.
    Tmp1 = PromoteOp(Node->getOperand(0));
    Tmp2 = PromoteOp(Node->getOperand(1));
    if (MVT::isInteger(NVT)) {
      Tmp1 = DAG.getNode(ISD::SIGN_EXTEND_INREG, NVT, Tmp1, VT);
      Tmp2 = DAG.getNode(ISD::SIGN_EXTEND_INREG, NVT, Tmp2, VT);
    }
    Result = DAG.getNode(Node->getOpcode(), NVT, Tmp1, Tmp2);

    // Perform FP_ROUND: this is probably overly pessimistic.
    if (MVT::isFloatingPoint(NVT) && NoExcessFPPrecision)
      Result = DAG.getNode(ISD::FP_ROUND_INREG, NVT, Result, VT);
    break;

  case ISD::UDIV:
  case ISD::UREM:
    // These operators require that their input be zero extended.
    Tmp1 = PromoteOp(Node->getOperand(0));
    Tmp2 = PromoteOp(Node->getOperand(1));
    assert(MVT::isInteger(NVT) && "Operators don't apply to FP!");
    Tmp1 = DAG.getNode(ISD::ZERO_EXTEND_INREG, NVT, Tmp1, VT);
    Tmp2 = DAG.getNode(ISD::ZERO_EXTEND_INREG, NVT, Tmp2, VT);
    Result = DAG.getNode(Node->getOpcode(), NVT, Tmp1, Tmp2);
    break;

  case ISD::SHL:
    Tmp1 = PromoteOp(Node->getOperand(0));
    Tmp2 = LegalizeOp(Node->getOperand(1));
    Result = DAG.getNode(ISD::SHL, NVT, Tmp1, Tmp2);
    break;
  case ISD::SRA:
    // The input value must be properly sign extended.
    Tmp1 = PromoteOp(Node->getOperand(0));
    Tmp1 = DAG.getNode(ISD::SIGN_EXTEND_INREG, NVT, Tmp1, VT);
    Tmp2 = LegalizeOp(Node->getOperand(1));
    Result = DAG.getNode(ISD::SRA, NVT, Tmp1, Tmp2);
    break;
  case ISD::SRL:
    // The input value must be properly zero extended.
    Tmp1 = PromoteOp(Node->getOperand(0));
    Tmp1 = DAG.getNode(ISD::ZERO_EXTEND_INREG, NVT, Tmp1, VT);
    Tmp2 = LegalizeOp(Node->getOperand(1));
    Result = DAG.getNode(ISD::SRL, NVT, Tmp1, Tmp2);
    break;
  case ISD::LOAD:
    Tmp1 = LegalizeOp(Node->getOperand(0));   // Legalize the chain.
    Tmp2 = LegalizeOp(Node->getOperand(1));   // Legalize the pointer.
    Result = DAG.getNode(ISD::EXTLOAD, NVT, Tmp1, Tmp2, VT);

    // Remember that we legalized the chain.
    AddLegalizedOperand(Op.getValue(1), Result.getValue(1));
    break;
  case ISD::SELECT:
    switch (getTypeAction(Node->getOperand(0).getValueType())) {
    case Expand: assert(0 && "It's impossible to expand bools");
    case Legal:
      Tmp1 = LegalizeOp(Node->getOperand(0));// Legalize the condition.
      break;
    case Promote:
      Tmp1 = PromoteOp(Node->getOperand(0)); // Promote the condition.
      break;
    }
    Tmp2 = PromoteOp(Node->getOperand(1));   // Legalize the op0
    Tmp3 = PromoteOp(Node->getOperand(2));   // Legalize the op1
    Result = DAG.getNode(ISD::SELECT, NVT, Tmp1, Tmp2, Tmp3);
    break;
  case ISD::CALL: {
    Tmp1 = LegalizeOp(Node->getOperand(0));  // Legalize the chain.
    Tmp2 = LegalizeOp(Node->getOperand(1));  // Legalize the callee.

    std::vector<SDOperand> Ops;
    for (unsigned i = 2, e = Node->getNumOperands(); i != e; ++i)
      Ops.push_back(LegalizeOp(Node->getOperand(i)));

    assert(Node->getNumValues() == 2 && Op.ResNo == 0 &&
           "Can only promote single result calls");
    std::vector<MVT::ValueType> RetTyVTs;
    RetTyVTs.reserve(2);
    RetTyVTs.push_back(NVT);
    RetTyVTs.push_back(MVT::Other);
    SDNode *NC = DAG.getCall(RetTyVTs, Tmp1, Tmp2, Ops);
    Result = SDOperand(NC, 0);

    // Insert the new chain mapping.
    AddLegalizedOperand(Op.getValue(1), Result.getValue(1));
    break;
  } 
  }

  assert(Result.Val && "Didn't set a result!");
  AddPromotedOperand(Op, Result);
  return Result;
}

/// ExpandAddSub - Find a clever way to expand this add operation into
/// subcomponents.
void SelectionDAGLegalize::ExpandAddSub(bool isAdd, SDOperand LHS,SDOperand RHS,
                                        SDOperand &Lo, SDOperand &Hi) {
  // Expand the subcomponents.
  SDOperand LHSL, LHSH, RHSL, RHSH;
  ExpandOp(LHS, LHSL, LHSH);
  ExpandOp(RHS, RHSL, RHSH);

  // Convert this add to the appropriate ADDC pair.  The low part has no carry
  // in.
  unsigned Opc = isAdd ? ISD::ADD_PARTS : ISD::SUB_PARTS;
  std::vector<SDOperand> Ops;
  Ops.push_back(LHSL);
  Ops.push_back(LHSH);
  Ops.push_back(RHSL);
  Ops.push_back(RHSH);
  Lo = DAG.getNode(Opc, LHSL.getValueType(), Ops);
  Hi = Lo.getValue(1);
}

/// ExpandShift - Try to find a clever way to expand this shift operation out to
/// smaller elements.  If we can't find a way that is more efficient than a
/// libcall on this target, return false.  Otherwise, return true with the
/// low-parts expanded into Lo and Hi.
bool SelectionDAGLegalize::ExpandShift(unsigned Opc, SDOperand Op,SDOperand Amt,
                                       SDOperand &Lo, SDOperand &Hi) {
  assert((Opc == ISD::SHL || Opc == ISD::SRA || Opc == ISD::SRL) &&
         "This is not a shift!");
  MVT::ValueType NVT = TLI.getTypeToTransformTo(Op.getValueType());

  // If we have an efficient select operation (or if the selects will all fold
  // away), lower to some complex code, otherwise just emit the libcall.
  if (TLI.getOperationAction(ISD::SELECT, NVT) != TargetLowering::Legal &&
      !isa<ConstantSDNode>(Amt))
    return false;

  SDOperand InL, InH;
  ExpandOp(Op, InL, InH);
  SDOperand ShAmt = LegalizeOp(Amt);
  MVT::ValueType ShTy = ShAmt.getValueType();
  
  unsigned NVTBits = MVT::getSizeInBits(NVT);
  SDOperand NAmt = DAG.getNode(ISD::SUB, ShTy,           // NAmt = 32-ShAmt
                               DAG.getConstant(NVTBits, ShTy), ShAmt);

  // Compare the unmasked shift amount against 32.
  SDOperand Cond = DAG.getSetCC(ISD::SETGE, TLI.getSetCCResultTy(), ShAmt,
                                DAG.getConstant(NVTBits, ShTy));

  if (TLI.getShiftAmountFlavor() != TargetLowering::Mask) {
    ShAmt = DAG.getNode(ISD::AND, ShTy, ShAmt,             // ShAmt &= 31
                        DAG.getConstant(NVTBits-1, ShTy));
    NAmt  = DAG.getNode(ISD::AND, ShTy, NAmt,              // NAmt &= 31
                        DAG.getConstant(NVTBits-1, ShTy));
  }

  if (Opc == ISD::SHL) {
    SDOperand T1 = DAG.getNode(ISD::OR, NVT,// T1 = (Hi << Amt) | (Lo >> NAmt)
                               DAG.getNode(ISD::SHL, NVT, InH, ShAmt),
                               DAG.getNode(ISD::SRL, NVT, InL, NAmt));
    SDOperand T2 = DAG.getNode(ISD::SHL, NVT, InL, ShAmt); // T2 = Lo << Amt&31
    
    Hi = DAG.getNode(ISD::SELECT, NVT, Cond, T2, T1);
    Lo = DAG.getNode(ISD::SELECT, NVT, Cond, DAG.getConstant(0, NVT), T2);
  } else {
    SDOperand HiLoPart = DAG.getNode(ISD::SELECT, NVT,
                                     DAG.getSetCC(ISD::SETEQ,
                                                  TLI.getSetCCResultTy(), NAmt,
                                                  DAG.getConstant(32, ShTy)),
                                     DAG.getConstant(0, NVT),
                                     DAG.getNode(ISD::SHL, NVT, InH, NAmt));
    SDOperand T1 = DAG.getNode(ISD::OR, NVT,// T1 = (Hi << NAmt) | (Lo >> Amt)
                               HiLoPart,
                               DAG.getNode(ISD::SRL, NVT, InL, ShAmt));
    SDOperand T2 = DAG.getNode(Opc, NVT, InH, ShAmt);  // T2 = InH >> ShAmt&31

    SDOperand HiPart;
    if (Opc == ISD::SRA)
      HiPart = DAG.getNode(ISD::SRA, NVT, InH,
                           DAG.getConstant(NVTBits-1, ShTy));
    else
      HiPart = DAG.getConstant(0, NVT);
    Lo = DAG.getNode(ISD::SELECT, NVT, Cond, T2, T1);
    Hi = DAG.getNode(ISD::SELECT, NVT, Cond, HiPart, T2);
  }
  return true;
}

/// FindLatestAdjCallStackDown - Scan up the dag to find the latest (highest
/// NodeDepth) node that is an AdjCallStackDown operation and occurs later than
/// Found.
static void FindLatestAdjCallStackDown(SDNode *Node, SDNode *&Found) {
  if (Node->getNodeDepth() <= Found->getNodeDepth()) return;

  // If we found an ADJCALLSTACKDOWN, we already know this node occurs later
  // than the Found node. Just remember this node and return.
  if (Node->getOpcode() == ISD::ADJCALLSTACKDOWN) {
    Found = Node;
    return;
  }

  // Otherwise, scan the operands of Node to see if any of them is a call.
  assert(Node->getNumOperands() != 0 &&
         "All leaves should have depth equal to the entry node!");
  for (unsigned i = 0, e = Node->getNumOperands()-1; i != e; ++i)
    FindLatestAdjCallStackDown(Node->getOperand(i).Val, Found);

  // Tail recurse for the last iteration.
  FindLatestAdjCallStackDown(Node->getOperand(Node->getNumOperands()-1).Val,
                             Found);
}


/// FindEarliestAdjCallStackUp - Scan down the dag to find the earliest (lowest
/// NodeDepth) node that is an AdjCallStackUp operation and occurs more recent
/// than Found.
static void FindEarliestAdjCallStackUp(SDNode *Node, SDNode *&Found) {
  if (Found && Node->getNodeDepth() >= Found->getNodeDepth()) return;

  // If we found an ADJCALLSTACKUP, we already know this node occurs earlier
  // than the Found node. Just remember this node and return.
  if (Node->getOpcode() == ISD::ADJCALLSTACKUP) {
    Found = Node;
    return;
  }

  // Otherwise, scan the operands of Node to see if any of them is a call.
  SDNode::use_iterator UI = Node->use_begin(), E = Node->use_end();
  if (UI == E) return;
  for (--E; UI != E; ++UI)
    FindEarliestAdjCallStackUp(*UI, Found);

  // Tail recurse for the last iteration.
  FindEarliestAdjCallStackUp(*UI, Found);
}

/// FindAdjCallStackUp - Given a chained node that is part of a call sequence,
/// find the ADJCALLSTACKUP node that terminates the call sequence.
static SDNode *FindAdjCallStackUp(SDNode *Node) {
  if (Node->getOpcode() == ISD::ADJCALLSTACKUP)
    return Node;
  assert(!Node->use_empty() && "Could not find ADJCALLSTACKUP!");

  if (Node->hasOneUse())  // Simple case, only has one user to check.
    return FindAdjCallStackUp(*Node->use_begin());
  
  SDOperand TheChain(Node, Node->getNumValues()-1);
  assert(TheChain.getValueType() == MVT::Other && "Is not a token chain!");
  
  for (SDNode::use_iterator UI = Node->use_begin(), 
         E = Node->use_end(); ; ++UI) {
    assert(UI != E && "Didn't find a user of the tokchain, no ADJCALLSTACKUP!");
    
    // Make sure to only follow users of our token chain.
    SDNode *User = *UI;
    for (unsigned i = 0, e = User->getNumOperands(); i != e; ++i)
      if (User->getOperand(i) == TheChain)
        return FindAdjCallStackUp(User);
  }
  assert(0 && "Unreachable");
  abort();
}

/// FindInputOutputChains - If we are replacing an operation with a call we need
/// to find the call that occurs before and the call that occurs after it to
/// properly serialize the calls in the block.
static SDOperand FindInputOutputChains(SDNode *OpNode, SDNode *&OutChain,
                                       SDOperand Entry) {
  SDNode *LatestAdjCallStackDown = Entry.Val;
  FindLatestAdjCallStackDown(OpNode, LatestAdjCallStackDown);
  //std::cerr << "Found node: "; LatestAdjCallStackDown->dump(); std::cerr <<"\n";

  SDNode *LatestAdjCallStackUp = FindAdjCallStackUp(LatestAdjCallStackDown);


  SDNode *EarliestAdjCallStackUp = 0;
  FindEarliestAdjCallStackUp(OpNode, EarliestAdjCallStackUp);

  if (EarliestAdjCallStackUp) {
    //std::cerr << "Found node: "; 
    //EarliestAdjCallStackUp->dump(); std::cerr <<"\n";
  }

  return SDOperand(LatestAdjCallStackUp, 0);
}



// ExpandLibCall - Expand a node into a call to a libcall.  If the result value
// does not fit into a register, return the lo part and set the hi part to the
// by-reg argument.  If it does fit into a single register, return the result
// and leave the Hi part unset.
SDOperand SelectionDAGLegalize::ExpandLibCall(const char *Name, SDNode *Node,
                                              SDOperand &Hi) {
  SDNode *OutChain;
  SDOperand InChain = FindInputOutputChains(Node, OutChain,
                                            DAG.getEntryNode());
  // TODO.  Link in chains.

  TargetLowering::ArgListTy Args;
  for (unsigned i = 0, e = Node->getNumOperands(); i != e; ++i) {
    MVT::ValueType ArgVT = Node->getOperand(i).getValueType();
    const Type *ArgTy = MVT::getTypeForValueType(ArgVT);
    Args.push_back(std::make_pair(Node->getOperand(i), ArgTy));
  }
  SDOperand Callee = DAG.getExternalSymbol(Name, TLI.getPointerTy());
  
  // We don't care about token chains for libcalls.  We just use the entry
  // node as our input and ignore the output chain.  This allows us to place
  // calls wherever we need them to satisfy data dependences.
  const Type *RetTy = MVT::getTypeForValueType(Node->getValueType(0));
  SDOperand Result = TLI.LowerCallTo(InChain, RetTy, Callee,
                                     Args, DAG).first;
  switch (getTypeAction(Result.getValueType())) {
  default: assert(0 && "Unknown thing");
  case Legal:
    return Result;
  case Promote:
    assert(0 && "Cannot promote this yet!");
  case Expand:
    SDOperand Lo;
    ExpandOp(Result, Lo, Hi);
    return Lo;
  }
}


/// ExpandIntToFP - Expand a [US]INT_TO_FP operation, assuming that the
/// destination type is legal.
SDOperand SelectionDAGLegalize::
ExpandIntToFP(bool isSigned, MVT::ValueType DestTy, SDOperand Source) {
  assert(getTypeAction(DestTy) == Legal && "Destination type is not legal!");
  assert(getTypeAction(Source.getValueType()) == Expand &&
         "This is not an expansion!");
  assert(Source.getValueType() == MVT::i64 && "Only handle expand from i64!");

  SDNode *OutChain;
  SDOperand InChain = FindInputOutputChains(Source.Val, OutChain,
                                            DAG.getEntryNode());

  const char *FnName = 0;
  if (isSigned) {
    if (DestTy == MVT::f32)
      FnName = "__floatdisf";
    else {
      assert(DestTy == MVT::f64 && "Unknown fp value type!");
      FnName = "__floatdidf";
    }
  } else {
    // If this is unsigned, and not supported, first perform the conversion to
    // signed, then adjust the result if the sign bit is set.
    SDOperand SignedConv = ExpandIntToFP(false, DestTy, Source);

    assert(0 && "Unsigned casts not supported yet!");
  }
  SDOperand Callee = DAG.getExternalSymbol(FnName, TLI.getPointerTy());

  TargetLowering::ArgListTy Args;
  const Type *ArgTy = MVT::getTypeForValueType(Source.getValueType());
  Args.push_back(std::make_pair(Source, ArgTy));

  // We don't care about token chains for libcalls.  We just use the entry
  // node as our input and ignore the output chain.  This allows us to place
  // calls wherever we need them to satisfy data dependences.
  const Type *RetTy = MVT::getTypeForValueType(DestTy);
  return TLI.LowerCallTo(InChain, RetTy, Callee, Args, DAG).first;
                         
}
                   


/// ExpandOp - Expand the specified SDOperand into its two component pieces
/// Lo&Hi.  Note that the Op MUST be an expanded type.  As a result of this, the
/// LegalizeNodes map is filled in for any results that are not expanded, the
/// ExpandedNodes map is filled in for any results that are expanded, and the
/// Lo/Hi values are returned.
void SelectionDAGLegalize::ExpandOp(SDOperand Op, SDOperand &Lo, SDOperand &Hi){
  MVT::ValueType VT = Op.getValueType();
  MVT::ValueType NVT = TLI.getTypeToTransformTo(VT);
  SDNode *Node = Op.Val;
  assert(getTypeAction(VT) == Expand && "Not an expanded type!");
  assert(MVT::isInteger(VT) && "Cannot expand FP values!");
  assert(MVT::isInteger(NVT) && NVT < VT &&
         "Cannot expand to FP value or to larger int value!");

  // If there is more than one use of this, see if we already expanded it.
  // There is no use remembering values that only have a single use, as the map
  // entries will never be reused.
  if (!Node->hasOneUse()) {
    std::map<SDOperand, std::pair<SDOperand, SDOperand> >::iterator I
      = ExpandedNodes.find(Op);
    if (I != ExpandedNodes.end()) {
      Lo = I->second.first;
      Hi = I->second.second;
      return;
    }
  }

  // Expanding to multiple registers needs to perform an optimization step, and
  // is not careful to avoid operations the target does not support.  Make sure
  // that all generated operations are legalized in the next iteration.
  NeedsAnotherIteration = true;

  switch (Node->getOpcode()) {
  default:
    std::cerr << "NODE: "; Node->dump(); std::cerr << "\n";
    assert(0 && "Do not know how to expand this operator!");
    abort();
  case ISD::Constant: {
    uint64_t Cst = cast<ConstantSDNode>(Node)->getValue();
    Lo = DAG.getConstant(Cst, NVT);
    Hi = DAG.getConstant(Cst >> MVT::getSizeInBits(NVT), NVT);
    break;
  }

  case ISD::CopyFromReg: {
    unsigned Reg = cast<RegSDNode>(Node)->getReg();
    // Aggregate register values are always in consequtive pairs.
    Lo = DAG.getCopyFromReg(Reg, NVT, Node->getOperand(0));
    Hi = DAG.getCopyFromReg(Reg+1, NVT, Lo.getValue(1));
    
    // Remember that we legalized the chain.
    AddLegalizedOperand(Op.getValue(1), Hi.getValue(1));

    assert(isTypeLegal(NVT) && "Cannot expand this multiple times yet!");
    break;
  }

  case ISD::LOAD: {
    SDOperand Ch = LegalizeOp(Node->getOperand(0));   // Legalize the chain.
    SDOperand Ptr = LegalizeOp(Node->getOperand(1));  // Legalize the pointer.
    Lo = DAG.getLoad(NVT, Ch, Ptr);

    // Increment the pointer to the other half.
    unsigned IncrementSize = MVT::getSizeInBits(Lo.getValueType())/8;
    Ptr = DAG.getNode(ISD::ADD, Ptr.getValueType(), Ptr,
                      getIntPtrConstant(IncrementSize));
    Hi = DAG.getLoad(NVT, Ch, Ptr);

    // Build a factor node to remember that this load is independent of the
    // other one.
    SDOperand TF = DAG.getNode(ISD::TokenFactor, MVT::Other, Lo.getValue(1),
                               Hi.getValue(1));
    
    // Remember that we legalized the chain.
    AddLegalizedOperand(Op.getValue(1), TF);
    if (!TLI.isLittleEndian())
      std::swap(Lo, Hi);
    break;
  }
  case ISD::CALL: {
    SDOperand Chain  = LegalizeOp(Node->getOperand(0));  // Legalize the chain.
    SDOperand Callee = LegalizeOp(Node->getOperand(1));  // Legalize the callee.

    bool Changed = false;
    std::vector<SDOperand> Ops;
    for (unsigned i = 2, e = Node->getNumOperands(); i != e; ++i) {
      Ops.push_back(LegalizeOp(Node->getOperand(i)));
      Changed |= Ops.back() != Node->getOperand(i);
    }

    assert(Node->getNumValues() == 2 && Op.ResNo == 0 &&
           "Can only expand a call once so far, not i64 -> i16!");

    std::vector<MVT::ValueType> RetTyVTs;
    RetTyVTs.reserve(3);
    RetTyVTs.push_back(NVT);
    RetTyVTs.push_back(NVT);
    RetTyVTs.push_back(MVT::Other);
    SDNode *NC = DAG.getCall(RetTyVTs, Chain, Callee, Ops);
    Lo = SDOperand(NC, 0);
    Hi = SDOperand(NC, 1);

    // Insert the new chain mapping.
    AddLegalizedOperand(Op.getValue(1), Hi.getValue(2));
    break;
  }
  case ISD::AND:
  case ISD::OR:
  case ISD::XOR: {   // Simple logical operators -> two trivial pieces.
    SDOperand LL, LH, RL, RH;
    ExpandOp(Node->getOperand(0), LL, LH);
    ExpandOp(Node->getOperand(1), RL, RH);
    Lo = DAG.getNode(Node->getOpcode(), NVT, LL, RL);
    Hi = DAG.getNode(Node->getOpcode(), NVT, LH, RH);
    break;
  }
  case ISD::SELECT: {
    SDOperand C, LL, LH, RL, RH;

    switch (getTypeAction(Node->getOperand(0).getValueType())) {
    case Expand: assert(0 && "It's impossible to expand bools");
    case Legal:
      C = LegalizeOp(Node->getOperand(0)); // Legalize the condition.
      break;
    case Promote:
      C = PromoteOp(Node->getOperand(0));  // Promote the condition.
      break;
    }
    ExpandOp(Node->getOperand(1), LL, LH);
    ExpandOp(Node->getOperand(2), RL, RH);
    Lo = DAG.getNode(ISD::SELECT, NVT, C, LL, RL);
    Hi = DAG.getNode(ISD::SELECT, NVT, C, LH, RH);
    break;
  }
  case ISD::SIGN_EXTEND: {
    // The low part is just a sign extension of the input (which degenerates to
    // a copy).
    Lo = DAG.getNode(ISD::SIGN_EXTEND, NVT, LegalizeOp(Node->getOperand(0)));
    
    // The high part is obtained by SRA'ing all but one of the bits of the lo
    // part.
    unsigned LoSize = MVT::getSizeInBits(Lo.getValueType());
    Hi = DAG.getNode(ISD::SRA, NVT, Lo, DAG.getConstant(LoSize-1,
                                                       TLI.getShiftAmountTy()));
    break;
  }
  case ISD::ZERO_EXTEND:
    // The low part is just a zero extension of the input (which degenerates to
    // a copy).
    Lo = DAG.getNode(ISD::ZERO_EXTEND, NVT, LegalizeOp(Node->getOperand(0)));
    
    // The high part is just a zero.
    Hi = DAG.getConstant(0, NVT);
    break;

    // These operators cannot be expanded directly, emit them as calls to
    // library functions.
  case ISD::FP_TO_SINT:
    if (Node->getOperand(0).getValueType() == MVT::f32)
      Lo = ExpandLibCall("__fixsfdi", Node, Hi);
    else
      Lo = ExpandLibCall("__fixdfdi", Node, Hi);
    break;
  case ISD::FP_TO_UINT:
    if (Node->getOperand(0).getValueType() == MVT::f32)
      Lo = ExpandLibCall("__fixunssfdi", Node, Hi);
    else
      Lo = ExpandLibCall("__fixunsdfdi", Node, Hi);
    break;

  case ISD::SHL:
    // If we can emit an efficient shift operation, do so now.
    if (ExpandShift(ISD::SHL, Node->getOperand(0), Node->getOperand(1), Lo, Hi))
      break;
    // Otherwise, emit a libcall.
    Lo = ExpandLibCall("__ashldi3", Node, Hi);
    break;

  case ISD::SRA:
    // If we can emit an efficient shift operation, do so now.
    if (ExpandShift(ISD::SRA, Node->getOperand(0), Node->getOperand(1), Lo, Hi))
      break;
    // Otherwise, emit a libcall.
    Lo = ExpandLibCall("__ashrdi3", Node, Hi);
    break;
  case ISD::SRL:
    // If we can emit an efficient shift operation, do so now.
    if (ExpandShift(ISD::SRL, Node->getOperand(0), Node->getOperand(1), Lo, Hi))
      break;
    // Otherwise, emit a libcall.
    Lo = ExpandLibCall("__lshrdi3", Node, Hi);
    break;

  case ISD::ADD:
    ExpandAddSub(true, Node->getOperand(0), Node->getOperand(1), Lo, Hi);
    break;
  case ISD::SUB:
    ExpandAddSub(false, Node->getOperand(0), Node->getOperand(1), Lo, Hi);
    break;
  case ISD::MUL:  Lo = ExpandLibCall("__muldi3" , Node, Hi); break;
  case ISD::SDIV: Lo = ExpandLibCall("__divdi3" , Node, Hi); break;
  case ISD::UDIV: Lo = ExpandLibCall("__udivdi3", Node, Hi); break;
  case ISD::SREM: Lo = ExpandLibCall("__moddi3" , Node, Hi); break;
  case ISD::UREM: Lo = ExpandLibCall("__umoddi3", Node, Hi); break;
  }

  // Remember in a map if the values will be reused later.
  if (!Node->hasOneUse()) {
    bool isNew = ExpandedNodes.insert(std::make_pair(Op,
                                            std::make_pair(Lo, Hi))).second;
    assert(isNew && "Value already expanded?!?");
  }
}


// SelectionDAG::Legalize - This is the entry point for the file.
//
void SelectionDAG::Legalize() {
  /// run - This is the main entry point to this class.
  ///
  SelectionDAGLegalize(*this).Run();
}

