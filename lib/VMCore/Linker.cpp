//===- Linker.cpp - Module Linker Implementation --------------------------===//
// 
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file implements the LLVM module linker.
//
// Specifically, this:
//  * Merges global variables between the two modules
//    * Uninit + Uninit = Init, Init + Uninit = Init, Init + Init = Error if !=
//  * Merges functions between two modules
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/Linker.h"
#include "llvm/Constants.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Module.h"
#include "llvm/SymbolTable.h"
#include "llvm/Instructions.h"
#include "llvm/Assembly/Writer.h"
#include "llvm/System/Path.h"
#include <iostream>
#include <sstream>
using namespace llvm;

// Error - Simple wrapper function to conditionally assign to E and return true.
// This just makes error return conditions a little bit simpler...
//
static inline bool Error(std::string *E, const std::string &Message) {
  if (E) *E = Message;
  return true;
}

static std::string ToStr(const Type *Ty, const Module *M) {
  std::ostringstream OS;
  WriteTypeSymbolic(OS, Ty, M);
  return OS.str();
}

//
// Function: ResolveTypes()
//
// Description:
//  Attempt to link the two specified types together.
//
// Inputs:
//  DestTy - The type to which we wish to resolve.
//  SrcTy  - The original type which we want to resolve.
//  Name   - The name of the type.
//
// Outputs:
//  DestST - The symbol table in which the new type should be placed.
//
// Return value:
//  true  - There is an error and the types cannot yet be linked.
//  false - No errors.
//
static bool ResolveTypes(const Type *DestTy, const Type *SrcTy,
                         SymbolTable *DestST, const std::string &Name) {
  if (DestTy == SrcTy) return false;       // If already equal, noop

  // Does the type already exist in the module?
  if (DestTy && !isa<OpaqueType>(DestTy)) {  // Yup, the type already exists...
    if (const OpaqueType *OT = dyn_cast<OpaqueType>(SrcTy)) {
      const_cast<OpaqueType*>(OT)->refineAbstractTypeTo(DestTy);
    } else {
      return true;  // Cannot link types... neither is opaque and not-equal
    }
  } else {                       // Type not in dest module.  Add it now.
    if (DestTy)                  // Type _is_ in module, just opaque...
      const_cast<OpaqueType*>(cast<OpaqueType>(DestTy))
                           ->refineAbstractTypeTo(SrcTy);
    else if (!Name.empty())
      DestST->insert(Name, const_cast<Type*>(SrcTy));
  }
  return false;
}

static const FunctionType *getFT(const PATypeHolder &TH) {
  return cast<FunctionType>(TH.get());
}
static const StructType *getST(const PATypeHolder &TH) {
  return cast<StructType>(TH.get());
}

// RecursiveResolveTypes - This is just like ResolveTypes, except that it
// recurses down into derived types, merging the used types if the parent types
// are compatible.
//
static bool RecursiveResolveTypesI(const PATypeHolder &DestTy,
                                   const PATypeHolder &SrcTy,
                                   SymbolTable *DestST, const std::string &Name,
                std::vector<std::pair<PATypeHolder, PATypeHolder> > &Pointers) {
  const Type *SrcTyT = SrcTy.get();
  const Type *DestTyT = DestTy.get();
  if (DestTyT == SrcTyT) return false;       // If already equal, noop
  
  // If we found our opaque type, resolve it now!
  if (isa<OpaqueType>(DestTyT) || isa<OpaqueType>(SrcTyT))
    return ResolveTypes(DestTyT, SrcTyT, DestST, Name);
  
  // Two types cannot be resolved together if they are of different primitive
  // type.  For example, we cannot resolve an int to a float.
  if (DestTyT->getTypeID() != SrcTyT->getTypeID()) return true;

  // Otherwise, resolve the used type used by this derived type...
  switch (DestTyT->getTypeID()) {
  case Type::FunctionTyID: {
    if (cast<FunctionType>(DestTyT)->isVarArg() !=
        cast<FunctionType>(SrcTyT)->isVarArg() ||
        cast<FunctionType>(DestTyT)->getNumContainedTypes() !=
        cast<FunctionType>(SrcTyT)->getNumContainedTypes())
      return true;
    for (unsigned i = 0, e = getFT(DestTy)->getNumContainedTypes(); i != e; ++i)
      if (RecursiveResolveTypesI(getFT(DestTy)->getContainedType(i),
                                 getFT(SrcTy)->getContainedType(i), DestST, "",
                                 Pointers))
        return true;
    return false;
  }
  case Type::StructTyID: {
    if (getST(DestTy)->getNumContainedTypes() != 
        getST(SrcTy)->getNumContainedTypes()) return 1;
    for (unsigned i = 0, e = getST(DestTy)->getNumContainedTypes(); i != e; ++i)
      if (RecursiveResolveTypesI(getST(DestTy)->getContainedType(i),
                                 getST(SrcTy)->getContainedType(i), DestST, "",
                                 Pointers))
        return true;
    return false;
  }
  case Type::ArrayTyID: {
    const ArrayType *DAT = cast<ArrayType>(DestTy.get());
    const ArrayType *SAT = cast<ArrayType>(SrcTy.get());
    if (DAT->getNumElements() != SAT->getNumElements()) return true;
    return RecursiveResolveTypesI(DAT->getElementType(), SAT->getElementType(),
                                  DestST, "", Pointers);
  }
  case Type::PointerTyID: {
    // If this is a pointer type, check to see if we have already seen it.  If
    // so, we are in a recursive branch.  Cut off the search now.  We cannot use
    // an associative container for this search, because the type pointers (keys
    // in the container) change whenever types get resolved...
    //
    for (unsigned i = 0, e = Pointers.size(); i != e; ++i)
      if (Pointers[i].first == DestTy)
        return Pointers[i].second != SrcTy;

    // Otherwise, add the current pointers to the vector to stop recursion on
    // this pair.
    Pointers.push_back(std::make_pair(DestTyT, SrcTyT));
    bool Result =
      RecursiveResolveTypesI(cast<PointerType>(DestTy.get())->getElementType(),
                             cast<PointerType>(SrcTy.get())->getElementType(),
                             DestST, "", Pointers);
    Pointers.pop_back();
    return Result;
  }
  default: assert(0 && "Unexpected type!"); return true;
  }  
}

