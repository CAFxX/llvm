//===- LevelRaise.cpp - Code to change LLVM to higher level -----------------=//
//
// This file implements the 'raising' part of the LevelChange API.  This is
// useful because, in general, it makes the LLVM code terser and easier to
// analyze.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/LevelChange.h"
#include "llvm/Transforms/Utils/Local.h"
#include "TransformInternals.h"
#include "llvm/iOther.h"
#include "llvm/iMemory.h"
#include "llvm/Pass.h"
#include "llvm/ConstantHandling.h"
#include "llvm/Analysis/Expressions.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "Support/STLExtras.h"
#include "Support/StatisticReporter.h"
#include <algorithm>
#include <iostream>
using std::cerr;

static Statistic<> NumLoadStorePeepholes("raise\t\t- Number of load/store peepholes");
static Statistic<> NumGEPInstFormed("raise\t\t- Number of other getelementptr's formed");
static Statistic<> NumExprTreesConv("raise\t\t- Number of expression trees converted");
static Statistic<> NumCastOfCast("raise\t\t- Number of cast-of-self removed");
static Statistic<> NumDCEorCP("raise\t\t- Number of insts DCE'd or constprop'd");


#define PRINT_PEEPHOLE(ID, NUM, I)            \
  DEBUG(std::cerr << "Inst P/H " << ID << "[" << NUM << "] " << I)

#define PRINT_PEEPHOLE1(ID, I1) do { PRINT_PEEPHOLE(ID, 0, I1); } while (0)
#define PRINT_PEEPHOLE2(ID, I1, I2) \
  do { PRINT_PEEPHOLE(ID, 0, I1); PRINT_PEEPHOLE(ID, 1, I2); } while (0)
#define PRINT_PEEPHOLE3(ID, I1, I2, I3) \
  do { PRINT_PEEPHOLE(ID, 0, I1); PRINT_PEEPHOLE(ID, 1, I2); \
       PRINT_PEEPHOLE(ID, 2, I3); } while (0)
#define PRINT_PEEPHOLE4(ID, I1, I2, I3, I4) \
  do { PRINT_PEEPHOLE(ID, 0, I1); PRINT_PEEPHOLE(ID, 1, I2); \
       PRINT_PEEPHOLE(ID, 2, I3); PRINT_PEEPHOLE(ID, 3, I4); } while (0)


// isReinterpretingCast - Return true if the cast instruction specified will
// cause the operand to be "reinterpreted".  A value is reinterpreted if the
// cast instruction would cause the underlying bits to change.
//
static inline bool isReinterpretingCast(const CastInst *CI) {
  return!CI->getOperand(0)->getType()->isLosslesslyConvertableTo(CI->getType());
}


// Peephole optimize the following instructions:
// %t1 = cast ? to x *
// %t2 = add x * %SP, %t1              ;; Constant must be 2nd operand
//
// Into: %t3 = getelementptr {<...>} * %SP, <element indices>
//       %t2 = cast <eltype> * %t3 to {<...>}*
//
static bool HandleCastToPointer(BasicBlock::iterator BI,
                                const PointerType *DestPTy) {
  CastInst &CI = cast<CastInst>(*BI);
  if (CI.use_empty()) return false;

  // Scan all of the uses, looking for any uses that are not add
  // instructions.  If we have non-adds, do not make this transformation.
  //
  for (Value::use_iterator I = CI.use_begin(), E = CI.use_end();
       I != E; ++I) {
    if (BinaryOperator *BO = dyn_cast<BinaryOperator>(*I)) {
      if (BO->getOpcode() != Instruction::Add)
        return false;
    } else {
      return false;
    }
  }

  std::vector<Value*> Indices;
  Value *Src = CI.getOperand(0);
  const Type *Result = ConvertableToGEP(DestPTy, Src, Indices, &BI);
  if (Result == 0) return false;  // Not convertable...

  PRINT_PEEPHOLE2("cast-add-to-gep:in", Src, CI);

  // If we have a getelementptr capability... transform all of the 
  // add instruction uses into getelementptr's.
  while (!CI.use_empty()) {
    BinaryOperator *I = cast<BinaryOperator>(*CI.use_begin());
    assert(I->getOpcode() == Instruction::Add && I->getNumOperands() == 2 &&
           "Use is not a valid add instruction!");
    
    // Get the value added to the cast result pointer...
    Value *OtherPtr = I->getOperand((I->getOperand(0) == &CI) ? 1 : 0);

    Instruction *GEP = new GetElementPtrInst(OtherPtr, Indices, I->getName());
    PRINT_PEEPHOLE1("cast-add-to-gep:i", I);

    if (GEP->getType() == I->getType()) {
      // Replace the old add instruction with the shiny new GEP inst
      ReplaceInstWithInst(I, GEP);
    } else {
      // If the type produced by the gep instruction differs from the original
      // add instruction type, insert a cast now.
      //

      // Insert the GEP instruction before the old add instruction...
      I->getParent()->getInstList().insert(I, GEP);

      PRINT_PEEPHOLE1("cast-add-to-gep:o", GEP);
      GEP = new CastInst(GEP, I->getType());

      // Replace the old add instruction with the shiny new GEP inst
      ReplaceInstWithInst(I, GEP);
    }

    PRINT_PEEPHOLE1("cast-add-to-gep:o", GEP);
  }
  return true;
}

