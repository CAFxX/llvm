//===-- llvm/ValueHolderImpl.h - Implement ValueHolder template --*- C++ -*--=//
//
// This file implements the ValueHolder class.  This is kept out of line because
// it tends to pull in a lot of dependencies on other headers and most files
// don't need all that crud.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_VALUEHOLDER_IMPL_H
#define LLVM_VALUEHOLDER_IMPL_H

#include "llvm/ValueHolder.h"
#include "llvm/SymbolTable.h"
#include <algorithm>

template<class ValueSubclass, class ItemParentType>
void ValueHolder<ValueSubclass,ItemParentType>::setParent(SymTabValue *P) { 
  if (Parent) {     // Remove all of the items from the old symbol table..
    SymbolTable *SymTab = Parent->getSymbolTable();
    for (iterator I = begin(); I != end(); I++)
      if ((*I)->hasName()) SymTab->remove(*I);
  }

  Parent = P; 

  if (Parent) {     // Remove all of the items from the old symbol table..
    SymbolTable *SymTab = Parent->getSymbolTableSure();
    for (iterator I = begin(); I != end(); I++)
      if ((*I)->hasName()) SymTab->insert(*I);
  }
}


template<class ValueSubclass, class ItemParentType>
void ValueHolder<ValueSubclass,ItemParentType>::remove(ValueSubclass *D) {
  iterator I(find(begin(), end(), D));
  assert(I != end() && "Value not in ValueHolder!!");
  remove(I);
}

// ValueHolder::remove(iterator &) this removes the element at the location specified
// by the iterator, and leaves the iterator pointing to the element that used to follow
// the element deleted.
//
template<class ValueSubclass, class ItemParentType>
ValueSubclass *ValueHolder<ValueSubclass,ItemParentType>::remove(iterator &DI) {
  assert(DI != ValueList.end() && 
         "Trying to remove the end of the def list!!!");
  
  ValueSubclass *i = *DI;
  DI = ValueList.erase(DI);

  i->setParent(0);  // I don't own you anymore... byebye...
  
  // You don't get to be in the symbol table anymore... byebye
  if (i->hasName() && Parent)
    Parent->getSymbolTable()->remove(i);
  
  return i;
}

template<class ValueSubclass, class ItemParentType>
void ValueHolder<ValueSubclass,ItemParentType>::push_front(ValueSubclass *Inst) {
  assert(Inst->getParent() == 0 && "Value already has parent!");
  Inst->setParent(ItemParent);

  //ValueList.push_front(Inst);
  ValueList.insert(ValueList.begin(), Inst);
 
  if (Inst->hasName() && Parent)
    Parent->getSymbolTableSure()->insert(Inst);
}

template<class ValueSubclass, class ItemParentType>
void ValueHolder<ValueSubclass,ItemParentType>::push_back(ValueSubclass *Inst) {
  assert(Inst->getParent() == 0 && "Value already has parent!");
  Inst->setParent(ItemParent);

  ValueList.push_back(Inst);
  
  if (Inst->hasName() && Parent)
    Parent->getSymbolTableSure()->insert(Inst);
}

#endif