static bool RecursiveResolveTypes(const PATypeHolder &DestTy,
                                  const PATypeHolder &SrcTy,
                                  SymbolTable *DestST, const std::string &Name){
  std::vector<std::pair<PATypeHolder, PATypeHolder> > PointerTypes;
  return RecursiveResolveTypesI(DestTy, SrcTy, DestST, Name, PointerTypes);
}


// LinkTypes - Go through the symbol table of the Src module and see if any
// types are named in the src module that are not named in the Dst module.
// Make sure there are no type name conflicts.
//
static bool LinkTypes(Module *Dest, const Module *Src, std::string *Err) {
  SymbolTable       *DestST = &Dest->getSymbolTable();
  const SymbolTable *SrcST  = &Src->getSymbolTable();

  // Look for a type plane for Type's...
  SymbolTable::type_const_iterator TI = SrcST->type_begin();
  SymbolTable::type_const_iterator TE = SrcST->type_end();
  if (TI == TE) return false;  // No named types, do nothing.

  // Some types cannot be resolved immediately because they depend on other
  // types being resolved to each other first.  This contains a list of types we
  // are waiting to recheck.
  std::vector<std::string> DelayedTypesToResolve;

  for ( ; TI != TE; ++TI ) {
    const std::string &Name = TI->first;
    const Type *RHS = TI->second;

    // Check to see if this type name is already in the dest module...
    Type *Entry = DestST->lookupType(Name);

    if (ResolveTypes(Entry, RHS, DestST, Name)) {
      // They look different, save the types 'till later to resolve.
      DelayedTypesToResolve.push_back(Name);
    }
  }

  // Iteratively resolve types while we can...
  while (!DelayedTypesToResolve.empty()) {
    // Loop over all of the types, attempting to resolve them if possible...
    unsigned OldSize = DelayedTypesToResolve.size();

    // Try direct resolution by name...
    for (unsigned i = 0; i != DelayedTypesToResolve.size(); ++i) {
      const std::string &Name = DelayedTypesToResolve[i];
      Type *T1 = SrcST->lookupType(Name);
      Type *T2 = DestST->lookupType(Name);
      if (!ResolveTypes(T2, T1, DestST, Name)) {
        // We are making progress!
        DelayedTypesToResolve.erase(DelayedTypesToResolve.begin()+i);
        --i;
      }
    }

    // Did we not eliminate any types?
    if (DelayedTypesToResolve.size() == OldSize) {
      // Attempt to resolve subelements of types.  This allows us to merge these
      // two types: { int* } and { opaque* }
      for (unsigned i = 0, e = DelayedTypesToResolve.size(); i != e; ++i) {
        const std::string &Name = DelayedTypesToResolve[i];
        PATypeHolder T1(SrcST->lookupType(Name));
        PATypeHolder T2(DestST->lookupType(Name));

        if (!RecursiveResolveTypes(T2, T1, DestST, Name)) {
          // We are making progress!
          DelayedTypesToResolve.erase(DelayedTypesToResolve.begin()+i);
          
          // Go back to the main loop, perhaps we can resolve directly by name
          // now...
          break;
        }
      }

      // If we STILL cannot resolve the types, then there is something wrong.
      // Report the warning and delete one of the names.
      if (DelayedTypesToResolve.size() == OldSize) {
        const std::string &Name = DelayedTypesToResolve.back();
        
        const Type *T1 = SrcST->lookupType(Name);
        const Type *T2 = DestST->lookupType(Name);
        std::cerr << "WARNING: Type conflict between types named '" << Name
                  <<  "'.\n    Src='";
        WriteTypeSymbolic(std::cerr, T1, Src);
        std::cerr << "'.\n   Dest='";
        WriteTypeSymbolic(std::cerr, T2, Dest);
        std::cerr << "'\n";

        // Remove the symbol name from the destination.
        DelayedTypesToResolve.pop_back();
      }
    }
  }


  return false;
}

