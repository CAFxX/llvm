//===- InstrInfoEmitter.cpp - Generate a Instruction Set Desc. ------------===//
//
// This tablegen backend is responsible for emitting a description of the target
// instruction set for the code generator.
//
//===----------------------------------------------------------------------===//

#include "InstrSelectorEmitter.h"
#include "CodeGenWrappers.h"
#include "Record.h"
#include "Support/Debug.h"

NodeType::ArgResultTypes NodeType::Translate(Record *R) {
  const std::string &Name = R->getName();
  if (Name == "DNVT_void") return Void;
  if (Name == "DNVT_val" ) return Val;
  if (Name == "DNVT_arg0") return Arg0;
  if (Name == "DNVT_ptr" ) return Ptr;
  throw "Unknown DagNodeValType '" + Name + "'!";
}

std::ostream &operator<<(std::ostream &OS, const TreePatternNode &N) {
  if (N.isLeaf())
    return OS << N.getType() << ":" << *N.getValue();
  OS << "(" << N.getType() << ":";
  OS << N.getOperator()->getName();
  
  const std::vector<TreePatternNode*> &Children = N.getChildren();
  if (!Children.empty()) {
    OS << " " << *Children[0];
    for (unsigned i = 1, e = Children.size(); i != e; ++i)
      OS << ", " << *Children[i];
  }  
  return OS << ")";
}
void TreePatternNode::dump() const { std::cerr << *this; }


/// ProcessNodeTypes - Process all of the node types in the current
/// RecordKeeper, turning them into the more accessible NodeTypes data
/// structure.
///
void InstrSelectorEmitter::ProcessNodeTypes() {
  std::vector<Record*> Nodes = Records.getAllDerivedDefinitions("DagNode");
  for (unsigned i = 0, e = Nodes.size(); i != e; ++i) {
    Record *Node = Nodes[i];
    
    // Translate the return type...
    NodeType::ArgResultTypes RetTy =
      NodeType::Translate(Node->getValueAsDef("RetType"));

    // Translate the arguments...
    ListInit *Args = Node->getValueAsListInit("ArgTypes");
    std::vector<NodeType::ArgResultTypes> ArgTypes;

    for (unsigned a = 0, e = Args->getSize(); a != e; ++a) {
      if (DefInit *DI = dynamic_cast<DefInit*>(Args->getElement(a)))
        ArgTypes.push_back(NodeType::Translate(DI->getDef()));
      else
        throw "In node " + Node->getName() + ", argument is not a Def!";

      if (a == 0 && ArgTypes.back() == NodeType::Arg0)
        throw "In node " + Node->getName() + ", arg 0 cannot have type 'arg0'!";
      if (ArgTypes.back() == NodeType::Void)
        throw "In node " + Node->getName() + ", args cannot be void type!";
    }
    if (RetTy == NodeType::Arg0 && Args->getSize() == 0)
      throw "In node " + Node->getName() +
            ", invalid return type for nullary node!";

    // Add the node type mapping now...
    NodeTypes[Node] = NodeType(RetTy, ArgTypes);
    DEBUG(std::cerr << "Got node type '" << Node->getName() << "'\n");
  }
}

static MVT::ValueType getIntrinsicType(Record *R) {
  // Check to see if this is a register or a register class...
  const std::vector<Record*> &SuperClasses = R->getSuperClasses();
  for (unsigned i = 0, e = SuperClasses.size(); i != e; ++i)
    if (SuperClasses[i]->getName() == "RegisterClass") {
      return getValueType(R->getValueAsDef("RegType"));
    } else if (SuperClasses[i]->getName() == "Register") {
      std::cerr << "WARNING: Explicit registers not handled yet!\n";
      return MVT::Other;
    } else if (SuperClasses[i]->getName() == "Nonterminal") {
      //std::cerr << "Warning nonterminal type not handled yet:" << R->getName()
      //          << "\n";
      return MVT::Other;
    }
  throw "Error: Unknown value used: " + R->getName();
}

// Parse the specified DagInit into a TreePattern which we can use.
//
TreePatternNode *InstrSelectorEmitter::ParseTreePattern(DagInit *DI,
                                                   const std::string &RecName) {
  Record *Operator = DI->getNodeType();

  if (!NodeTypes.count(Operator))
    throw "Illegal node for instruction pattern: '" + Operator->getName() +"'!";

  const std::vector<Init*> &Args = DI->getArgs();
  std::vector<TreePatternNode*> Children;
  
  for (unsigned i = 0, e = Args.size(); i != e; ++i) {
    Init *Arg = Args[i];
    if (DagInit *DI = dynamic_cast<DagInit*>(Arg)) {
      Children.push_back(ParseTreePattern(DI, RecName));
    } else if (DefInit *DI = dynamic_cast<DefInit*>(Arg)) {
      Children.push_back(new TreePatternNode(DI));
      // If it's a regclass or something else known, set the type.
      Children.back()->setType(getIntrinsicType(DI->getDef()));
    } else {
      Arg->dump();
      throw "Unknown value for tree pattern in '" + RecName + "'!";
    }
  }

  return new TreePatternNode(Operator, Children);
}

