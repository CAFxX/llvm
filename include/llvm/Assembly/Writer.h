//===-- llvm/Assembly/Writer.h - Printer for VM assembly files ---*- C++ -*--=//
//
// This functionality is implemented by the lib/Assembly/Writer library.
// This library is used to print VM assembly language files to an iostream. It
// can print VM code at a variety of granularities, ranging from a whole class
// down to an individual instruction.  This makes it useful for debugging.
//
// This file also defines functions that allow it to output files that a program
// called VCG can read.
//
// This library uses the Analysis library to figure out offsets for
// variables in the method tables...
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ASSEMBLY_WRITER_H
#define LLVM_ASSEMBLY_WRITER_H

#include <iosfwd>
class Type;
class Module;
class Value;
class SlotCalculator;


// WriteTypeSymbolic - This attempts to write the specified type as a symbolic
// type, iff there is an entry in the modules symbol table for the specified
// type or one of it's component types.  This is slower than a simple x << Type;
//
std::ostream &WriteTypeSymbolic(std::ostream &, const Type *, const Module *M);


// WriteAsOperand - Write the name of the specified value out to the specified
// ostream.  This can be useful when you just want to print int %reg126, not the
// whole instruction that generated it.
//
std::ostream &WriteAsOperand(std::ostream &, const Value *, bool PrintTy = true,
                             bool PrintName = true, SlotCalculator *Table = 0);

#endif
