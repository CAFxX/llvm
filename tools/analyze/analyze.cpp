//===----------------------------------------------------------------------===//
// LLVM 'Analyze' UTILITY 
//
// This utility is designed to print out the results of running various analysis
// passes on a program.  This is useful for understanding a program, or for 
// debugging an analysis pass.
//
//  analyze --help           - Output information about command line switches
//  analyze --quiet          - Do not print analysis name before output
//
//===----------------------------------------------------------------------===//

#include "llvm/Instruction.h"
#include "llvm/Module.h"
#include "llvm/Function.h"
#include "llvm/iPHINode.h"
#include "llvm/PassManager.h"
#include "llvm/Bytecode/Reader.h"
#include "llvm/Assembly/Parser.h"
#include "llvm/Assembly/PrintModulePass.h"
#include "llvm/Analysis/Writer.h"
#include "llvm/Analysis/InstForest.h"
#include "llvm/Analysis/Dominators.h"
#include "llvm/Analysis/IntervalPartition.h"
#include "llvm/Analysis/Expressions.h"
#include "llvm/Analysis/InductionVariable.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/DataStructure.h"
#include "llvm/Analysis/FindUnsafePointerTypes.h"
#include "llvm/Analysis/FindUsedTypes.h"
#include "llvm/Support/InstIterator.h"
#include "Support/CommandLine.h"
#include <algorithm>
#include <iostream>

using std::ostream;
using std::string;

//===----------------------------------------------------------------------===//
// printPass - Specify how to print out a pass.  For most passes, the standard
// way of using operator<< works great, so we use it directly...
//
template<class PassType>
static void printPass(PassType &P, ostream &O, Module *M) {
  O << P;
}

template<class PassType>
static void printPass(PassType &P, ostream &O, Function *F) {
  O << P;
}

// Other classes require more information to print out information, so we
// specialize the template here for them...
//
template<>
static void printPass(DataStructure &P, ostream &O, Module *M) {
  P.print(O, M);
}

template<>
static void printPass(FindUsedTypes &FUT, ostream &O, Module *M) {
  FUT.printTypes(O, M);
}

template<>
static void printPass(FindUnsafePointerTypes &FUPT, ostream &O,
                      Module *M) {
  FUPT.printResults(M, O);
}



template <class PassType, class PassName>
class PassPrinter;  // Do not implement

template <class PassName>
class PassPrinter<Pass, PassName> : public Pass {
  const string Message;
  const AnalysisID ID;
public:
  PassPrinter(const string &M, AnalysisID id) : Message(M), ID(id) {}
  
  virtual bool run(Module *M) {
    std::cout << Message << "\n";
    printPass(getAnalysis<PassName>(ID), std::cout, M);
    return false;
  }

  virtual void getAnalysisUsageInfo(Pass::AnalysisSet &Required,
                                    Pass::AnalysisSet &Destroyed,
                                    Pass::AnalysisSet &Provided) {
    Required.push_back(ID);
  }
};

template <class PassName>
class PassPrinter<MethodPass, PassName> : public MethodPass {
  const string Message;
  const AnalysisID ID;
public:
  PassPrinter(const string &M, AnalysisID id) : Message(M), ID(id) {}
  
  virtual bool runOnMethod(Function *F) {
    std::cout << Message << " on method '" << F->getName() << "'\n";
    printPass(getAnalysis<PassName>(ID), std::cout, F);
    return false;
  }

  virtual void getAnalysisUsageInfo(Pass::AnalysisSet &Required,
                                    Pass::AnalysisSet &Destroyed,
                                    Pass::AnalysisSet &Provided) {
    Required.push_back(ID);
  }
};



template <class PassType, class PassName, AnalysisID &ID>
Pass *New(const string &Message) {
  return new PassPrinter<PassType, PassName>(Message, ID);
}
template <class PassType, class PassName>
Pass *New(const string &Message) {
  return new PassPrinter<PassType, PassName>(Message, PassName::ID);
}



Pass *NewPrintFunction(const string &Message) {
  return new PrintMethodPass(Message, &std::cout);
}
Pass *NewPrintModule(const string &Message) {
  return new PrintModulePass(&std::cout);
}

struct InstForest : public MethodPass {
  void doit(Function *F) {
    std::cout << analysis::InstForest<char>(F);
  }
};

struct IndVars : public MethodPass {
  void doit(Function *F) {
    cfg::LoopInfo &LI = getAnalysis<cfg::LoopInfo>();
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I)
      if (PHINode *PN = dyn_cast<PHINode>(*I)) {
        InductionVariable IV(PN, &LI);
        if (IV.InductionType != InductionVariable::Unknown)
          std::cout << IV;
      }
  }

  void getAnalysisUsageInfo(Pass::AnalysisSet &Req,
                            Pass::AnalysisSet &, Pass::AnalysisSet &) {
    Req.push_back(cfg::LoopInfo::ID);
  }
};

struct Exprs : public MethodPass {
  static void doit(Function *F) {
    std::cout << "Classified expressions for: " << F->getName() << "\n";
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
      std::cout << *I;
      
      if ((*I)->getType() == Type::VoidTy) continue;
      analysis::ExprType R = analysis::ClassifyExpression(*I);
      if (R.Var == *I) continue;  // Doesn't tell us anything
      
      std::cout << "\t\tExpr =";
      switch (R.ExprTy) {
      case analysis::ExprType::ScaledLinear:
        WriteAsOperand(std::cout << "(", (Value*)R.Scale) << " ) *";
        // fall through
      case analysis::ExprType::Linear:
        WriteAsOperand(std::cout << "(", R.Var) << " )";
        if (R.Offset == 0) break;
        else std::cout << " +";
        // fall through
      case analysis::ExprType::Constant:
        if (R.Offset) WriteAsOperand(std::cout, (Value*)R.Offset);
        else std::cout << " 0";
        break;
      }
      std::cout << "\n\n";
    }
  }
};


