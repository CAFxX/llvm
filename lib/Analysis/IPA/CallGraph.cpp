//===- CallGraph.cpp - Build a Module's call graph --------------------------=//
//
// This file implements call graph construction (from a module), and will
// eventually implement call graph serialization and deserialization for
// annotation support.
//
// This call graph represents a dynamic method invocation as a null method node.
// A call graph may only have up to one null method node that represents all of
// the dynamic method invocations.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/Writer.h"
#include "llvm/Module.h"
#include "llvm/Method.h"
#include "llvm/iOther.h"
#include "llvm/iTerminators.h"
#include "Support/STLExtras.h"
#include <algorithm>

AnalysisID cfg::CallGraph::ID(AnalysisID::create<cfg::CallGraph>());
//AnalysisID cfg::CallGraph::ID(AnalysisID::template AnalysisID<cfg::CallGraph>());

// getNodeFor - Return the node for the specified method or create one if it
// does not already exist.
//
cfg::CallGraphNode *cfg::CallGraph::getNodeFor(Method *M) {
  iterator I = MethodMap.find(M);
  if (I != MethodMap.end()) return I->second;

  assert(M->getParent() == Mod && "Method not in current module!");
  CallGraphNode *New = new CallGraphNode(M);

  MethodMap.insert(std::make_pair(M, New));
  return New;
}

// addToCallGraph - Add a method to the call graph, and link the node to all of
// the methods that it calls.
//
void cfg::CallGraph::addToCallGraph(Method *M) {
  CallGraphNode *Node = getNodeFor(M);

  // If this method has external linkage, 
  if (!M->hasInternalLinkage())
    Root->addCalledMethod(Node);

  for (Method::inst_iterator I = M->inst_begin(), E = M->inst_end();
       I != E; ++I) {
    // Dynamic calls will cause Null nodes to be created
    if (CallInst *CI = dyn_cast<CallInst>(*I))
      Node->addCalledMethod(getNodeFor(CI->getCalledMethod()));
    else if (InvokeInst *II = dyn_cast<InvokeInst>(*I))
      Node->addCalledMethod(getNodeFor(II->getCalledMethod()));
  }
}

bool cfg::CallGraph::run(Module *TheModule) {
  destroy();

  Mod = TheModule;

  // Create the root node of the module...
  Root = new CallGraphNode(0);

  // Add every method to the call graph...
  for_each(Mod->begin(), Mod->end(), bind_obj(this,&CallGraph::addToCallGraph));
  
  return false;
}

void cfg::CallGraph::destroy() {
  for (MethodMapTy::iterator I = MethodMap.begin(), E = MethodMap.end();
       I != E; ++I) {
    delete I->second;
  }
  MethodMap.clear();
}


void cfg::WriteToOutput(const CallGraphNode *CGN, std::ostream &o) {
  if (CGN->getMethod())
    o << "Call graph node for method: '" << CGN->getMethod()->getName() <<"'\n";
  else
    o << "Call graph node null method:\n";

  for (unsigned i = 0; i < CGN->size(); ++i)
    o << "  Calls method '" << (*CGN)[i]->getMethod()->getName() << "'\n";
  o << "\n";
}

void cfg::WriteToOutput(const CallGraph &CG, std::ostream &o) {
  WriteToOutput(CG.getRoot(), o);
  for (CallGraph::const_iterator I = CG.begin(), E = CG.end(); I != E; ++I)
    o << I->second;
}


//===----------------------------------------------------------------------===//
// Implementations of public modification methods
//

// Methods to keep a call graph up to date with a method that has been
// modified
//
void cfg::CallGraph::addMethodToModule(Method *Meth) {
  assert(0 && "not implemented");
  abort();
}

// removeMethodFromModule - Unlink the method from this module, returning it.
// Because this removes the method from the module, the call graph node is
// destroyed.  This is only valid if the method does not call any other
// methods (ie, there are no edges in it's CGN).  The easiest way to do this
// is to dropAllReferences before calling this.
//
Method *cfg::CallGraph::removeMethodFromModule(CallGraphNode *CGN) {
  assert(CGN->CalledMethods.empty() && "Cannot remove method from call graph"
	 " if it references other methods!");
  Method *M = CGN->getMethod();  // Get the method for the call graph node
  delete CGN;                    // Delete the call graph node for this method
  MethodMap.erase(M);            // Remove the call graph node from the map

  Mod->getMethodList().remove(M);
  return M;
}


// 
// Checks if a method contains any call instructions.
// Note that this uses the call graph only if one is provided.
// It does not build the call graph.
// 
bool IsLeafMethod(const Method* M, const cfg::CallGraph* CG) {
  if (CG) {
    const cfg::CallGraphNode *cgn = (*CG)[M];
    return (cgn->begin() == cgn->end());
  }

  for (Method::const_inst_iterator I = M->inst_begin(), E = M->inst_end();
       I != E; ++I)
    if ((*I)->getOpcode() == Instruction::Call)
      return false;
  return true;
}


