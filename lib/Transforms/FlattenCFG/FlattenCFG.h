#include "llvm/Transforms/IPO.h"
#include "llvm/Constant.h"
#include "llvm/Instructions.h"
#include "llvm/Module.h"
#include "llvm/Pass.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Support/CallSite.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/ADT/DenseSet.h"
#include <map>
#include <set>
#include <string>
#include <sstream>

using namespace llvm;

#define PassBoilerplate(Class,Switch,Description) \
	using namespace PassNS; \
	char Class::ID = 0; \
	static llvm::RegisterPass<Class> RegisterPass_Class(Switch, Description); \
	using namespace llvm;

typedef DenseMap<Function*, Function*> FunctionMap;
typedef std::set<Function*> FunctionSet;

template <class T> std::string to_string (const T& t) {
  std::stringstream ss;
  ss << t;
  return ss.str();
}

static std::string to_string(Function* f) {
  std::stringstream ss;
  ss << f->getFunctionType()->getReturnType()->getDescription() << " " 
      << f->getName().str() << "(";
  for (int i=0, n=f->getFunctionType()->getNumParams(); i < n; i++)
    ss << f->getFunctionType()->getParamType(i)->getDescription() << (i<n-1?", ":")");
  return ss.str();
}