static void PrintMap(const std::map<const Value*, Value*> &M) {
  for (std::map<const Value*, Value*>::const_iterator I = M.begin(), E =M.end();
       I != E; ++I) {
    std::cerr << " Fr: " << (void*)I->first << " ";
    I->first->dump();
    std::cerr << " To: " << (void*)I->second << " ";
    I->second->dump();
    std::cerr << "\n";
  }
}


// RemapOperand - Use LocalMap and GlobalMap to convert references from one
// module to another.  This is somewhat sophisticated in that it can
// automatically handle constant references correctly as well...
//
static Value *RemapOperand(const Value *In,
                           std::map<const Value*, Value*> &LocalMap,
                           std::map<const Value*, Value*> *GlobalMap) {
  std::map<const Value*,Value*>::const_iterator I = LocalMap.find(In);
  if (I != LocalMap.end()) return I->second;

  if (GlobalMap) {
    I = GlobalMap->find(In);
    if (I != GlobalMap->end()) return I->second;
  }

  // Check to see if it's a constant that we are interesting in transforming...
  if (const Constant *CPV = dyn_cast<Constant>(In)) {
    if ((!isa<DerivedType>(CPV->getType()) && !isa<ConstantExpr>(CPV)) ||
        isa<ConstantAggregateZero>(CPV))
      return const_cast<Constant*>(CPV);   // Simple constants stay identical...

    Constant *Result = 0;

    if (const ConstantArray *CPA = dyn_cast<ConstantArray>(CPV)) {
      std::vector<Constant*> Operands(CPA->getNumOperands());
      for (unsigned i = 0, e = CPA->getNumOperands(); i != e; ++i)
        Operands[i] =
          cast<Constant>(RemapOperand(CPA->getOperand(i), LocalMap, GlobalMap));
      Result = ConstantArray::get(cast<ArrayType>(CPA->getType()), Operands);
    } else if (const ConstantStruct *CPS = dyn_cast<ConstantStruct>(CPV)) {
      std::vector<Constant*> Operands(CPS->getNumOperands());
      for (unsigned i = 0, e = CPS->getNumOperands(); i != e; ++i)
        Operands[i] =
          cast<Constant>(RemapOperand(CPS->getOperand(i), LocalMap, GlobalMap));
      Result = ConstantStruct::get(cast<StructType>(CPS->getType()), Operands);
    } else if (isa<ConstantPointerNull>(CPV) || isa<UndefValue>(CPV)) {
      Result = const_cast<Constant*>(CPV);
    } else if (isa<GlobalValue>(CPV)) {
      Result = cast<Constant>(RemapOperand(CPV, LocalMap, GlobalMap));
    } else if (const ConstantExpr *CE = dyn_cast<ConstantExpr>(CPV)) {
      if (CE->getOpcode() == Instruction::GetElementPtr) {
        Value *Ptr = RemapOperand(CE->getOperand(0), LocalMap, GlobalMap);
        std::vector<Constant*> Indices;
        Indices.reserve(CE->getNumOperands()-1);
        for (unsigned i = 1, e = CE->getNumOperands(); i != e; ++i)
          Indices.push_back(cast<Constant>(RemapOperand(CE->getOperand(i),
                                                        LocalMap, GlobalMap)));

        Result = ConstantExpr::getGetElementPtr(cast<Constant>(Ptr), Indices);
      } else if (CE->getNumOperands() == 1) {
        // Cast instruction
        assert(CE->getOpcode() == Instruction::Cast);
        Value *V = RemapOperand(CE->getOperand(0), LocalMap, GlobalMap);
        Result = ConstantExpr::getCast(cast<Constant>(V), CE->getType());
      } else if (CE->getNumOperands() == 3) {
        // Select instruction
        assert(CE->getOpcode() == Instruction::Select);
        Value *V1 = RemapOperand(CE->getOperand(0), LocalMap, GlobalMap);
        Value *V2 = RemapOperand(CE->getOperand(1), LocalMap, GlobalMap);
        Value *V3 = RemapOperand(CE->getOperand(2), LocalMap, GlobalMap);
        Result = ConstantExpr::getSelect(cast<Constant>(V1), cast<Constant>(V2),
                                         cast<Constant>(V3));
      } else if (CE->getNumOperands() == 2) {
        // Binary operator...
        Value *V1 = RemapOperand(CE->getOperand(0), LocalMap, GlobalMap);
        Value *V2 = RemapOperand(CE->getOperand(1), LocalMap, GlobalMap);

        Result = ConstantExpr::get(CE->getOpcode(), cast<Constant>(V1),
                                   cast<Constant>(V2));
      } else {
        assert(0 && "Unknown constant expr type!");
      }

    } else {
      assert(0 && "Unknown type of derived type constant value!");
    }

    // Cache the mapping in our local map structure...
    if (GlobalMap)
      GlobalMap->insert(std::make_pair(In, Result));
    else
      LocalMap.insert(std::make_pair(In, Result));
    return Result;
  }

  std::cerr << "XXX LocalMap: \n";
  PrintMap(LocalMap);

  if (GlobalMap) {
    std::cerr << "XXX GlobalMap: \n";
    PrintMap(*GlobalMap);
  }

  std::cerr << "Couldn't remap value: " << (void*)In << " " << *In << "\n";
  assert(0 && "Couldn't remap value!");
  return 0;
}