// Peephole optimize the following instructions:
// %t1 = cast ulong <const int> to {<...>} *
// %t2 = add {<...>} * %SP, %t1              ;; Constant must be 2nd operand
//
//    or
// %t1 = cast {<...>}* %SP to int*
// %t5 = cast ulong <const int> to int*
// %t2 = add int* %t1, %t5                   ;; int is same size as field
//
// Into: %t3 = getelementptr {<...>} * %SP, <element indices>
//       %t2 = cast <eltype> * %t3 to {<...>}*
//
static bool PeepholeOptimizeAddCast(BasicBlock *BB, BasicBlock::iterator &BI,
                                    Value *AddOp1, CastInst *AddOp2) {
  const CompositeType *CompTy;
  Value *OffsetVal = AddOp2->getOperand(0);
  Value *SrcPtr;  // Of type pointer to struct...

  if ((CompTy = getPointedToComposite(AddOp1->getType()))) {
    SrcPtr = AddOp1;                      // Handle the first case...
  } else if (CastInst *AddOp1c = dyn_cast<CastInst>(AddOp1)) {
    SrcPtr = AddOp1c->getOperand(0);      // Handle the second case...
    CompTy = getPointedToComposite(SrcPtr->getType());
  }

  // Only proceed if we have detected all of our conditions successfully...
  if (!CompTy || !SrcPtr || !OffsetVal->getType()->isIntegral())
    return false;

  std::vector<Value*> Indices;
  if (!ConvertableToGEP(SrcPtr->getType(), OffsetVal, Indices, &BI))
    return false;  // Not convertable... perhaps next time

  if (getPointedToComposite(AddOp1->getType())) {  // case 1
    PRINT_PEEPHOLE2("add-to-gep1:in", AddOp2, *BI);
  } else {
    PRINT_PEEPHOLE3("add-to-gep2:in", AddOp1, AddOp2, *BI);
  }

  GetElementPtrInst *GEP = new GetElementPtrInst(SrcPtr, Indices,
                                                 AddOp2->getName());
  BI = ++BB->getInstList().insert(BI, GEP);

  Instruction *NCI = new CastInst(GEP, AddOp1->getType());
  ReplaceInstWithInst(BB->getInstList(), BI, NCI);
  PRINT_PEEPHOLE2("add-to-gep:out", GEP, NCI);
  return true;
}

