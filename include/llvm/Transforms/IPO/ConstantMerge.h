//===- llvm/Transforms/ConstantMerge.h - Merge duplicate consts --*- C++ -*--=//
//
// This file defines the interface to a pass that merges duplicate global
// constants together into a single constant that is shared.  This is useful
// because some passes (ie TraceValues) insert a lot of string constants into
// the program, regardless of whether or not they duplicate an existing string.
//
// Algorithm: ConstantMerge is designed to build up a map of available constants
// and elminate duplicates when it is initialized.
//
// The DynamicConstantMerge method is a superset of the ConstantMerge algorithm
// that checks for each method to see if constants have been added to the
// constant pool since it was last run... if so, it processes them.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_CONSTANTMERGE_H
#define LLVM_TRANSFORMS_CONSTANTMERGE_H

class Pass;
Pass *createConstantMergePass();
Pass *createDynamicConstantMergePass();

#endif
