//===-- EmitAssembly.cpp - Emit Sparc Specific .s File ---------------------==//
//
// This file implements all of the stuff neccesary to output a .s file from
// LLVM.  The code in this file assumes that the specified module has already
// been compiled into the internal data structures of the Module.
//
// The entry point of this file is the UltraSparc::emitAssembly method.
//
//===----------------------------------------------------------------------===//

#include "SparcInternals.h"
#include "llvm/Analysis/SlotCalculator.h"
#include "llvm/Transforms/Linker.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/GlobalVariable.h"
#include "llvm/GlobalValue.h"
#include "llvm/ConstPoolVals.h"
#include "llvm/DerivedTypes.h"
#include "llvm/BasicBlock.h"
#include "llvm/Method.h"
#include "llvm/Module.h"
#include "llvm/Support/HashExtras.h"
#include "llvm/Support/StringExtras.h"
#include <locale.h>

namespace {


class SparcAsmPrinter {
  typedef hash_map<const Value*, int> ValIdMap;
  typedef ValIdMap::      iterator ValIdMapIterator;
  typedef ValIdMap::const_iterator ValIdMapConstIterator;
  
  ostream &toAsm;
  SlotCalculator Table;   // map anonymous values to unique integer IDs
  ValIdMap valToIdMap;    // used for values not handled by SlotCalculator 
  const UltraSparc &Target;
  
  enum Sections {
    Unknown,
    Text,
    ReadOnlyData,
    InitRWData,
    UninitRWData,
  } CurSection;
  
public:
  inline SparcAsmPrinter(ostream &o, const Module *M, const UltraSparc &t)
    : toAsm(o), Table(SlotCalculator(M, true)), Target(t), CurSection(Unknown) {
    emitModule(M);
  }

private :
  void emitModule(const Module *M);
  void emitMethod(const Method *M);
  void emitGlobalsAndConstants(const Module* module);
  //void processMethodArgument(const MethodArgument *MA);
  void emitBasicBlock(const BasicBlock *BB);
  void emitMachineInst(const MachineInstr *MI);
  
  void printGlobalVariable(const GlobalVariable* GV);
  void printSingleConstant(const ConstPoolVal* CV, string valID = string(""));
  void printConstant(      const ConstPoolVal* CV, string valID = string(""));
  
  unsigned int printOperands(const MachineInstr *MI, unsigned int opNum);
  void printOneOperand(const MachineOperand &Op);

  bool OpIsBranchTargetLabel(const MachineInstr *MI, unsigned int opNum);
  bool OpIsMemoryAddressBase(const MachineInstr *MI, unsigned int opNum);
  
  // enterSection - Use this method to enter a different section of the output
  // executable.  This is used to only output neccesary section transitions.
  //
  void enterSection(enum Sections S) {
    if (S == CurSection) return;        // Only switch section if neccesary
    CurSection = S;

    toAsm << "\n\t.section ";
    switch (S)
      {
      default: assert(0 && "Bad section name!");
      case Text:         toAsm << "\".text\""; break;
      case ReadOnlyData: toAsm << "\".rodata\",#alloc"; break;
      case InitRWData:   toAsm << "\".data\",#alloc,#write"; break;
      case UninitRWData: toAsm << "\".bss\",#alloc,#write\nBbss.bss:"; break;
      }
    toAsm << "\n";
  }

  string getValidSymbolName(const string &S) {
    string Result;
    
    // Symbol names in Sparc assembly language have these rules:
    // (a) Must match { letter | _ | . | $ } { letter | _ | . | $ | digit }*
    // (b) A name beginning in "." is treated as a local name.
    // (c) Names beginning with "_" are reserved by ANSI C and shd not be used.
    // 
    if (S[0] == '_' || isdigit(S[0]))
      Result += "ll";
    
    for (unsigned i = 0; i < S.size(); ++i)
      {
        char C = S[i];
        if (C == '_' || C == '.' || C == '$' || isalpha(C) || isdigit(C))
          Result += C;
        else
          {
            Result += '_';
            Result += char('0' + ((unsigned char)C >> 4));
            Result += char('0' + (C & 0xF));
          }
      }
    return Result;
  }