/// ForceRenaming - The LLVM SymbolTable class autorenames globals that conflict
/// in the symbol table.  This is good for all clients except for us.  Go
/// through the trouble to force this back.
static void ForceRenaming(GlobalValue *GV, const std::string &Name) {
  assert(GV->getName() != Name && "Can't force rename to self");
  SymbolTable &ST = GV->getParent()->getSymbolTable();

  // If there is a conflict, rename the conflict.
  Value *ConflictVal = ST.lookup(GV->getType(), Name);
  assert(ConflictVal&&"Why do we have to force rename if there is no conflic?");
  GlobalValue *ConflictGV = cast<GlobalValue>(ConflictVal);
  assert(ConflictGV->hasInternalLinkage() &&
         "Not conflicting with a static global, should link instead!");

  ConflictGV->setName("");          // Eliminate the conflict
  GV->setName(Name);                // Force the name back
  ConflictGV->setName(Name);        // This will cause ConflictGV to get renamed
  assert(GV->getName() == Name && ConflictGV->getName() != Name &&
         "ForceRenaming didn't work");
}


// LinkGlobals - Loop through the global variables in the src module and merge
// them into the dest module.
//
static bool LinkGlobals(Module *Dest, const Module *Src,
                        std::map<const Value*, Value*> &ValueMap,
                    std::multimap<std::string, GlobalVariable *> &AppendingVars,
                        std::map<std::string, GlobalValue*> &GlobalsByName,
                        std::string *Err) {
  // We will need a module level symbol table if the src module has a module
  // level symbol table...
  SymbolTable *ST = (SymbolTable*)&Dest->getSymbolTable();
  
  // Loop over all of the globals in the src module, mapping them over as we go
  //
  for (Module::const_giterator I = Src->gbegin(), E = Src->gend(); I != E; ++I){
    const GlobalVariable *SGV = I;
    GlobalVariable *DGV = 0;
    // Check to see if may have to link the global.
    if (SGV->hasName() && !SGV->hasInternalLinkage())
      if (!(DGV = Dest->getGlobalVariable(SGV->getName(),
                                          SGV->getType()->getElementType()))) {
        std::map<std::string, GlobalValue*>::iterator EGV =
          GlobalsByName.find(SGV->getName());
        if (EGV != GlobalsByName.end())
          DGV = dyn_cast<GlobalVariable>(EGV->second);
        if (DGV && RecursiveResolveTypes(SGV->getType(), DGV->getType(), ST, ""))
          DGV = 0;  // FIXME: gross.
      }

    assert(SGV->hasInitializer() || SGV->hasExternalLinkage() &&
           "Global must either be external or have an initializer!");

    bool SGExtern = SGV->isExternal();
    bool DGExtern = DGV ? DGV->isExternal() : false;

    if (!DGV || DGV->hasInternalLinkage() || SGV->hasInternalLinkage()) {
      // No linking to be performed, simply create an identical version of the
      // symbol over in the dest module... the initializer will be filled in
      // later by LinkGlobalInits...
      //
      GlobalVariable *NewDGV =
        new GlobalVariable(SGV->getType()->getElementType(),
                           SGV->isConstant(), SGV->getLinkage(), /*init*/0,
                           SGV->getName(), Dest);

      // If the LLVM runtime renamed the global, but it is an externally visible
      // symbol, DGV must be an existing global with internal linkage.  Rename
      // it.
      if (NewDGV->getName() != SGV->getName() && !NewDGV->hasInternalLinkage())
        ForceRenaming(NewDGV, SGV->getName());

      // Make sure to remember this mapping...
      ValueMap.insert(std::make_pair(SGV, NewDGV));
      if (SGV->hasAppendingLinkage())
        // Keep track that this is an appending variable...
        AppendingVars.insert(std::make_pair(SGV->getName(), NewDGV));

    } else if (SGV->isExternal()) {
      // If SGV is external or if both SGV & DGV are external..  Just link the
      // external globals, we aren't adding anything.
      ValueMap.insert(std::make_pair(SGV, DGV));

      // Inherit 'const' information.
      if (SGV->isConstant()) DGV->setConstant(true);

    } else if (DGV->isExternal()) {   // If DGV is external but SGV is not...
      ValueMap.insert(std::make_pair(SGV, DGV));
      DGV->setLinkage(SGV->getLinkage());    // Inherit linkage!

      if (DGV->isConstant() && !SGV->isConstant())
        return Error(Err, "Linking globals named '" + SGV->getName() +
                     "': declaration is const but definition is not!");

      // Inherit 'const' information.
      if (SGV->isConstant()) DGV->setConstant(true);

    } else if (SGV->hasWeakLinkage() || SGV->hasLinkOnceLinkage()) {
      // At this point we know that DGV has LinkOnce, Appending, Weak, or
      // External linkage.  If DGV is Appending, this is an error.
      if (DGV->hasAppendingLinkage())
        return Error(Err, "Linking globals named '" + SGV->getName() +
                     "' with 'weak' and 'appending' linkage is not allowed!");

      if (SGV->isConstant() != DGV->isConstant())
        return Error(Err, "Global Variable Collision on '" + 
                     ToStr(SGV->getType(), Src) + " %" + SGV->getName() +
                     "' - Global variables differ in const'ness");

      // Otherwise, just perform the link.
      ValueMap.insert(std::make_pair(SGV, DGV));

      // Linkonce+Weak = Weak
      if (DGV->hasLinkOnceLinkage() && SGV->hasWeakLinkage())
        DGV->setLinkage(SGV->getLinkage());

    } else if (DGV->hasWeakLinkage() || DGV->hasLinkOnceLinkage()) {
      // At this point we know that SGV has LinkOnce, Appending, or External
      // linkage.  If SGV is Appending, this is an error.
      if (SGV->hasAppendingLinkage())
        return Error(Err, "Linking globals named '" + SGV->getName() +
                     " ' with 'weak' and 'appending' linkage is not allowed!");

      if (SGV->isConstant() != DGV->isConstant())
        return Error(Err, "Global Variable Collision on '" + 
                     ToStr(SGV->getType(), Src) + " %" + SGV->getName() +
                     "' - Global variables differ in const'ness");

      if (!SGV->hasLinkOnceLinkage())
        DGV->setLinkage(SGV->getLinkage());    // Inherit linkage!
      ValueMap.insert(std::make_pair(SGV, DGV));
  
    } else if (SGV->getLinkage() != DGV->getLinkage()) {
      return Error(Err, "Global variables named '" + SGV->getName() +
                   "' have different linkage specifiers!");
      // Inherit 'const' information.
      if (SGV->isConstant()) DGV->setConstant(true);

    } else if (SGV->hasExternalLinkage()) {
      // Allow linking two exactly identical external global variables...
      if (SGV->isConstant() != DGV->isConstant())
        return Error(Err, "Global Variable Collision on '" + 
                     ToStr(SGV->getType(), Src) + " %" + SGV->getName() +
                     "' - Global variables differ in const'ness");

      if (SGV->getInitializer() != DGV->getInitializer())
        return Error(Err, "Global Variable Collision on '" + 
                     ToStr(SGV->getType(), Src) + " %" + SGV->getName() +
                    "' - External linkage globals have different initializers");

      ValueMap.insert(std::make_pair(SGV, DGV));
    } else if (SGV->hasAppendingLinkage()) {
      // No linking is performed yet.  Just insert a new copy of the global, and
      // keep track of the fact that it is an appending variable in the
      // AppendingVars map.  The name is cleared out so that no linkage is
      // performed.
      GlobalVariable *NewDGV =
        new GlobalVariable(SGV->getType()->getElementType(),
                           SGV->isConstant(), SGV->getLinkage(), /*init*/0,
                           "", Dest);

      // Make sure to remember this mapping...
      ValueMap.insert(std::make_pair(SGV, NewDGV));

      // Keep track that this is an appending variable...
      AppendingVars.insert(std::make_pair(SGV->getName(), NewDGV));
    } else {
      assert(0 && "Unknown linkage!");
    }
  }
  return false;
}