// UpdateNodeType - Set the node type of N to VT if VT contains information.  If
// N already contains a conflicting type, then throw an exception
//
static bool UpdateNodeType(TreePatternNode *N, MVT::ValueType VT,
                           const std::string &RecName) {
  if (VT == MVT::Other || N->getType() == VT) return false;

  if (N->getType() == MVT::Other) {
    N->setType(VT);
    return true;
  }

  throw "Type inferfence contradiction found for pattern " + RecName;
}

// InferTypes - Perform type inference on the tree, returning true if there
// are any remaining untyped nodes and setting MadeChange if any changes were
// made.
bool InstrSelectorEmitter::InferTypes(TreePatternNode *N,
                                      const std::string &RecName,
                                      bool &MadeChange) {
  if (N->isLeaf()) return N->getType() == MVT::Other;

  bool AnyUnset = false;
  Record *Operator = N->getOperator();
  assert(NodeTypes.count(Operator) && "No node info for node!");
  const NodeType &NT = NodeTypes[Operator];

  // Check to see if we can infer anything about the argument types from the
  // return types...
  const std::vector<TreePatternNode*> &Children = N->getChildren();
  if (Children.size() != NT.ArgTypes.size())
    throw "In record " + RecName + " incorrect number of children for " +
          Operator->getName() + " node!";

  for (unsigned i = 0, e = Children.size(); i != e; ++i) {
    AnyUnset |= InferTypes(Children[i], RecName, MadeChange);


    switch (NT.ArgTypes[i]) {
    case NodeType::Arg0:
      MadeChange |= UpdateNodeType(Children[i], Children[0]->getType(),RecName);
      break;
    case NodeType::Val:
      if (Children[i]->getType() == MVT::isVoid)
        throw "In pattern for " + RecName + " should not get a void node!";
      break;
    case NodeType::Ptr:
      MadeChange |= UpdateNodeType(Children[i],Target.getPointerType(),RecName);
      break;
    default: assert(0 && "Invalid argument ArgType!");
    }
  }

  // See if we can infer anything about the return type now...
  switch (NT.ResultType) {
  case NodeType::Void:
    MadeChange |= UpdateNodeType(N, MVT::isVoid, RecName);
    break;
  case NodeType::Arg0:
    MadeChange |= UpdateNodeType(N, Children[0]->getType(), RecName);
    break;

  case NodeType::Ptr:
    MadeChange |= UpdateNodeType(N, Target.getPointerType(), RecName);
    break;
  case NodeType::Val:
    if (N->getType() == MVT::isVoid)
      throw "In pattern for " + RecName + " should not get a void node!";
    break;
  default:
    assert(0 && "Unhandled type constraint!");
    break;
  }

  return AnyUnset | N->getType() == MVT::Other;
}


// ReadAndCheckPattern - Parse the specified DagInit into a pattern and then
// perform full type inference.
//
TreePatternNode *InstrSelectorEmitter::ReadAndCheckPattern(DagInit *DI,
                                                  const std::string &RecName) {
  // First, parse the pattern...
  TreePatternNode *Pattern = ParseTreePattern(DI, RecName);
  
  bool MadeChange, AnyUnset;
  do {
    MadeChange = false;
    AnyUnset = InferTypes(Pattern, RecName, MadeChange);
    if (AnyUnset && !MadeChange) {
      std::cerr << "In pattern: " << *Pattern << "\n";
      throw "Cannot infer types for " + RecName;
    }
  } while (AnyUnset || MadeChange);

  return Pattern;
}

// ProcessNonTerminals - Read in all nonterminals and incorporate them into
// our pattern database.
void InstrSelectorEmitter::ProcessNonTerminals() {
  std::vector<Record*> NTs = Records.getAllDerivedDefinitions("Nonterminal");
  for (unsigned i = 0, e = NTs.size(); i != e; ++i) {
    DagInit *DI = NTs[i]->getValueAsDag("Pattern");

    TreePatternNode *Pattern = ReadAndCheckPattern(DI, NTs[i]->getName());

    DEBUG(std::cerr << "Parsed nonterm pattern " << NTs[i]->getName() << "\t= "
          << *Pattern << "\n");
  }
}


/// ProcessInstructionPatterns - Read in all subclasses of Instruction, and
/// process those with a useful Pattern field.
///
void InstrSelectorEmitter::ProcessInstructionPatterns() {
  std::vector<Record*> Insts = Records.getAllDerivedDefinitions("Instruction");
  for (unsigned i = 0, e = Insts.size(); i != e; ++i) {
    Record *Inst = Insts[i];
    if (DagInit *DI = dynamic_cast<DagInit*>(Inst->getValueInit("Pattern"))) {
      TreePatternNode *Pattern = ReadAndCheckPattern(DI, Inst->getName());

      DEBUG(std::cerr << "Parsed inst pattern " << Inst->getName() << "\t= "
                      << *Pattern << "\n");
    }
  }
}


void InstrSelectorEmitter::run(std::ostream &OS) {
  // Type-check all of the node types to ensure we "understand" them.
  ProcessNodeTypes();
  
  // Read in all of the nonterminals...
  //ProcessNonTerminals();

  // Read all of the instruction patterns in...
  ProcessInstructionPatterns();

  // Read all of the Expander patterns in...
  
}