  // getID - Return a valid identifier for the specified value.  Base it on
  // the name of the identifier if possible, use a numbered value based on
  // prefix otherwise.  FPrefix is always prepended to the output identifier.
  //
  string getID(const Value *V, const char *Prefix, const char *FPrefix = 0) {
    string Result;
    string FP(FPrefix ? FPrefix : "");  // "Forced prefix"
    if (V->hasName()) {
      Result = FP + V->getName();
    } else {
      int valId = Table.getValSlot(V);
      if (valId == -1) {
        ValIdMapConstIterator I = valToIdMap.find(V);
        valId = (I == valToIdMap.end())? (valToIdMap[V] = valToIdMap.size())
                                       : (*I).second;
      }
      Result = FP + string(Prefix) + itostr(valId);
    }
    return getValidSymbolName(Result);
  }
  
  // getID Wrappers - Ensure consistent usage...
  string getID(const Module *M) {
    return getID(M, "LLVMModule_");
  }
  string getID(const Method *M) {
    return getID(M, "LLVMMethod_");
  }
  string getID(const BasicBlock *BB) {
    return getID(BB, "LL", (".L_"+getID(BB->getParent())+"_").c_str());
  }
  string getID(const GlobalVariable *GV) {
    return getID(GV, "LLVMGlobal_", ".G_");
  }
  string getID(const ConstPoolVal *CV) {
    return getID(CV, "LLVMConst_", ".C_");
  }
  