// LinkGlobalInits - Update the initializers in the Dest module now that all
// globals that may be referenced are in Dest.
//
static bool LinkGlobalInits(Module *Dest, const Module *Src,
                            std::map<const Value*, Value*> &ValueMap,
                            std::string *Err) {

  // Loop over all of the globals in the src module, mapping them over as we go
  //
  for (Module::const_giterator I = Src->gbegin(), E = Src->gend(); I != E; ++I){
    const GlobalVariable *SGV = I;

    if (SGV->hasInitializer()) {      // Only process initialized GV's
      // Figure out what the initializer looks like in the dest module...
      Constant *SInit =
        cast<Constant>(RemapOperand(SGV->getInitializer(), ValueMap, 0));

      GlobalVariable *DGV = cast<GlobalVariable>(ValueMap[SGV]);    
      if (DGV->hasInitializer()) {
        if (SGV->hasExternalLinkage()) {
          if (DGV->getInitializer() != SInit)
            return Error(Err, "Global Variable Collision on '" + 
                         ToStr(SGV->getType(), Src) +"':%"+SGV->getName()+
                         " - Global variables have different initializers");
        } else if (DGV->hasLinkOnceLinkage() || DGV->hasWeakLinkage()) {
          // Nothing is required, mapped values will take the new global
          // automatically.
        } else if (SGV->hasLinkOnceLinkage() || SGV->hasWeakLinkage()) {
          // Nothing is required, mapped values will take the new global
          // automatically.
        } else if (DGV->hasAppendingLinkage()) {
          assert(0 && "Appending linkage unimplemented!");
        } else {
          assert(0 && "Unknown linkage!");
        }
      } else {
        // Copy the initializer over now...
        DGV->setInitializer(SInit);
      }
    }
  }
  return false;
}