static bool PeepholeOptimize(BasicBlock *BB, BasicBlock::iterator &BI) {
  Instruction *I = BI;

  if (CastInst *CI = dyn_cast<CastInst>(I)) {
    Value       *Src    = CI->getOperand(0);
    Instruction *SrcI   = dyn_cast<Instruction>(Src); // Nonnull if instr source
    const Type  *DestTy = CI->getType();

    // Peephole optimize the following instruction:
    // %V2 = cast <ty> %V to <ty>
    //
    // Into: <nothing>
    //
    if (DestTy == Src->getType()) {   // Check for a cast to same type as src!!
      PRINT_PEEPHOLE1("cast-of-self-ty", CI);
      CI->replaceAllUsesWith(Src);
      if (!Src->hasName() && CI->hasName()) {
        std::string Name = CI->getName();
        CI->setName("");
        Src->setName(Name, BB->getParent()->getSymbolTable());
      }

      // DCE the instruction now, to avoid having the iterative version of DCE
      // have to worry about it.
      //
      BI = BB->getInstList().erase(BI);

      ++NumCastOfCast;
      return true;
    }

    // Check to see if it's a cast of an instruction that does not depend on the
    // specific type of the operands to do it's job.
    if (!isReinterpretingCast(CI)) {
      ValueTypeCache ConvertedTypes;

      // Check to see if we can convert the source of the cast to match the
      // destination type of the cast...
      //
      ConvertedTypes[CI] = CI->getType();  // Make sure the cast doesn't change
      if (ExpressionConvertableToType(Src, DestTy, ConvertedTypes)) {
        PRINT_PEEPHOLE3("CAST-SRC-EXPR-CONV:in ", Src, CI, BB->getParent());
          
        DEBUG(cerr << "\nCONVERTING SRC EXPR TYPE:\n");
        ValueMapCache ValueMap;
        Value *E = ConvertExpressionToType(Src, DestTy, ValueMap);
        if (Constant *CPV = dyn_cast<Constant>(E))
          CI->replaceAllUsesWith(CPV);

        BI = BB->begin();  // Rescan basic block.  BI might be invalidated.
        PRINT_PEEPHOLE1("CAST-SRC-EXPR-CONV:out", E);
        DEBUG(cerr << "DONE CONVERTING SRC EXPR TYPE: \n" << BB->getParent());
        ++NumExprTreesConv;
        return true;
      }

      // Check to see if we can convert the users of the cast value to match the
      // source type of the cast...
      //
      ConvertedTypes.clear();
      if (ValueConvertableToType(CI, Src->getType(), ConvertedTypes)) {
        PRINT_PEEPHOLE3("CAST-DEST-EXPR-CONV:in ", Src, CI, BB->getParent());

        DEBUG(cerr << "\nCONVERTING EXPR TYPE:\n");
        ValueMapCache ValueMap;
        ConvertValueToNewType(CI, Src, ValueMap);  // This will delete CI!

        BI = BB->begin();  // Rescan basic block.  BI might be invalidated.
        PRINT_PEEPHOLE1("CAST-DEST-EXPR-CONV:out", Src);
        DEBUG(cerr << "DONE CONVERTING EXPR TYPE: \n\n" << BB->getParent());
        ++NumExprTreesConv;
        return true;
      }
    }

    // Otherwise find out it this cast is a cast to a pointer type, which is
    // then added to some other pointer, then loaded or stored through.  If
    // so, convert the add into a getelementptr instruction...
    //
    if (const PointerType *DestPTy = dyn_cast<PointerType>(DestTy)) {
      if (HandleCastToPointer(BI, DestPTy)) {
        BI = BB->begin();  // Rescan basic block.  BI might be invalidated.
        ++NumGEPInstFormed;
        return true;
      }
    }

    // Check to see if we are casting from a structure pointer to a pointer to
    // the first element of the structure... to avoid munching other peepholes,
    // we only let this happen if there are no add uses of the cast.
    //
    // Peephole optimize the following instructions:
    // %t1 = cast {<...>} * %StructPtr to <ty> *
    //
    // Into: %t2 = getelementptr {<...>} * %StructPtr, <0, 0, 0, ...>
    //       %t1 = cast <eltype> * %t1 to <ty> *
    //
    if (const CompositeType *CTy = getPointedToComposite(Src->getType()))
      if (const PointerType *DestPTy = dyn_cast<PointerType>(DestTy)) {

        // Loop over uses of the cast, checking for add instructions.  If an add
        // exists, this is probably a part of a more complex GEP, so we don't
        // want to mess around with the cast.
        //
        bool HasAddUse = false;
        for (Value::use_iterator I = CI->use_begin(), E = CI->use_end();
             I != E; ++I)
          if (isa<Instruction>(*I) &&
              cast<Instruction>(*I)->getOpcode() == Instruction::Add) {
            HasAddUse = true; break;
          }

        // If it doesn't have an add use, check to see if the dest type is
        // losslessly convertable to one of the types in the start of the struct
        // type.
        //
        if (!HasAddUse) {
          const Type *DestPointedTy = DestPTy->getElementType();
          unsigned Depth = 1;
          const CompositeType *CurCTy = CTy;
          const Type *ElTy = 0;

          // Build the index vector, full of all zeros
          std::vector<Value*> Indices;
          Indices.push_back(ConstantUInt::get(Type::UIntTy, 0));
          while (CurCTy && !isa<PointerType>(CurCTy)) {
            if (const StructType *CurSTy = dyn_cast<StructType>(CurCTy)) {
              // Check for a zero element struct type... if we have one, bail.
              if (CurSTy->getElementTypes().size() == 0) break;
            
              // Grab the first element of the struct type, which must lie at
              // offset zero in the struct.
              //
              ElTy = CurSTy->getElementTypes()[0];
            } else {
              ElTy = cast<ArrayType>(CurCTy)->getElementType();
            }

            // Insert a zero to index through this type...
            Indices.push_back(ConstantUInt::get(CurCTy->getIndexType(), 0));

            // Did we find what we're looking for?
            if (ElTy->isLosslesslyConvertableTo(DestPointedTy)) break;
            
            // Nope, go a level deeper.
            ++Depth;
            CurCTy = dyn_cast<CompositeType>(ElTy);
            ElTy = 0;
          }
          
          // Did we find what we were looking for? If so, do the transformation
          if (ElTy) {
            PRINT_PEEPHOLE1("cast-for-first:in", CI);

            // Insert the new T cast instruction... stealing old T's name
            GetElementPtrInst *GEP = new GetElementPtrInst(Src, Indices,
                                                           CI->getName());
            CI->setName("");
            BI = ++BB->getInstList().insert(BI, GEP);

            // Make the old cast instruction reference the new GEP instead of
            // the old src value.
            //
            CI->setOperand(0, GEP);
            
            PRINT_PEEPHOLE2("cast-for-first:out", GEP, CI);
            ++NumGEPInstFormed;
            return true;
          }
        }
      }

  } else if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
    Value *Val     = SI->getOperand(0);
    Value *Pointer = SI->getPointerOperand();
    
    // Peephole optimize the following instructions:
    // %t = cast <T1>* %P to <T2> * ;; If T1 is losslessly convertable to T2
    // store <T2> %V, <T2>* %t
    //
    // Into: 
    // %t = cast <T2> %V to <T1>
    // store <T1> %t2, <T1>* %P
    //
    // Note: This is not taken care of by expr conversion because there might
    // not be a cast available for the store to convert the incoming value of.
    // This code is basically here to make sure that pointers don't have casts
    // if possible.
    //
    if (CastInst *CI = dyn_cast<CastInst>(Pointer))
      if (Value *CastSrc = CI->getOperand(0)) // CSPT = CastSrcPointerType
        if (const PointerType *CSPT = dyn_cast<PointerType>(CastSrc->getType()))
          // convertable types?
          if (Val->getType()->isLosslesslyConvertableTo(CSPT->getElementType()) &&
              !SI->hasIndices()) {      // No subscripts yet!
            PRINT_PEEPHOLE3("st-src-cast:in ", Pointer, Val, SI);

            // Insert the new T cast instruction... stealing old T's name
            CastInst *NCI = new CastInst(Val, CSPT->getElementType(),
                                         CI->getName());
            CI->setName("");
            BI = ++BB->getInstList().insert(BI, NCI);

            // Replace the old store with a new one!
            ReplaceInstWithInst(BB->getInstList(), BI,
                                SI = new StoreInst(NCI, CastSrc));
            PRINT_PEEPHOLE3("st-src-cast:out", NCI, CastSrc, SI);
            ++NumLoadStorePeepholes;
            return true;
          }

  } else if (LoadInst *LI = dyn_cast<LoadInst>(I)) {
    Value *Pointer = LI->getOperand(0);
    const Type *PtrElType =
      cast<PointerType>(Pointer->getType())->getElementType();
    
    // Peephole optimize the following instructions:
    // %Val = cast <T1>* to <T2>*    ;; If T1 is losslessly convertable to T2
    // %t = load <T2>* %P
    //
    // Into: 
    // %t = load <T1>* %P
    // %Val = cast <T1> to <T2>
    //
    // Note: This is not taken care of by expr conversion because there might
    // not be a cast available for the store to convert the incoming value of.
    // This code is basically here to make sure that pointers don't have casts
    // if possible.
    //
    if (CastInst *CI = dyn_cast<CastInst>(Pointer))
      if (Value *CastSrc = CI->getOperand(0)) // CSPT = CastSrcPointerType
        if (const PointerType *CSPT = dyn_cast<PointerType>(CastSrc->getType()))
          // convertable types?
          if (PtrElType->isLosslesslyConvertableTo(CSPT->getElementType()) &&
              !LI->hasIndices()) {      // No subscripts yet!
            PRINT_PEEPHOLE2("load-src-cast:in ", Pointer, LI);

            // Create the new load instruction... loading the pre-casted value
            LoadInst *NewLI = new LoadInst(CastSrc, LI->getName());
            
            // Insert the new T cast instruction... stealing old T's name
            CastInst *NCI = new CastInst(NewLI, LI->getType(), CI->getName());
            BI = ++BB->getInstList().insert(BI, NewLI);

            // Replace the old store with a new one!
            ReplaceInstWithInst(BB->getInstList(), BI, NCI);
            PRINT_PEEPHOLE3("load-src-cast:out", NCI, CastSrc, NewLI);
            ++NumLoadStorePeepholes;
            return true;
          }

  } else if (I->getOpcode() == Instruction::Add &&
             isa<CastInst>(I->getOperand(1))) {

    if (PeepholeOptimizeAddCast(BB, BI, I->getOperand(0),
                                cast<CastInst>(I->getOperand(1)))) {
      ++NumGEPInstFormed;
      return true;
    }
  }

  return false;
}