template<class TraitClass>
class PrinterPass : public TraitClass {
  const string Message;
public:
  PrinterPass(const string &M) : Message(M) {}
  
  virtual bool runOnMethod(Function *F) {
    std::cout << Message << " on method '" << F->getName() << "'\n";

    TraitClass::doit(F);
    return false;
  }
};


template<class PassClass>
Pass *Create(const string &Message) {
  return new PassClass(Message);
}



enum Ans {
  // global analyses
  print, intervals, exprs, instforest, loops, indvars,

  // ip analyses
  printmodule, callgraph, datastructure, printusedtypes, unsafepointertypes,

  domset, idom, domtree, domfrontier,
  postdomset, postidom, postdomtree, postdomfrontier,
};

cl::String InputFilename ("", "Load <arg> file to analyze", cl::NoFlags, "-");
cl::Flag   Quiet         ("q", "Don't print analysis pass names");
cl::Alias  QuietA        ("quiet", "Alias for -q", cl::NoFlags, Quiet);
cl::EnumList<enum Ans> AnalysesList(cl::NoFlags,
  clEnumVal(print          , "Print each function"),
  clEnumVal(intervals      , "Print Interval Partitions"),
  clEnumVal(exprs          , "Classify Expressions"),
  clEnumVal(instforest     , "Print Instruction Forest"),
  clEnumVal(loops          , "Print natural loops"),
  clEnumVal(indvars        , "Print Induction Variables"),

  clEnumVal(printmodule    , "Print entire module"),
  clEnumVal(callgraph      , "Print Call Graph"),
  clEnumVal(datastructure  , "Print data structure information"),
  clEnumVal(printusedtypes , "Print types used by module"),
  clEnumVal(unsafepointertypes, "Print unsafe pointer types"),

  clEnumVal(domset         , "Print Dominator Sets"),
  clEnumVal(idom           , "Print Immediate Dominators"),
  clEnumVal(domtree        , "Print Dominator Tree"),
  clEnumVal(domfrontier    , "Print Dominance Frontier"),

  clEnumVal(postdomset     , "Print Postdominator Sets"),
  clEnumVal(postidom       , "Print Immediate Postdominators"),
  clEnumVal(postdomtree    , "Print Post Dominator Tree"),
  clEnumVal(postdomfrontier, "Print Postdominance Frontier"),
0);


struct {
  enum Ans AnID;
  Pass *(*PassConstructor)(const string &Message);
} AnTable[] = {
  // Global analyses
  { print             , NewPrintFunction                        },
  { intervals         , New<MethodPass, cfg::IntervalPartition> },
  { loops             , New<MethodPass, cfg::LoopInfo>          },
  { instforest        , Create<PrinterPass<InstForest> >        },
  { indvars           , Create<PrinterPass<IndVars> >           },
  { exprs             , Create<PrinterPass<Exprs> >             },

  // IP Analyses...
  { printmodule       , NewPrintModule                    },
  { printusedtypes    , New<Pass, FindUsedTypes>          },
  { callgraph         , New<Pass, CallGraph>              },
  { datastructure     , New<Pass, DataStructure>          },
  { unsafepointertypes, New<Pass, FindUnsafePointerTypes> },

  // Dominator analyses
  { domset            , New<MethodPass, cfg::DominatorSet>        },
  { idom              , New<MethodPass, cfg::ImmediateDominators> },
  { domtree           , New<MethodPass, cfg::DominatorTree>       },
  { domfrontier       , New<MethodPass, cfg::DominanceFrontier>   },

  { postdomset        , New<MethodPass, cfg::DominatorSet, cfg::DominatorSet::PostDomID> },
  { postidom          , New<MethodPass, cfg::ImmediateDominators, cfg::ImmediateDominators::PostDomID> },
  { postdomtree       , New<MethodPass, cfg::DominatorTree, cfg::DominatorTree::PostDomID> },
  { postdomfrontier   , New<MethodPass, cfg::DominanceFrontier, cfg::DominanceFrontier::PostDomID> },
};

int main(int argc, char **argv) {
  cl::ParseCommandLineOptions(argc, argv, " llvm analysis printer tool\n");

  Module *CurMod = 0;
  try {
    CurMod = ParseBytecodeFile(InputFilename);
    if (!CurMod && !(CurMod = ParseAssemblyFile(InputFilename))){
      std::cerr << "Input file didn't read correctly.\n";
      return 1;
    }
  } catch (const ParseException &E) {
    std::cerr << E.getMessage() << "\n";
    return 1;
  }

  // Create a PassManager to hold and optimize the collection of passes we are
  // about to build...
  //
  PassManager Analyses;

  // Loop over all of the analyses looking for analyses to run...
  for (unsigned i = 0; i < AnalysesList.size(); ++i) {
    enum Ans AnalysisPass = AnalysesList[i];

    for (unsigned j = 0; j < sizeof(AnTable)/sizeof(AnTable[0]); ++j) {
      if (AnTable[j].AnID == AnalysisPass) {
        string Message;
        if (!Quiet)
          Message = "\nRunning: '" + 
            string(AnalysesList.getArgDescription(AnalysisPass)) + "' analysis";
        Analyses.add(AnTable[j].PassConstructor(Message));
        break;                       // get an error later
      }
    }
  }  

  Analyses.run(CurMod);

  delete CurMod;
  return 0;
}