// LinkFunctionProtos - Link the functions together between the two modules,
// without doing function bodies... this just adds external function prototypes
// to the Dest function...
//
static bool LinkFunctionProtos(Module *Dest, const Module *Src,
                               std::map<const Value*, Value*> &ValueMap,
                             std::map<std::string, GlobalValue*> &GlobalsByName,
                               std::string *Err) {
  SymbolTable *ST = (SymbolTable*)&Dest->getSymbolTable();
  
  // Loop over all of the functions in the src module, mapping them over as we
  // go
  //
  for (Module::const_iterator I = Src->begin(), E = Src->end(); I != E; ++I) {
    const Function *SF = I;   // SrcFunction
    Function *DF = 0;
    if (SF->hasName() && !SF->hasInternalLinkage()) {
      // Check to see if may have to link the function.
      if (!(DF = Dest->getFunction(SF->getName(), SF->getFunctionType()))) {
        std::map<std::string, GlobalValue*>::iterator EF =
          GlobalsByName.find(SF->getName());
        if (EF != GlobalsByName.end())
          DF = dyn_cast<Function>(EF->second);
        if (DF && RecursiveResolveTypes(SF->getType(), DF->getType(), ST, ""))
          DF = 0;  // FIXME: gross.
      }
    }

    if (!DF || SF->hasInternalLinkage() || DF->hasInternalLinkage()) {
      // Function does not already exist, simply insert an function signature
      // identical to SF into the dest module...
      Function *NewDF = new Function(SF->getFunctionType(), SF->getLinkage(),
                                     SF->getName(), Dest);

      // If the LLVM runtime renamed the function, but it is an externally
      // visible symbol, DF must be an existing function with internal linkage.
      // Rename it.
      if (NewDF->getName() != SF->getName() && !NewDF->hasInternalLinkage())
        ForceRenaming(NewDF, SF->getName());

      // ... and remember this mapping...
      ValueMap.insert(std::make_pair(SF, NewDF));
    } else if (SF->isExternal()) {
      // If SF is external or if both SF & DF are external..  Just link the
      // external functions, we aren't adding anything.
      ValueMap.insert(std::make_pair(SF, DF));
    } else if (DF->isExternal()) {   // If DF is external but SF is not...
      // Link the external functions, update linkage qualifiers
      ValueMap.insert(std::make_pair(SF, DF));
      DF->setLinkage(SF->getLinkage());

    } else if (SF->hasWeakLinkage() || SF->hasLinkOnceLinkage()) {
      // At this point we know that DF has LinkOnce, Weak, or External linkage.
      ValueMap.insert(std::make_pair(SF, DF));

      // Linkonce+Weak = Weak
      if (DF->hasLinkOnceLinkage() && SF->hasWeakLinkage())
        DF->setLinkage(SF->getLinkage());

    } else if (DF->hasWeakLinkage() || DF->hasLinkOnceLinkage()) {
      // At this point we know that SF has LinkOnce or External linkage.
      ValueMap.insert(std::make_pair(SF, DF));
      if (!SF->hasLinkOnceLinkage())   // Don't inherit linkonce linkage
        DF->setLinkage(SF->getLinkage());

    } else if (SF->getLinkage() != DF->getLinkage()) {
      return Error(Err, "Functions named '" + SF->getName() +
                   "' have different linkage specifiers!");
    } else if (SF->hasExternalLinkage()) {
      // The function is defined in both modules!!
      return Error(Err, "Function '" + 
                   ToStr(SF->getFunctionType(), Src) + "':\"" + 
                   SF->getName() + "\" - Function is already defined!");
    } else {
      assert(0 && "Unknown linkage configuration found!");
    }
  }
  return false;
}

// LinkFunctionBody - Copy the source function over into the dest function and
// fix up references to values.  At this point we know that Dest is an external
// function, and that Src is not.
//
static bool LinkFunctionBody(Function *Dest, const Function *Src,
                             std::map<const Value*, Value*> &GlobalMap,
                             std::string *Err) {
  assert(Src && Dest && Dest->isExternal() && !Src->isExternal());
  std::map<const Value*, Value*> LocalMap;   // Map for function local values

  // Go through and convert function arguments over...
  Function::aiterator DI = Dest->abegin();
  for (Function::const_aiterator I = Src->abegin(), E = Src->aend();
       I != E; ++I, ++DI) {
    DI->setName(I->getName());  // Copy the name information over...

    // Add a mapping to our local map
    LocalMap.insert(std::make_pair(I, DI));
  }

  // Loop over all of the basic blocks, copying the instructions over...
  //
  for (Function::const_iterator I = Src->begin(), E = Src->end(); I != E; ++I) {
    // Create new basic block and add to mapping and the Dest function...
    BasicBlock *DBB = new BasicBlock(I->getName(), Dest);
    LocalMap.insert(std::make_pair(I, DBB));

    // Loop over all of the instructions in the src basic block, copying them
    // over.  Note that this is broken in a strict sense because the cloned
    // instructions will still be referencing values in the Src module, not
    // the remapped values.  In our case, however, we will not get caught and 
    // so we can delay patching the values up until later...
    //
    for (BasicBlock::const_iterator II = I->begin(), IE = I->end(); 
         II != IE; ++II) {
      Instruction *DI = II->clone();
      DI->setName(II->getName());
      DBB->getInstList().push_back(DI);
      LocalMap.insert(std::make_pair(II, DI));
    }
  }

  // At this point, all of the instructions and values of the function are now
  // copied over.  The only problem is that they are still referencing values in
  // the Source function as operands.  Loop through all of the operands of the
  // functions and patch them up to point to the local versions...
  //
  for (Function::iterator BB = Dest->begin(), BE = Dest->end(); BB != BE; ++BB)
    for (BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E; ++I)
      for (Instruction::op_iterator OI = I->op_begin(), OE = I->op_end();
           OI != OE; ++OI)
        *OI = RemapOperand(*OI, LocalMap, &GlobalMap);

  return false;
}