static bool DoRaisePass(Function &F) {
  bool Changed = false;
  for (Function::iterator BB = F.begin(), BBE = F.end(); BB != BBE; ++BB)
    for (BasicBlock::iterator BI = BB->begin(); BI != BB->end();) {
      DEBUG(cerr << "Processing: " << *BI);
      if (dceInstruction(BI) || doConstantPropogation(BI)) {
        Changed = true; 
        ++NumDCEorCP;
        DEBUG(cerr << "***\t\t^^-- DeadCode Elinated!\n");
      } else if (PeepholeOptimize(BB, BI)) {
        Changed = true;
      } else {
        ++BI;
      }
    }

  return Changed;
}


// RaisePointerReferences::doit - Raise a function representation to a higher
// level.
//
static bool doRPR(Function &F) {
  DEBUG(cerr << "\n\n\nStarting to work on Function '" << F.getName() << "'\n");

  // Insert casts for all incoming pointer pointer values that are treated as
  // arrays...
  //
  bool Changed = false, LocalChange;
  
  do {
    DEBUG(cerr << "Looping: \n" << F);

    // Iterate over the function, refining it, until it converges on a stable
    // state
    LocalChange = false;
    while (DoRaisePass(F)) LocalChange = true;
    Changed |= LocalChange;

  } while (LocalChange);

  return Changed;
}

namespace {
  struct RaisePointerReferences : public FunctionPass {
    const char *getPassName() const { return "Raise Pointer References"; }

    virtual bool runOnFunction(Function &F) { return doRPR(F); }

    virtual void getAnalysisUsage(AnalysisUsage &AU) const {
      AU.preservesCFG();
    }
  };
}

Pass *createRaisePointerReferencesPass() {
  return new RaisePointerReferences();
}


