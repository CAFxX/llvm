//===- SimpleStructMutation.cpp - Swap structure elements around -*- C++ -*--=//
//
// This pass does a simple transformation that swaps all of the elements of the
// struct types in the program around.
//
//===----------------------------------------------------------------------===//


#include "llvm/Transforms/IPO/SimpleStructMutation.h"
#include "llvm/Transforms/IPO/MutateStructTypes.h"
#include "llvm/Analysis/FindUsedTypes.h"
#include "llvm/Analysis/FindUnsafePointerTypes.h"
#include "../TransformInternals.h"
#include <algorithm>
#include <iostream>
using std::vector;
using std::set;
using std::pair;

namespace {
  class SimpleStructMutation : public MutateStructTypes {
  public:
    enum Transform { SwapElements, SortElements } CurrentXForm;
    
    SimpleStructMutation(enum Transform XForm) : CurrentXForm(XForm) {}
    
    virtual bool run(Module *M) {
      setTransforms(getTransforms(M, CurrentXForm));
      bool Changed = MutateStructTypes::run(M);
      clearTransforms();
      return Changed;
    }
    
    // getAnalysisUsage - This function needs the results of the
    // FindUsedTypes and FindUnsafePointerTypes analysis passes...
    //
    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.addRequired(FindUsedTypes::ID);
      AU.addRequired(FindUnsafePointerTypes::ID);
      MutateStructTypes::getAnalysisUsage(AU);
    }
    
  private:
    TransformsType getTransforms(Module *M, enum Transform);
  };
}  // end anonymous namespace



// PruneTypes - Given a type Ty, make sure that neither it, or one of its
// subtypes, occur in TypesToModify.
//
static void PruneTypes(const Type *Ty, set<const StructType*> &TypesToModify,
                       set<const Type*> &ProcessedTypes) {
  if (ProcessedTypes.count(Ty)) return;  // Already been checked
  ProcessedTypes.insert(Ty);

  // If the element is in TypesToModify, remove it now...
  if (const StructType *ST = dyn_cast<StructType>(Ty)) {
    TypesToModify.erase(ST);  // This doesn't fail if the element isn't present
    std::cerr << "Unable to swap type: " << ST << "\n";
  }

  // Remove all types that this type contains as well... do not remove types
  // that are referenced only through pointers, because we depend on the size of
  // the pointer, not on what the structure points to.
  //
  for (Type::subtype_iterator I = Ty->subtype_begin(), E = Ty->subtype_end();
       I != E; ++I) {
    if (!isa<PointerType>(*I))
      PruneTypes(*I, TypesToModify, ProcessedTypes);
  }
}

static bool FirstLess(const pair<unsigned, unsigned> &LHS,
                      const pair<unsigned, unsigned> &RHS) {
  return LHS.second < RHS.second;
}

static unsigned getIndex(const vector<pair<unsigned, unsigned> > &Vec,
                         unsigned Field) {
  for (unsigned i = 0; ; ++i)
    if (Vec[i].first == Field) return i;
}

static inline void GetTransformation(const StructType *ST,
                                     vector<int> &Transform,
                                enum SimpleStructMutation::Transform XForm) {
  unsigned NumElements = ST->getElementTypes().size();
  Transform.reserve(NumElements);

  switch (XForm) {
  case SimpleStructMutation::SwapElements:
    // The transformation to do is: just simply swap the elements
    for (unsigned i = 0; i < NumElements; ++i)
      Transform.push_back(NumElements-i-1);
    break;

  case SimpleStructMutation::SortElements: {
    vector<pair<unsigned, unsigned> > ElList;

    // Build mapping from index to size
    for (unsigned i = 0; i < NumElements; ++i)
      ElList.push_back(
              std::make_pair(i, TD.getTypeSize(ST->getElementTypes()[i])));

    sort(ElList.begin(), ElList.end(), ptr_fun(FirstLess));

    for (unsigned i = 0; i < NumElements; ++i)
      Transform.push_back(getIndex(ElList, i));

    break;
  }
  }
}


SimpleStructMutation::TransformsType
  SimpleStructMutation::getTransforms(Module *M, enum Transform XForm) {
  // We need to know which types to modify, and which types we CAN'T modify
  // TODO: Do symbol tables as well

  // Get the results out of the analyzers...
  FindUsedTypes          &FUT = getAnalysis<FindUsedTypes>();
  const set<const Type *> &UsedTypes  = FUT.getTypes();

  FindUnsafePointerTypes &FUPT = getAnalysis<FindUnsafePointerTypes>();
  const set<PointerType*> &UnsafePTys = FUPT.getUnsafeTypes();



  // Combine the two sets, weeding out non structure types.  Closures in C++
  // sure would be nice.
  set<const StructType*> TypesToModify;
  for (set<const Type *>::const_iterator I = UsedTypes.begin(), 
         E = UsedTypes.end(); I != E; ++I)
    if (const StructType *ST = dyn_cast<StructType>(*I))
      TypesToModify.insert(ST);


  // Go through the Unsafe types and remove all types from TypesToModify that we
  // are not allowed to modify, because that would be unsafe.
  //
  set<const Type*> ProcessedTypes;
  for (set<PointerType*>::const_iterator I = UnsafePTys.begin(),
         E = UnsafePTys.end(); I != E; ++I) {
    //cerr << "Pruning type: " << *I << "\n";
    PruneTypes(*I, TypesToModify, ProcessedTypes);
  }


  // Build up a set of structure types that we are going to modify, and
  // information describing how to modify them.
  std::map<const StructType*, vector<int> > Transforms;

  for (set<const StructType*>::iterator I = TypesToModify.begin(),
         E = TypesToModify.end(); I != E; ++I) {
    const StructType *ST = *I;

    vector<int> &Transform = Transforms[ST];  // Fill in the map directly
    GetTransformation(ST, Transform, XForm);
  }
  
  return Transforms;
}


Pass *createSwapElementsPass() {
  return new SimpleStructMutation(SimpleStructMutation::SwapElements);
}
Pass *createSortElementsPass() {
  return new SimpleStructMutation(SimpleStructMutation::SortElements);
}

