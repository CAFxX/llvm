//===-- Writer.cpp - Library for Printing VM assembly files ------*- C++ -*--=//
//
// This library implements the functionality defined in llvm/Assembly/Writer.h
//
// This library uses the Analysis library to figure out offsets for
// variables in the method tables...
//
// TODO: print out the type name instead of the full type if a particular type
//       is in the symbol table...
//
//===----------------------------------------------------------------------===//

#include "llvm/Assembly/Writer.h"
#include "llvm/Analysis/SlotCalculator.h"
#include "llvm/Module.h"
#include "llvm/Method.h"
#include "llvm/BasicBlock.h"
#include "llvm/ConstPoolVals.h"
#include "llvm/iOther.h"
#include "llvm/iMemory.h"

class AssemblyWriter : public ModuleAnalyzer {
  ostream &Out;
  SlotCalculator &Table;
public:
  inline AssemblyWriter(ostream &o, SlotCalculator &Tab) : Out(o), Table(Tab) {
  }

  inline void write(const Module *M)         { processModule(M);      }
  inline void write(const Method *M)         { processMethod(M);      }
  inline void write(const BasicBlock *BB)    { processBasicBlock(BB); }
  inline void write(const Instruction *I)    { processInstruction(I); }
  inline void write(const ConstPoolVal *CPV) { processConstant(CPV);  }

protected:
  virtual bool visitMethod(const Method *M);
  virtual bool processConstPool(const ConstantPool &CP, bool isMethod);
  virtual bool processConstant(const ConstPoolVal *CPV);
  virtual bool processMethod(const Method *M);
  virtual bool processMethodArgument(const MethodArgument *MA);
  virtual bool processBasicBlock(const BasicBlock *BB);
  virtual bool processInstruction(const Instruction *I);

private :
  void writeOperand(const Value *Op, bool PrintType, bool PrintName = true);
};



// visitMethod - This member is called after the above two steps, visting each
// method, because they are effectively values that go into the constant pool.
//
bool AssemblyWriter::visitMethod(const Method *M) {
  return false;
}

bool AssemblyWriter::processConstPool(const ConstantPool &CP, bool isMethod) {
  // Done printing arguments...
  if (isMethod) Out << ")\n";

  ModuleAnalyzer::processConstPool(CP, isMethod);
  
  if (isMethod)
    Out << "begin";
  else
    Out << "implementation\n";
  return false;
}


// processConstant - Print out a constant pool entry...
//
bool AssemblyWriter::processConstant(const ConstPoolVal *CPV) {
  Out << "\t";

  // Print out name if it exists...
  if (CPV->hasName())
    Out << "%" << CPV->getName() << " = ";

  // Print out the opcode...
  Out << CPV->getType();

  // Write the value out now...
  writeOperand(CPV, false, false);

  if (!CPV->hasName() && CPV->getType() != Type::VoidTy) {
    int Slot = Table.getValSlot(CPV); // Print out the def slot taken...
    Out << "\t\t; <" << CPV->getType() << ">:";
    if (Slot >= 0) Out << Slot;
    else Out << "<badref>";
  } 

  Out << endl;
  return false;
}

// processMethod - Process all aspects of a method.
//
bool AssemblyWriter::processMethod(const Method *M) {
  // Print out the return type and name...
  Out << "\n" << M->getReturnType() << " \"" << M->getName() << "\"(";
  Table.incorporateMethod(M);
  ModuleAnalyzer::processMethod(M);
  Table.purgeMethod();
  Out << "end\n";
  return false;
}

// processMethodArgument - This member is called for every argument that 
// is passed into the method.  Simply print it out
//
bool AssemblyWriter::processMethodArgument(const MethodArgument *Arg) {
  // Insert commas as we go... the first arg doesn't get a comma
  if (Arg != Arg->getParent()->getArgumentList().front()) Out << ", ";

  // Output type...
  Out << Arg->getType();
  
  // Output name, if available...
  if (Arg->hasName())
    Out << " %" << Arg->getName();
  else if (Table.getValSlot(Arg) < 0)
    Out << "<badref>";
  
  return false;
}

// processBasicBlock - This member is called for each basic block in a methd.
//
bool AssemblyWriter::processBasicBlock(const BasicBlock *BB) {
  if (BB->hasName()) {              // Print out the label if it exists...
    Out << "\n" << BB->getName() << ":";
  } else {
    int Slot = Table.getValSlot(BB);
    Out << "\n; <label>:";
    if (Slot >= 0) 
      Out << Slot;         // Extra newline seperates out label's
    else 
      Out << "<badref>"; 
  }
  Out << "\t\t\t\t\t;[#uses=" << BB->use_size() << "]\n";  // Output # uses

  ModuleAnalyzer::processBasicBlock(BB);
  return false;
}