// LinkFunctionBodies - Link in the function bodies that are defined in the
// source module into the DestModule.  This consists basically of copying the
// function over and fixing up references to values.
//
static bool LinkFunctionBodies(Module *Dest, const Module *Src,
                               std::map<const Value*, Value*> &ValueMap,
                               std::string *Err) {

  // Loop over all of the functions in the src module, mapping them over as we
  // go
  //
  for (Module::const_iterator SF = Src->begin(), E = Src->end(); SF != E; ++SF){
    if (!SF->isExternal()) {                  // No body if function is external
      Function *DF = cast<Function>(ValueMap[SF]); // Destination function

      // DF not external SF external?
      if (DF->isExternal()) {
        // Only provide the function body if there isn't one already.
        if (LinkFunctionBody(DF, SF, ValueMap, Err))
          return true;
      }
    }
  }
  return false;
}

// LinkAppendingVars - If there were any appending global variables, link them
// together now.  Return true on error.
//
static bool LinkAppendingVars(Module *M,
                  std::multimap<std::string, GlobalVariable *> &AppendingVars,
                              std::string *ErrorMsg) {
  if (AppendingVars.empty()) return false; // Nothing to do.
  
  // Loop over the multimap of appending vars, processing any variables with the
  // same name, forming a new appending global variable with both of the
  // initializers merged together, then rewrite references to the old variables
  // and delete them.
  //
  std::vector<Constant*> Inits;
  while (AppendingVars.size() > 1) {
    // Get the first two elements in the map...
    std::multimap<std::string,
      GlobalVariable*>::iterator Second = AppendingVars.begin(), First=Second++;

    // If the first two elements are for different names, there is no pair...
    // Otherwise there is a pair, so link them together...
    if (First->first == Second->first) {
      GlobalVariable *G1 = First->second, *G2 = Second->second;
      const ArrayType *T1 = cast<ArrayType>(G1->getType()->getElementType());
      const ArrayType *T2 = cast<ArrayType>(G2->getType()->getElementType());
      
      // Check to see that they two arrays agree on type...
      if (T1->getElementType() != T2->getElementType())
        return Error(ErrorMsg,
         "Appending variables with different element types need to be linked!");
      if (G1->isConstant() != G2->isConstant())
        return Error(ErrorMsg,
                     "Appending variables linked with different const'ness!");

      unsigned NewSize = T1->getNumElements() + T2->getNumElements();
      ArrayType *NewType = ArrayType::get(T1->getElementType(), NewSize);

      // Create the new global variable...
      GlobalVariable *NG =
        new GlobalVariable(NewType, G1->isConstant(), G1->getLinkage(),
                           /*init*/0, First->first, M);

      // Merge the initializer...
      Inits.reserve(NewSize);
      if (ConstantArray *I = dyn_cast<ConstantArray>(G1->getInitializer())) {
        for (unsigned i = 0, e = T1->getNumElements(); i != e; ++i)
          Inits.push_back(I->getOperand(i));
      } else {
        assert(isa<ConstantAggregateZero>(G1->getInitializer()));
        Constant *CV = Constant::getNullValue(T1->getElementType());
        for (unsigned i = 0, e = T1->getNumElements(); i != e; ++i)
          Inits.push_back(CV);
      }
      if (ConstantArray *I = dyn_cast<ConstantArray>(G2->getInitializer())) {
        for (unsigned i = 0, e = T2->getNumElements(); i != e; ++i)
          Inits.push_back(I->getOperand(i));
      } else {
        assert(isa<ConstantAggregateZero>(G2->getInitializer()));
        Constant *CV = Constant::getNullValue(T2->getElementType());
        for (unsigned i = 0, e = T2->getNumElements(); i != e; ++i)
          Inits.push_back(CV);
      }
      NG->setInitializer(ConstantArray::get(NewType, Inits));
      Inits.clear();

      // Replace any uses of the two global variables with uses of the new
      // global...

      // FIXME: This should rewrite simple/straight-forward uses such as
      // getelementptr instructions to not use the Cast!
      G1->replaceAllUsesWith(ConstantExpr::getCast(NG, G1->getType()));
      G2->replaceAllUsesWith(ConstantExpr::getCast(NG, G2->getType()));

      // Remove the two globals from the module now...
      M->getGlobalList().erase(G1);
      M->getGlobalList().erase(G2);

      // Put the new global into the AppendingVars map so that we can handle
      // linking of more than two vars...
      Second->second = NG;
    }
    AppendingVars.erase(First);
  }

  return false;
}


