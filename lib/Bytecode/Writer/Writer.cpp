//===-- Writer.cpp - Library for writing VM bytecode files -------*- C++ -*--=//
//
// This library implements the functionality defined in llvm/Bytecode/Writer.h
//
// Note that this file uses an unusual technique of outputting all the bytecode
// to a deque of unsigned char's, then copies the deque to an ostream.  The
// reason for this is that we must do "seeking" in the stream to do back-
// patching, and some very important ostreams that we want to support (like
// pipes) do not support seeking.  :( :( :(
//
// The choice of the deque data structure is influenced by the extremely fast
// "append" speed, plus the free "seek"/replace in the middle of the stream. I
// didn't use a vector because the stream could end up very large and copying
// the whole thing to reallocate would be kinda silly.
//
// Note that the performance of this library is not terribly important, because
// it shouldn't be used by JIT type applications... so it is not a huge focus
// at least.  :)
//
//===----------------------------------------------------------------------===//

#include "WriterInternals.h"
#include "llvm/Bytecode/WriteBytecodePass.h"
#include "llvm/Module.h"
#include "llvm/SymbolTable.h"
#include "llvm/DerivedTypes.h"
#include "Support/STLExtras.h"
#include "Support/Statistic.h"
#include <string.h>
#include <algorithm>

static RegisterPass<WriteBytecodePass> X("emitbytecode", "Bytecode Writer");

static Statistic<> 
BytesWritten("bytecodewriter", "Number of bytecode bytes written");


BytecodeWriter::BytecodeWriter(std::deque<unsigned char> &o, const Module *M) 
  : Out(o), Table(M, false) {

  outputSignature();

  // Emit the top level CLASS block.
  BytecodeBlock ModuleBlock(BytecodeFormat::Module, Out);

  // Output the ID of first "derived" type:
  output_vbr((unsigned)Type::FirstDerivedTyID, Out);
  align32(Out);

  // Output module level constants, including types used by the function protos
  outputConstants(false);

  // The ModuleInfoBlock follows directly after the Module constant pool
  outputModuleInfoBlock(M);

  // Do the whole module now! Process each function at a time...
  for (Module::const_iterator I = M->begin(), E = M->end(); I != E; ++I)
    processMethod(I);

  // If needed, output the symbol table for the module...
  if (M->hasSymbolTable())
    outputSymbolTable(*M->getSymbolTable());
}

// Helper function for outputConstants().
// Writes out all the constants in the plane Plane starting at entry StartNo.
// 
void BytecodeWriter::outputConstantsInPlane(const std::vector<const Value*>
                                            &Plane, unsigned StartNo) {
  unsigned ValNo = StartNo;
  
  // Scan through and ignore function arguments...
  for (; ValNo < Plane.size() && isa<Argument>(Plane[ValNo]); ValNo++)
    /*empty*/;

  unsigned NC = ValNo;              // Number of constants
  for (; NC < Plane.size() && 
         (isa<Constant>(Plane[NC]) || isa<Type>(Plane[NC])); NC++)
    /*empty*/;
  NC -= ValNo;                      // Convert from index into count
  if (NC == 0) return;              // Skip empty type planes...

  // Output type header: [num entries][type id number]
  //
  output_vbr(NC, Out);

  // Output the Type ID Number...
  int Slot = Table.getValSlot(Plane.front()->getType());
  assert (Slot != -1 && "Type in constant pool but not in function!!");
  output_vbr((unsigned)Slot, Out);

  //cerr << "Emitting " << NC << " constants of type '" 
  //	 << Plane.front()->getType()->getName() << "' = Slot #" << Slot << "\n";

  for (unsigned i = ValNo; i < ValNo+NC; ++i) {
    const Value *V = Plane[i];
    if (const Constant *CPV = dyn_cast<Constant>(V)) {
      //cerr << "Serializing value: <" << V->getType() << ">: " << V << ":" 
      //     << Out.size() << "\n";
      outputConstant(CPV);
    } else {
      outputType(cast<const Type>(V));
    }
  }
}

void BytecodeWriter::outputConstants(bool isFunction) {
  BytecodeBlock CPool(BytecodeFormat::ConstantPool, Out);

  unsigned NumPlanes = Table.getNumPlanes();
  
  // Write the type plane for types first because earlier planes
  // (e.g. for a primitive type like float) may have constants constructed
  // using types coming later (e.g., via getelementptr from a pointer type).
  // The type plane is needed before types can be fwd or bkwd referenced.
  if (!isFunction) {
    const std::vector<const Value*> &Plane = Table.getPlane(Type::TypeTyID);
    assert(!Plane.empty() && "No types at all?");
    unsigned ValNo = Type::FirstDerivedTyID; // Start at the derived types...
    outputConstantsInPlane(Plane, ValNo);      // Write out the types
  }
  
  for (unsigned pno = 0; pno != NumPlanes; pno++) {
    const std::vector<const Value*> &Plane = Table.getPlane(pno);
    if (!Plane.empty()) {             // Skip empty type planes...
      unsigned ValNo = 0;
      if (isFunction)                   // Don't reemit module constants
        ValNo = Table.getModuleLevel(pno);
      else if (pno == Type::TypeTyID) // If type plane wasn't written out above
        continue;

      outputConstantsInPlane(Plane, ValNo); // Write out constants in the plane
    }
  }
}

