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

template<class ValueSubclass, class ItemParentType, class SymTabType>
void ValueHolder<ValueSubclass,ItemParentType,SymTabType>
::setParent(SymTabType *P) { 
  if (Parent) {     // Remove all of the items from the old symbol table..
    SymbolTable *SymTab = Parent->getSymbolTable();
    for (iterator I = begin(); I != end(); ++I)
      if ((*I)->hasName()) SymTab->remove(*I);
  }

  Parent = P; 

  if (Parent) {     // Remove all of the items from the old symbol table..
    SymbolTable *SymTab = Parent->getSymbolTableSure();
    for (iterator I = begin(); I != end(); ++I)
      if ((*I)->hasName()) SymTab->insert(*I);
  }
}


template<class ValueSubclass, class ItemParentType, class SymTabType>
void ValueHolder<ValueSubclass,ItemParentType,SymTabType>
::remove(ValueSubclass *D) {
  iterator I(find(begin(), end(), D));
  assert(I != end() && "Value not in ValueHolder!!");
  remove(I);
}

// ValueHolder::remove(iterator &) this removes the element at the location
// specified by the iterator, and leaves the iterator pointing to the element
// that used to follow the element deleted.
//
template<class ValueSubclass, class ItemParentType, class SymTabType>
ValueSubclass *ValueHolder<ValueSubclass,ItemParentType,SymTabType>
::remove(iterator &DI) {
  assert(DI != ValueList.end() && 
         "Trying to remove the end of the value list!!!");
  
  ValueSubclass *i = *DI;
  DI = ValueList.erase(DI);

  i->setParent(0);  // I don't own you anymore... byebye...
  
  // You don't get to be in the symbol table anymore... byebye
  if (i->hasName() && Parent)
    Parent->getSymbolTable()->remove(i);
  
  return i;
}

template<class ValueSubclass, class ItemParentType, class SymTabType>
ValueSubclass *ValueHolder<ValueSubclass,ItemParentType,SymTabType>
::pop_back() {
  assert(!ValueList.empty() && "Can't pop_back an empty valuelist!");
  ValueSubclass *i = ValueList.back();
  ValueList.pop_back();
  i->setParent(0);  // I don't own you anymore... byebye...
  
  // You don't get to be in the symbol table anymore... byebye
  if (i->hasName() && Parent)
    Parent->getSymbolTable()->remove(i);
  
  return i;
}


template<class ValueSubclass, class ItemParentType, class SymTabType>
ValueSubclass *ValueHolder<ValueSubclass,ItemParentType,SymTabType>
::remove(const iterator &DI) {
  assert(DI != ValueList.end() && 
         "Trying to remove the end of the def list!!!");
  
  ValueSubclass *i = *DI;
  ValueList.erase(DI);

  i->setParent(0);  // I don't own you anymore... byebye...
  
  // You don't get to be in the symbol table anymore... byebye
  if (i->hasName() && Parent)
    Parent->getSymbolTable()->remove(i);
  
  return i;
}

template<class ValueSubclass, class ItemParentType, class SymTabType>
void ValueHolder<ValueSubclass,ItemParentType,SymTabType>
::push_front(ValueSubclass *Inst) {
  assert(Inst->getParent() == 0 && "Value already has parent!");
  Inst->setParent(ItemParent);

  //ValueList.push_front(Inst);
  ValueList.insert(ValueList.begin(), Inst);
 
  if (Inst->hasName() && Parent)
    Parent->getSymbolTableSure()->insert(Inst);
}

template<class ValueSubclass, class ItemParentType, class SymTabType>
void ValueHolder<ValueSubclass,ItemParentType,SymTabType>
::push_back(ValueSubclass *Inst) {
  assert(Inst->getParent() == 0 && "Value already has parent!");
  Inst->setParent(ItemParent);

  ValueList.push_back(Inst);
  
  if (Inst->hasName() && Parent)
    Parent->getSymbolTableSure()->insert(Inst);
}

// ValueHolder::insert - This method inserts the specified value *BEFORE* the 
// indicated iterator position, and returns an interator to the newly inserted
// value.
//
template<class ValueSubclass, class ItemParentType, class SymTabType>
ValueHolder<ValueSubclass,ItemParentType,SymTabType>::iterator
ValueHolder<ValueSubclass,ItemParentType,SymTabType>
::insert(iterator Pos, ValueSubclass *Inst) {
  assert(Inst->getParent() == 0 && "Value already has parent!");
  Inst->setParent(ItemParent);

  iterator I = ValueList.insert(Pos, Inst);
  if (Inst->hasName() && Parent)
    Parent->getSymbolTableSure()->insert(Inst);
  return I;
}

// ValueHolder::insert - This method inserts the specified _range_ of values
// before the 'Pos' iterator, and returns an iterator to the first newly
// inserted element.  This currently only works for vector iterators...
//
// FIXME: This is not generic so that the code does not have to be around
// to be used... is this ok?
//
template<class ValueSubclass, class ItemParentType, class SymTabType>
void ValueHolder<ValueSubclass,ItemParentType,SymTabType>
::insert(iterator Pos,                     // Where to insert
         iterator First, iterator Last) {   // Vector to read insts from

  // Check to make sure that the values are not already in some valueholder...
  
  for (iterator X = First; X != Last; ++X) {
    assert((*X)->getParent() == 0 &&
           "Cannot insert into valueholder, value already has a parent!");
    (*X)->setParent(ItemParent);
  }

  // Add all of the values to the value holder...
  ValueList.insert(Pos, First, Last);

  // Insert all of the instructions in the symbol table...
  if (Parent)
    for (;First != Last; ++First)
      if ((*First)->hasName())
        Parent->getSymbolTableSure()->insert(*First);
}

#endif