  unsigned getOperandMask(unsigned Opcode) {
    switch (Opcode) {
    case SUBcc:   return 1 << 3;  // Remove CC argument
    case BA:    case BRZ:         // Remove Arg #0, which is always null or xcc
    case BRLEZ: case BRLZ:
    case BRNZ:  case BRGZ:
    case BRGEZ:   return 1 << 0;

    default:      return 0;       // By default, don't hack operands...
    }
  }
};

inline bool
SparcAsmPrinter::OpIsBranchTargetLabel(const MachineInstr *MI,
                                       unsigned int opNum) {
  switch (MI->getOpCode()) {
  case JMPLCALL:
  case JMPLRET: return (opNum == 0);
  default:      return false;
  }
}


inline bool
SparcAsmPrinter::OpIsMemoryAddressBase(const MachineInstr *MI,
                                       unsigned int opNum) {
  if (Target.getInstrInfo().isLoad(MI->getOpCode()))
    return (opNum == 0);
  else if (Target.getInstrInfo().isStore(MI->getOpCode()))
    return (opNum == 1);
  else
    return false;
}


#define PrintOp1PlusOp2(Op1, Op2) \
  printOneOperand(Op1); \
  toAsm << "+"; \
  printOneOperand(Op2);

unsigned int
SparcAsmPrinter::printOperands(const MachineInstr *MI,
                               unsigned int opNum)
{
  const MachineOperand& Op = MI->getOperand(opNum);
  
  if (OpIsBranchTargetLabel(MI, opNum))
    {
      PrintOp1PlusOp2(Op, MI->getOperand(opNum+1));
      return 2;
    }
  else if (OpIsMemoryAddressBase(MI, opNum))
    {
      toAsm << "[";
      PrintOp1PlusOp2(Op, MI->getOperand(opNum+1));
      toAsm << "]";
      return 2;
    }
  else
    {
      printOneOperand(Op);
      return 1;
    }
}


void
SparcAsmPrinter::printOneOperand(const MachineOperand &op)
{
  switch (op.getOperandType())
    {
    case MachineOperand::MO_VirtualRegister:
    case MachineOperand::MO_CCRegister:
    case MachineOperand::MO_MachineRegister:
      {
        int RegNum = (int)op.getAllocatedRegNum();
        
        // ****this code is temporary till NULL Values are fixed
        if (RegNum == 10000) {
          toAsm << "<NULL VALUE>";
        } else {
          toAsm << "%" << Target.getRegInfo().getUnifiedRegName(RegNum);
        }
        break;
      }
    
    case MachineOperand::MO_PCRelativeDisp:
      {
        const Value *Val = op.getVRegValue();
        if (!Val)
          toAsm << "\t<*NULL Value*>";
        else if (const BasicBlock *BB = dyn_cast<const BasicBlock>(Val))
          toAsm << getID(BB);
        else if (const Method *M = dyn_cast<const Method>(Val))
          toAsm << getID(M);
        else if (const GlobalVariable *GV=dyn_cast<const GlobalVariable>(Val))
          toAsm << getID(GV);
        else if (const ConstPoolVal *CV = dyn_cast<const ConstPoolVal>(Val))
          toAsm << getID(CV);
        else
          toAsm << "<unknown value=" << Val << ">";
        break;
      }
    
    case MachineOperand::MO_SignExtendedImmed:
    case MachineOperand::MO_UnextendedImmed:
      toAsm << op.getImmedValue();
      break;
    
    default:
      toAsm << op;      // use dump field
      break;
    }
}


void
SparcAsmPrinter::emitMachineInst(const MachineInstr *MI)
{
  unsigned Opcode = MI->getOpCode();

  if (TargetInstrDescriptors[Opcode].iclass & M_DUMMY_PHI_FLAG)
    return;  // IGNORE PHI NODES

  toAsm << "\t" << TargetInstrDescriptors[Opcode].opCodeString << "\t";

  unsigned Mask = getOperandMask(Opcode);
  
  bool NeedComma = false;
  unsigned N = 1;
  for (unsigned OpNum = 0; OpNum < MI->getNumOperands(); OpNum += N)
    if (! ((1 << OpNum) & Mask)) {        // Ignore this operand?
      if (NeedComma) toAsm << ", ";         // Handle comma outputing
      NeedComma = true;
      N = printOperands(MI, OpNum);
    }
  else
    N = 1;
  
  toAsm << endl;
}

void
SparcAsmPrinter::emitBasicBlock(const BasicBlock *BB)
{
  // Emit a label for the basic block
  toAsm << getID(BB) << ":\n";

  // Get the vector of machine instructions corresponding to this bb.
  const MachineCodeForBasicBlock &MIs = BB->getMachineInstrVec();
  MachineCodeForBasicBlock::const_iterator MII = MIs.begin(), MIE = MIs.end();

  // Loop over all of the instructions in the basic block...
  for (; MII != MIE; ++MII)
    emitMachineInst(*MII);
  toAsm << "\n";  // Seperate BB's with newlines
}

void
SparcAsmPrinter::emitMethod(const Method *M)
{
  if (M->isExternal()) return;

  // Make sure the slot table has information about this method...
  Table.incorporateMethod(M);

  string methName = getID(M);
  toAsm << "!****** Outputing Method: " << methName << " ******\n";
  enterSection(Text);
  toAsm << "\t.align\t4\n\t.global\t" << methName << "\n";
  //toAsm << "\t.type\t" << methName << ",#function\n";
  toAsm << "\t.type\t" << methName << ", 2\n";
  toAsm << methName << ":\n";

  // Output code for all of the basic blocks in the method...
  for (Method::const_iterator I = M->begin(), E = M->end(); I != E; ++I)
    emitBasicBlock(*I);

  // Output a .size directive so the debugger knows the extents of the function
  toAsm << ".EndOf_" << methName << ":\n\t.size "
        << methName << ", .EndOf_"
        << methName << "-" << methName << endl;

  // Put some spaces between the methods
  toAsm << "\n\n";

  // Forget all about M.
  Table.purgeMethod();
}

inline bool
ArrayTypeIsString(ArrayType* arrayType)
{
  return (arrayType->getElementType() == Type::UByteTy ||
          arrayType->getElementType() == Type::SByteTy);
}

inline const string
TypeToDataDirective(const Type* type)
{
  switch(type->getPrimitiveID())
    {
    case Type::BoolTyID: case Type::UByteTyID: case Type::SByteTyID:
      return ".byte";
    case Type::UShortTyID: case Type::ShortTyID:
      return ".half";
    case Type::UIntTyID: case Type::IntTyID:
      return ".word";
    case Type::ULongTyID: case Type::LongTyID: case Type::PointerTyID:
      return ".xword";
    case Type::FloatTyID:
      return ".single";
    case Type::DoubleTyID:
      return ".double";
    case Type::ArrayTyID:
      if (ArrayTypeIsString((ArrayType*) type))
        return ".ascii";
      else
        return "<InvaliDataTypeForPrinting>";
    default:
      return "<InvaliDataTypeForPrinting>";
    }
}

inline unsigned int
ConstantToSize(const ConstPoolVal* CV, const TargetMachine& target)
{
  if (ConstPoolArray* AV = dyn_cast<ConstPoolArray>(CV))
    if (ArrayTypeIsString((ArrayType*) CV->getType()))
      return 1 + AV->getNumOperands();
  
  return target.findOptimalStorageSize(CV->getType());
}


inline
unsigned int TypeToSize(const Type* type, const TargetMachine& target)
{
  return target.findOptimalStorageSize(type);
}


// Align data larger than one L1 cache line on L1 cache line boundaries.
// Align all smaller types on the next higher 2^x boundary (4, 8, ...).
// 
inline unsigned int
TypeToAlignment(const Type* type, const TargetMachine& target)
{
  unsigned int typeSize = target.findOptimalStorageSize(type);
  unsigned short cacheLineSize = target.getCacheInfo().getCacheLineSize(1); 
  if (typeSize > (int) cacheLineSize / 2)
    return cacheLineSize;
  else
    for (unsigned sz=1; /*no condition*/; sz *= 2)
      if (sz >= typeSize)
        return sz;
}


void
SparcAsmPrinter::printSingleConstant(const ConstPoolVal* CV,string valID)
{
  if (valID.length() == 0)
    valID = getID(CV);
  
  assert(CV->getType() != Type::VoidTy &&
         CV->getType() != Type::TypeTy &&
         CV->getType() != Type::LabelTy &&
         "Unexpected type for ConstPoolVal");
  
  assert((! isa<ConstPoolArray>( CV) && ! isa<ConstPoolStruct>(CV))
         && "Collective types should be handled outside this function");
  
  toAsm << "\t"
        << TypeToDataDirective(CV->getType()) << "\t";
  
  if (CV->getType()->isPrimitiveType())
    {
      if (CV->getType() == Type::FloatTy || CV->getType() == Type::DoubleTy)
        toAsm << "0r";                  // FP constants must have this prefix
      toAsm << CV->getStrValue() << endl;
    }
  else if (ConstPoolPointer* CPP = dyn_cast<ConstPoolPointer>(CV))
    {
      if (! CPP->isNullValue())
        assert(0 && "Cannot yet print non-null pointer constants to assembly");
      else
        toAsm << (void*) NULL << endl;
    }
  else if (ConstPoolPointerRef* CPRef = dyn_cast<ConstPoolPointerRef>(CV))
    {
      assert(0 && "Cannot yet initialize pointer refs in assembly");
    }
  else
    {
      assert(0 && "Unknown elementary type for constant");
    }
}

void
SparcAsmPrinter::printConstant(const ConstPoolVal* CV, string valID)
{
  if (valID.length() == 0)
    valID = getID(CV);
  
  assert(CV->getType() != Type::VoidTy &&
         CV->getType() != Type::TypeTy &&
         CV->getType() != Type::LabelTy &&
         "Unexpected type for ConstPoolVal");
  
  toAsm << "\t.align\t" << TypeToAlignment(CV->getType(), Target)
        << endl;
  
  // Print .size and .type only if it is not a string.
  ConstPoolArray *CPA = dyn_cast<ConstPoolArray>(CV);
  
  if (CPA && isStringCompatible(CPA))
    { // print it as a string and return
      toAsm << valID << ":" << endl;
      toAsm << "\t" << TypeToDataDirective(CV->getType()) << "\t"
            << getAsCString(CPA) << endl;
      return;
    }
      
  toAsm << "\t.type" << "\t" << valID << ",#object" << endl;
  toAsm << "\t.size" << "\t" << valID << ","
        << ConstantToSize(CV, Target) << endl;
  toAsm << valID << ":" << endl;
  
  if (CPA)
    { // Not a string.  Print the values in successive locations
      const vector<Use>& constValues = CPA->getValues();
      for (unsigned i=1; i < constValues.size(); i++)
        this->printSingleConstant(cast<ConstPoolVal>(constValues[i].get()));
    }
  else if (ConstPoolStruct *CPS = dyn_cast<ConstPoolStruct>(CV))
    { // Print the fields in successive locations
      const vector<Use>& constValues = CPA->getValues();
      for (unsigned i=1; i < constValues.size(); i++)
        this->printSingleConstant(cast<ConstPoolVal>(constValues[i].get()));
    }
  else
    this->printSingleConstant(CV, valID);
}


void
SparcAsmPrinter::printGlobalVariable(const GlobalVariable* GV)
{
  toAsm << "\t.global\t" << getID(GV) << endl;
  
  if (GV->hasInitializer())
    printConstant(GV->getInitializer(), getID(GV));
  else {
    toAsm << "\t.align\t"
          << TypeToAlignment(GV->getType()->getValueType(), Target) << endl;
    toAsm << "\t.type\t" << getID(GV) << ",#object" << endl;
    toAsm << "\t.reserve\t" << getID(GV) << ","
          << TypeToSize(GV->getType()->getValueType(), Target)
          << endl;
  }
}


static void
FoldConstPools(const Module *M,
               hash_set<const ConstPoolVal*>& moduleConstPool)
{
  for (Module::const_iterator I = M->begin(), E = M->end(); I != E; ++I)
    if (! (*I)->isExternal())
      {
        const hash_set<const ConstPoolVal*>& pool =
          MachineCodeForMethod::get(*I).getConstantPoolValues();
        moduleConstPool.insert(pool.begin(), pool.end());
      }
}


void
SparcAsmPrinter::emitGlobalsAndConstants(const Module *M)
{
  // First, get the constants there were marked by the code generator for
  // inclusion in the assembly code data area and fold them all into a
  // single constant pool since there may be lots of duplicates.  Also,
  // lets force these constants into the slot table so that we can get
  // unique names for unnamed constants also.
  // 
  hash_set<const ConstPoolVal*> moduleConstPool;
  FoldConstPools(M, moduleConstPool);
  
  // Now, emit the three data sections separately; the cost of I/O should
  // make up for the cost of extra passes over the globals list!
  // 
  // Read-only data section (implies initialized)
  for (Module::const_giterator GI=M->gbegin(), GE=M->gend(); GI != GE; ++GI)
    {
      const GlobalVariable* GV = *GI;
      if (GV->hasInitializer() && GV->isConstant())
        {
          if (GI == M->gbegin())
            enterSection(ReadOnlyData);
          printGlobalVariable(GV);
        }
  }
  
  for (hash_set<const ConstPoolVal*>::const_iterator I=moduleConstPool.begin(),
         E = moduleConstPool.end();  I != E; ++I)
    printConstant(*I);
  
  // Initialized read-write data section
  for (Module::const_giterator GI=M->gbegin(), GE=M->gend(); GI != GE; ++GI)
    {
      const GlobalVariable* GV = *GI;
      if (GV->hasInitializer() && ! GV->isConstant())
        {
          if (GI == M->gbegin())
            enterSection(InitRWData);
          printGlobalVariable(GV);
        }
  }

  // Uninitialized read-write data section
  for (Module::const_giterator GI=M->gbegin(), GE=M->gend(); GI != GE; ++GI)
    {
      const GlobalVariable* GV = *GI;
      if (! GV->hasInitializer())
        {
          if (GI == M->gbegin())
            enterSection(UninitRWData);
          printGlobalVariable(GV);
        }
  }

  toAsm << endl;
}


void
SparcAsmPrinter::emitModule(const Module *M)
{
  // TODO: Look for a filename annotation on M to emit a .file directive
  for (Module::const_iterator I = M->begin(), E = M->end(); I != E; ++I)
    emitMethod(*I);
  
  emitGlobalsAndConstants(M);
}

}  // End anonymous namespace


//
// emitAssembly - Output assembly language code (a .s file) for the specified
// method. The specified method must have been compiled before this may be
// used.
//
void
UltraSparc::emitAssembly(const Module *M, ostream &toAsm) const
{
  SparcAsmPrinter Print(toAsm, M, *this);
}