// processInstruction - This member is called for each Instruction in a methd.
//
bool AssemblyWriter::processInstruction(const Instruction *I) {
  Out << "\t";

  // Print out name if it exists...
  if (I && I->hasName())
    Out << "%" << I->getName() << " = ";

  // Print out the opcode...
  Out << I->getOpcode();

  // Print out the type of the operands...
  const Value *Operand = I->getOperand(0);

  // Special case conditional branches to swizzle the condition out to the front
  if (I->getInstType() == Instruction::Br && I->getOperand(1)) {
    writeOperand(I->getOperand(2), true);
    Out << ",";
    writeOperand(Operand, true);
    Out << ",";
    writeOperand(I->getOperand(1), true);

  } else if (I->getInstType() == Instruction::Switch) {
    // Special case switch statement to get formatting nice and correct...
    writeOperand(Operand         , true); Out << ",";
    writeOperand(I->getOperand(1), true); Out << " [";

    for (unsigned op = 2; (Operand = I->getOperand(op)); op += 2) {
      Out << "\n\t\t";
      writeOperand(Operand, true); Out << ",";
      writeOperand(I->getOperand(op+1), true);
    }
    Out << "\n\t]";
  } else if (I->getInstType() == Instruction::PHINode) {
    Out << " " << Operand->getType();

    Out << " [";  writeOperand(Operand, false); Out << ",";
    writeOperand(I->getOperand(1), false); Out << " ]";
    for (unsigned op = 2; (Operand = I->getOperand(op)); op += 2) {
      Out << ", [";  writeOperand(Operand, false); Out << ",";
      writeOperand(I->getOperand(op+1), false); Out << " ]";
    }
  } else if (I->getInstType() == Instruction::Ret && !Operand) {
    Out << " void";
  } else if (I->getInstType() == Instruction::Call) {
    writeOperand(Operand, true);
    Out << "(";
    Operand = I->getOperand(1);
    if (Operand) writeOperand(Operand, true);
    for (unsigned op = 2; (Operand = I->getOperand(op)); ++op) {
      Out << ",";
      writeOperand(Operand, true);
    }

    Out << " )";
  } else if (I->getInstType() == Instruction::Malloc || 
	     I->getInstType() == Instruction::Alloca) {
    Out << " " << ((const PointerType*)((ConstPoolType*)Operand)
		   ->getValue())->getValueType();
    if ((Operand = I->getOperand(1))) {
      Out << ","; writeOperand(Operand, true);
    }

  } else if (Operand) {   // Print the normal way...

    // PrintAllTypes - Instructions who have operands of all the same type 
    // omit the type from all but the first operand.  If the instruction has
    // different type operands (for example br), then they are all printed.
    bool PrintAllTypes = false;
    const Type *TheType = Operand->getType();
    unsigned i;

    for (i = 1; (Operand = I->getOperand(i)); i++) {
      if (Operand->getType() != TheType) {
	PrintAllTypes = true;       // We have differing types!  Print them all!
	break;
      }
    }

    if (!PrintAllTypes)
      Out << " " << I->getOperand(0)->getType();

    for (unsigned i = 0; (Operand = I->getOperand(i)); i++) {
      if (i) Out << ",";
      writeOperand(Operand, PrintAllTypes);
    }
  }

  // Print a little comment after the instruction indicating which slot it
  // occupies.
  //
  if (I->getType() != Type::VoidTy) {
    Out << "\t\t; <" << I->getType() << ">";

    if (!I->hasName()) {
      int Slot = Table.getValSlot(I); // Print out the def slot taken...
      if (Slot >= 0) Out << ":" << Slot;
      else Out << ":<badref>";
    }
    Out << "\t[#uses=" << I->use_size() << "]";  // Output # uses
  }
  Out << endl;

  return false;
}


void AssemblyWriter::writeOperand(const Value *Operand, bool PrintType, 
				  bool PrintName) {
  if (PrintType)
    Out << " " << Operand->getType();
  
  if (Operand->hasName() && PrintName) {
    Out << " %" << Operand->getName();
  } else {
    int Slot = Table.getValSlot(Operand);
    
    if (Operand->getValueType() == Value::ConstantVal) {
      Out << " " << ((ConstPoolVal*)Operand)->getStrValue();
    } else {
      if (Slot >= 0)  Out << " %" << Slot;
      else if (PrintName)
        Out << "<badref>";     // Not embeded into a location?
    }
  }
}


//===----------------------------------------------------------------------===//
//                       External Interface declarations
//===----------------------------------------------------------------------===//



void WriteToAssembly(const Module *M, ostream &o) {
  if (M == 0) { o << "<null> module\n"; return; }
  SlotCalculator SlotTable(M, true);
  AssemblyWriter W(o, SlotTable);

  W.write(M);
}

void WriteToAssembly(const Method *M, ostream &o) {
  if (M == 0) { o << "<null> method\n"; return; }
  SlotCalculator SlotTable(M->getParent(), true);
  AssemblyWriter W(o, SlotTable);

  W.write(M);
}


void WriteToAssembly(const BasicBlock *BB, ostream &o) {
  if (BB == 0) { o << "<null> basic block\n"; return; }

  SlotCalculator SlotTable(BB->getParent(), true);
  AssemblyWriter W(o, SlotTable);

  W.write(BB);
}

void WriteToAssembly(const ConstPoolVal *CPV, ostream &o) {
  if (CPV == 0) { o << "<null> constant pool value\n"; return; }

  SlotCalculator *SlotTable;

  // A Constant pool value may have a parent that is either a method or a 
  // module.  Untangle this now...
  //
  if (CPV->getParent() == 0 || 
      CPV->getParent()->getValueType() == Value::MethodVal) {
    SlotTable = new SlotCalculator((Method*)CPV->getParent(), true);
  } else {
    assert(CPV->getParent()->getValueType() == Value::ModuleVal);
    SlotTable = new SlotCalculator((Module*)CPV->getParent(), true);
  }

  AssemblyWriter W(o, *SlotTable);
  W.write(CPV);

  delete SlotTable;
}

void WriteToAssembly(const Instruction *I, ostream &o) {
  if (I == 0) { o << "<null> instruction\n"; return; }

  SlotCalculator SlotTable(I->getParent() ? I->getParent()->getParent() : 0, 
			   true);
  AssemblyWriter W(o, SlotTable);

  W.write(I);
}