// LinkModules - This function links two modules together, with the resulting
// left module modified to be the composite of the two input modules.  If an
// error occurs, true is returned and ErrorMsg (if not null) is set to indicate
// the problem.  Upon failure, the Dest module could be in a modified state, and
// shouldn't be relied on to be consistent.
//
bool llvm::LinkModules(Module *Dest, const Module *Src, std::string *ErrorMsg) {
  assert(Dest != 0 && "Invalid Destination module");
  assert(Src  != 0 && "Invalid Source Module");

  if (Dest->getEndianness() == Module::AnyEndianness)
    Dest->setEndianness(Src->getEndianness());
  if (Dest->getPointerSize() == Module::AnyPointerSize)
    Dest->setPointerSize(Src->getPointerSize());

  if (Src->getEndianness() != Module::AnyEndianness &&
      Dest->getEndianness() != Src->getEndianness())
    std::cerr << "WARNING: Linking two modules of different endianness!\n";
  if (Src->getPointerSize() != Module::AnyPointerSize &&
      Dest->getPointerSize() != Src->getPointerSize())
    std::cerr << "WARNING: Linking two modules of different pointer size!\n";

  // Update the destination module's dependent libraries list with the libraries 
  // from the source module. There's no opportunity for duplicates here as the
  // Module ensures that duplicate insertions are discarded.
  Module::lib_iterator SI = Src->lib_begin();
  Module::lib_iterator SE = Src->lib_end();
  while ( SI != SE ) {
    Dest->addLibrary(*SI);
    ++SI;
  }

  // LinkTypes - Go through the symbol table of the Src module and see if any
  // types are named in the src module that are not named in the Dst module.
  // Make sure there are no type name conflicts.
  //
  if (LinkTypes(Dest, Src, ErrorMsg)) return true;

  // ValueMap - Mapping of values from what they used to be in Src, to what they
  // are now in Dest.
  //
  std::map<const Value*, Value*> ValueMap;

  // AppendingVars - Keep track of global variables in the destination module
  // with appending linkage.  After the module is linked together, they are
  // appended and the module is rewritten.
  //
  std::multimap<std::string, GlobalVariable *> AppendingVars;

  // GlobalsByName - The LLVM SymbolTable class fights our best efforts at
  // linking by separating globals by type.  Until PR411 is fixed, we replicate
  // it's functionality here.
  std::map<std::string, GlobalValue*> GlobalsByName;

  for (Module::giterator I = Dest->gbegin(), E = Dest->gend(); I != E; ++I) {
    // Add all of the appending globals already in the Dest module to
    // AppendingVars.
    if (I->hasAppendingLinkage())
      AppendingVars.insert(std::make_pair(I->getName(), I));

    // Keep track of all globals by name.
    if (!I->hasInternalLinkage() && I->hasName())
      GlobalsByName[I->getName()] = I;
  }

  // Keep track of all globals by name.
  for (Module::iterator I = Dest->begin(), E = Dest->end(); I != E; ++I)
    if (!I->hasInternalLinkage() && I->hasName())
      GlobalsByName[I->getName()] = I;

  // Insert all of the globals in src into the Dest module... without linking
  // initializers (which could refer to functions not yet mapped over).
  //
  if (LinkGlobals(Dest, Src, ValueMap, AppendingVars, GlobalsByName, ErrorMsg))
    return true;

  // Link the functions together between the two modules, without doing function
  // bodies... this just adds external function prototypes to the Dest
  // function...  We do this so that when we begin processing function bodies,
  // all of the global values that may be referenced are available in our
  // ValueMap.
  //
  if (LinkFunctionProtos(Dest, Src, ValueMap, GlobalsByName, ErrorMsg))
    return true;

  // Update the initializers in the Dest module now that all globals that may
  // be referenced are in Dest.
  //
  if (LinkGlobalInits(Dest, Src, ValueMap, ErrorMsg)) return true;

  // Link in the function bodies that are defined in the source module into the
  // DestModule.  This consists basically of copying the function over and
  // fixing up references to values.
  //
  if (LinkFunctionBodies(Dest, Src, ValueMap, ErrorMsg)) return true;

  // If there were any appending global variables, link them together now.
  //
  if (LinkAppendingVars(Dest, AppendingVars, ErrorMsg)) return true;

  // If the source library's module id is in the dependent library list of the
  // destination library, remove it since that module is now linked in.
  sys::Path modId;
  modId.setFile(Src->getModuleIdentifier());
  if (!modId.isEmpty())
    Dest->removeLibrary(modId.getBasename());

  return false;
}

// vim: sw=2
