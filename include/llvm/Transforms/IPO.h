//===- llvm/Transforms/IPO.h - Interprocedural Transformations --*- C++ -*-===//
//
// This header file defines prototypes for accessor functions that expose passes
// in the IPO transformations library.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_IPO_H
#define LLVM_TRANSFORMS_IPO_H

class Pass;

//===----------------------------------------------------------------------===//
// createConstantMergePass - This function returns a new pass that merges
// duplicate global constants together into a single constant that is shared.
// This is useful because some passes (ie TraceValues) insert a lot of string
// constants into the program, regardless of whether or not they duplicate an
// existing string.
//
Pass *createConstantMergePass();


//===----------------------------------------------------------------------===//
// createDeadTypeEliminationPass - Return a new pass that eliminates symbol
// table entries for types that are never used.
//
Pass *createDeadTypeEliminationPass();


//===----------------------------------------------------------------------===//
// createGlobalDCEPass - This transform is designed to eliminate unreachable
// internal globals (functions or global variables)
//
Pass *createGlobalDCEPass();


//===----------------------------------------------------------------------===//
// FunctionResolvingPass - Go over the functions that are in the module and
// look for functions that have the same name.  More often than not, there will
// be things like:
//    void "foo"(...)
//    void "foo"(int, int)
// because of the way things are declared in C.  If this is the case, patch
// things up.
//
// This is an interprocedural pass.
//
Pass *createFunctionResolvingPass();


//===----------------------------------------------------------------------===//
// createInternalizePass - This pass loops over all of the functions in the
// input module, looking for a main function.  If a main function is found, all
// other functions are marked as internal.
//
Pass *createInternalizePass();


//===----------------------------------------------------------------------===//
// createPoolAllocatePass - This transform changes programs so that disjoint
// data structures are allocated out of different pools of memory, increasing
// locality and shrinking pointer size.
//
Pass *createPoolAllocatePass();


//===----------------------------------------------------------------------===//
// These passes are wrappers that can do a few simple structure mutation
// transformations.
//
Pass *createSwapElementsPass();
Pass *createSortElementsPass();

#endif
