//===-- Value.cpp - Implement the Value class -----------------------------===//
//
// This file implements the Value class. 
//
//===----------------------------------------------------------------------===//

#include "llvm/ValueHolderImpl.h"
#include "llvm/InstrTypes.h"
#include "llvm/SymbolTable.h"
#include "llvm/SymTabValue.h"
#include "llvm/ConstantPool.h"
#include "llvm/ConstPoolVals.h"
#include "llvm/Type.h"
#ifndef NDEBUG      // Only in -g mode...
#include "llvm/Assembly/Writer.h"
#endif
#include <algorithm>

//===----------------------------------------------------------------------===//
//                                Value Class
//===----------------------------------------------------------------------===//

Value::Value(const Type *ty, ValueTy vty, const string &name = "") : Name(name){
  Ty = ty;
  VTy = vty;
}

Value::~Value() {
#ifndef NDEBUG      // Only in -g mode...
  if (Uses.begin() != Uses.end()) {
    for (use_const_iterator I = Uses.begin(); I != Uses.end(); I++)
      cerr << "Use still stuck around after Def is destroyed:" << *I << endl;
  }
#endif
  assert(Uses.begin() == Uses.end());
}

void Value::replaceAllUsesWith(Value *D) {
  assert(D && "Value::replaceAllUsesWith(<null>) is invalid!");
  while (!Uses.empty()) {
    User *Use = Uses.front();
#ifndef NDEBUG
    unsigned NumUses = Uses.size();
#endif
    Use->replaceUsesOfWith(this, D);

#ifndef NDEBUG      // only in -g mode...
    if (Uses.size() == NumUses)
      cerr << "Use: " << Use << "replace with: " << D; 
#endif
    assert(Uses.size() != NumUses && "Didn't remove definition!");
  }
}

void Value::killUse(User *i) {
  if (i == 0) return;
  use_iterator I = find(Uses.begin(), Uses.end(), i);

  assert(I != Uses.end() && "Use not in uses list!!");
  Uses.erase(I);
}

User *Value::use_remove(use_iterator &I) {
  assert(I != Uses.end() && "Trying to remove the end of the use list!!!");
  User *i = *I;
  I = Uses.erase(I);
  return i;
}


//===----------------------------------------------------------------------===//
//                                 User Class
//===----------------------------------------------------------------------===//

User::User(const Type *Ty, ValueTy vty, const string &name) 
  : Value(Ty, vty, name) {
}

// replaceUsesOfWith - Replaces all references to the "From" definition with
// references to the "To" definition.
//
void User::replaceUsesOfWith(Value *From, Value *To) {
  if (From == To) return;   // Duh what?

  for (unsigned OpNum = 0; Value *D = getOperand(OpNum); OpNum++) {   
    if (D == From) {  // Okay, this operand is pointing to our fake def.
      // The side effects of this setOperand call include linking to
      // "To", adding "this" to the uses list of To, and
      // most importantly, removing "this" from the use list of "From".
      setOperand(OpNum, To); // Fix it now...
    }
  }
}


//===----------------------------------------------------------------------===//
//                             SymTabValue Class
//===----------------------------------------------------------------------===//

// Instantiate Templates - This ugliness is the price we have to pay
// for having a ValueHolderImpl.h file seperate from ValueHolder.h!  :(
//
template class ValueHolder<ConstPoolVal, SymTabValue>;

SymTabValue::SymTabValue(const Type *Ty, ValueTy dty, const string &name = "") 
  : Value(Ty, dty, name), ConstPool(this) { 
  ParentSymTab = SymTab = 0;
}


SymTabValue::~SymTabValue() {
  ConstPool.dropAllReferences();
  ConstPool.delete_all();
  ConstPool.setParent(0);

  delete SymTab;
}

void SymTabValue::setParentSymTab(SymbolTable *ST) {
  ParentSymTab = ST;
  if (SymTab) 
    SymTab->setParentSymTab(ST);
}

SymbolTable *SymTabValue::getSymbolTableSure() {
  if (!SymTab) SymTab = new SymbolTable(ParentSymTab);
  return SymTab;
}

// hasSymbolTable() - Returns true if there is a symbol table allocated to
// this object AND if there is at least one name in it!
//
bool SymTabValue::hasSymbolTable() const {
  if (!SymTab) return false;

  for (SymbolTable::const_iterator I = SymTab->begin(); 
       I != SymTab->end(); I++) {
    if (I->second.begin() != I->second.end())
      return true;                                // Found nonempty type plane!
  }
  
  return false;
}