void BytecodeWriter::outputModuleInfoBlock(const Module *M) {
  BytecodeBlock ModuleInfoBlock(BytecodeFormat::ModuleGlobalInfo, Out);
  
  // Output the types for the global variables in the module...
  for (Module::const_giterator I = M->gbegin(), End = M->gend(); I != End;++I) {
    int Slot = Table.getValSlot(I->getType());
    assert(Slot != -1 && "Module global vars is broken!");

    // Fields: bit0 = isConstant, bit1 = hasInitializer, bit2=InternalLinkage,
    // bit3+ = slot#
    unsigned oSlot = ((unsigned)Slot << 3) | (I->hasInternalLinkage() << 2) |
                     (I->hasInitializer() << 1) | I->isConstant();
    output_vbr(oSlot, Out);

    // If we have an initializer, output it now.
    if (I->hasInitializer()) {
      Slot = Table.getValSlot((Value*)I->getInitializer());
      assert(Slot != -1 && "No slot for global var initializer!");
      output_vbr((unsigned)Slot, Out);
    }
  }
  output_vbr((unsigned)Table.getValSlot(Type::VoidTy), Out);

  // Output the types of the functions in this module...
  for (Module::const_iterator I = M->begin(), End = M->end(); I != End; ++I) {
    int Slot = Table.getValSlot(I->getType());
    assert(Slot != -1 && "Module const pool is broken!");
    assert(Slot >= Type::FirstDerivedTyID && "Derived type not in range!");
    output_vbr((unsigned)Slot, Out);
  }
  output_vbr((unsigned)Table.getValSlot(Type::VoidTy), Out);


  align32(Out);
}

void BytecodeWriter::processMethod(const Function *F) {
  BytecodeBlock FunctionBlock(BytecodeFormat::Function, Out);
  output_vbr((unsigned)F->hasInternalLinkage(), Out);
  // Only output the constant pool and other goodies if needed...
  if (!F->isExternal()) {

    // Get slot information about the function...
    Table.incorporateFunction(F);

    // Output information about the constants in the function...
    outputConstants(true);

    // Output basic block nodes...
    for (Function::const_iterator I = F->begin(), E = F->end(); I != E; ++I)
      processBasicBlock(*I);
    
    // If needed, output the symbol table for the function...
    if (F->hasSymbolTable())
      outputSymbolTable(*F->getSymbolTable());
    
    Table.purgeFunction();
  }
}


void BytecodeWriter::processBasicBlock(const BasicBlock &BB) {
  BytecodeBlock FunctionBlock(BytecodeFormat::BasicBlock, Out);
  // Process all the instructions in the bb...
  for(BasicBlock::const_iterator I = BB.begin(), E = BB.end(); I != E; ++I)
    processInstruction(*I);
}

void BytecodeWriter::outputSymbolTable(const SymbolTable &MST) {
  BytecodeBlock FunctionBlock(BytecodeFormat::SymbolTable, Out);

  for (SymbolTable::const_iterator TI = MST.begin(); TI != MST.end(); ++TI) {
    SymbolTable::type_const_iterator I = MST.type_begin(TI->first);
    SymbolTable::type_const_iterator End = MST.type_end(TI->first);
    int Slot;
    
    if (I == End) continue;  // Don't mess with an absent type...

    // Symtab block header: [num entries][type id number]
    output_vbr(MST.type_size(TI->first), Out);

    Slot = Table.getValSlot(TI->first);
    assert(Slot != -1 && "Type in symtab, but not in table!");
    output_vbr((unsigned)Slot, Out);

    for (; I != End; ++I) {
      // Symtab entry: [def slot #][name]
      Slot = Table.getValSlot(I->second);
      assert(Slot != -1 && "Value in symtab but has no slot number!!");
      output_vbr((unsigned)Slot, Out);
      output(I->first, Out, false); // Don't force alignment...
    }
  }
}

void WriteBytecodeToFile(const Module *C, std::ostream &Out) {
  assert(C && "You can't write a null module!!");

  std::deque<unsigned char> Buffer;

  // This object populates buffer for us...
  BytecodeWriter BCW(Buffer, C);

  // Keep track of how much we've written...
  BytesWritten += Buffer.size();

  // Okay, write the deque out to the ostream now... the deque is not
  // sequential in memory, however, so write out as much as possible in big
  // chunks, until we're done.
  //
  std::deque<unsigned char>::const_iterator I = Buffer.begin(),E = Buffer.end();
  while (I != E) {                           // Loop until it's all written
    // Scan to see how big this chunk is...
    const unsigned char *ChunkPtr = &*I;
    const unsigned char *LastPtr = ChunkPtr;
    while (I != E) {
      const unsigned char *ThisPtr = &*++I;
      if (LastPtr+1 != ThisPtr) {   // Advanced by more than a byte of memory?
        ++LastPtr;
        break;
      }
      LastPtr = ThisPtr;
    }
    
    // Write out the chunk...
    Out.write((char*)ChunkPtr, LastPtr-ChunkPtr);
  }

  Out.flush();
}
