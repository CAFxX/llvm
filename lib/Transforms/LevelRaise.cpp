//===- LevelRaise.cpp - Code to change LLVM to higher level -----------------=//
//
// This file implements the 'raising' part of the LevelChange API.  This is
// useful because, in general, it makes the LLVM code terser and easier to
// analyze.  Note that it is good to run DCE after doing this transformation.
//
//  Eliminate silly things in the source that do not effect the level, but do
//  clean up the code:
//    * Casts of casts
//    - getelementptr/load & getelementptr/store are folded into a direct
//      load or store
//    - Convert this code (for both alloca and malloc):
//          %reg110 = shl uint %n, ubyte 2          ;;<uint>
//          %reg108 = alloca ubyte, uint %reg110            ;;<ubyte*>
//          %cast76 = cast ubyte* %reg108 to uint*          ;;<uint*>
//      To: %cast76 = alloca uint, uint %n
//   Convert explicit addressing to use getelementptr instruction where possible
//      - ...
//
//   Convert explicit addressing on pointers to use getelementptr instruction.
//    - If a pointer is used by arithmetic operation, insert an array casted
//      version into the source program, only for the following pointer types:
//        * Method argument pointers
//        - Pointers returned by alloca or malloc
//        - Pointers returned by function calls
//    - If a pointer is indexed with a value scaled by a constant size equal
//      to the element size of the array, the expression is replaced with a
//      getelementptr instruction.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/LevelChange.h"
#include "TransformInternals.h"
#include "llvm/Method.h"
#include "llvm/Support/STLExtras.h"
#include "llvm/iOther.h"
#include "llvm/iMemory.h"
#include "llvm/ConstPoolVals.h"
#include "llvm/Optimizations/ConstantHandling.h"
#include "llvm/Optimizations/DCE.h"
#include "llvm/Analysis/Expressions.h"
#include <algorithm>

#include "llvm/Assembly/Writer.h"

//#define DEBUG_PEEPHOLE_INSTS 1

#ifdef DEBUG_PEEPHOLE_INSTS
#define PRINT_PEEPHOLE(ID, NUM, I)            \
  cerr << "Inst P/H " << ID << "[" << NUM << "] " << I;
#else
#define PRINT_PEEPHOLE(ID, NUM, I)
#endif

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
  return !losslessCastableTypes(CI->getOperand(0)->getType(), CI->getType());
}




// DoInsertArrayCast - If the argument value has a pointer type, and if the
// argument value is used as an array, insert a cast before the specified 
// basic block iterator that casts the value to an array pointer.  Return the
// new cast instruction (in the CastResult var), or null if no cast is inserted.
//
static bool DoInsertArrayCast(Method *CurMeth, Value *V, BasicBlock *BB,
			      BasicBlock::iterator &InsertBefore,
			      CastInst *&CastResult) {
  const PointerType *ThePtrType = dyn_cast<PointerType>(V->getType());
  if (!ThePtrType) return false;
  bool InsertCast = false;

  for (Value::use_iterator I = V->use_begin(), E = V->use_end(); I != E; ++I) {
    Instruction *Inst = cast<Instruction>(*I);
    switch (Inst->getOpcode()) {
    default: break;                  // Not an interesting use...
    case Instruction::Add:           // It's being used as an array index!
  //case Instruction::Sub:
      InsertCast = true;
      break;
    case Instruction::Cast:          // There is already a cast instruction!
      if (const PointerType *PT = dyn_cast<const PointerType>(Inst->getType()))
	if (const ArrayType *AT = dyn_cast<const ArrayType>(PT->getValueType()))
	  if (AT->getElementType() == ThePtrType->getValueType()) {
	    // Cast already exists! Return the existing one!
	    CastResult = cast<CastInst>(Inst);
	    return false;       // No changes made to program though...
	  }
      break;
    }
  }

  if (!InsertCast) return false;  // There is no reason to insert a cast!

  // Insert a cast!
  const Type *ElTy = ThePtrType->getValueType();
  const PointerType *DestTy = PointerType::get(ArrayType::get(ElTy));

  CastResult = new CastInst(V, DestTy);
  BB->getInstList().insert(InsertBefore, CastResult);
  //cerr << "Inserted cast: " << CastResult;
  return true;            // Made a change!
}


// DoInsertArrayCasts - Loop over all "incoming" values in the specified method,
// inserting a cast for pointer values that are used as arrays. For our
// purposes, an incoming value is considered to be either a value that is 
// either a method parameter, a value created by alloca or malloc, or a value
// returned from a function call.  All casts are kept attached to their original
// values through the PtrCasts map.
//
static bool DoInsertArrayCasts(Method *M, map<Value*, CastInst*> &PtrCasts) {
  assert(!M->isExternal() && "Can't handle external methods!");

  // Insert casts for all arguments to the function...
  bool Changed = false;
  BasicBlock *CurBB = M->front();
  BasicBlock::iterator It = CurBB->begin();
  for (Method::ArgumentListType::iterator AI = M->getArgumentList().begin(), 
	 AE = M->getArgumentList().end(); AI != AE; ++AI) {
    CastInst *TheCast = 0;
    if (DoInsertArrayCast(M, *AI, CurBB, It, TheCast)) {
      It = CurBB->begin();      // We might have just invalidated the iterator!
      Changed = true;           // Yes we made a change
      ++It;                     // Insert next cast AFTER this one...
    }

    if (TheCast)                // Is there a cast associated with this value?
      PtrCasts[*AI] = TheCast;  // Yes, add it to the map...
  }

  // TODO: insert casts for alloca, malloc, and function call results.  Also, 
  // look for pointers that already have casts, to add to the map.

  return Changed;
}




// DoElminatePointerArithmetic - Loop over each incoming pointer variable,
// replacing indexing arithmetic with getelementptr calls.
//
static bool DoEliminatePointerArithmetic(const pair<Value*, CastInst*> &Val) {
  Value    *V  = Val.first;   // The original pointer
  CastInst *CV = Val.second;  // The array casted version of the pointer...

  for (Value::use_iterator I = V->use_begin(), E = V->use_end(); I != E; ++I) {
    Instruction *Inst = cast<Instruction>(*I);
    if (Inst->getOpcode() != Instruction::Add) 
      continue;   // We only care about add instructions

    BinaryOperator *Add = cast<BinaryOperator>(Inst);

    // Make sure the array is the first operand of the add expression...
    if (Add->getOperand(0) != V)
      Add->swapOperands();

    // Get the amount added to the pointer value...
    Value *AddAmount = Add->getOperand(1);

    
  }
  return false;
}


// Peephole Malloc instructions: we take a look at the use chain of the
// malloc instruction, and try to find out if the following conditions hold:
//   1. The malloc is of the form: 'malloc [sbyte], uint <constant>'
//   2. The only users of the malloc are cast & add instructions
//   3. Of the cast instructions, there is only one destination pointer type
//      [RTy] where the size of the pointed to object is equal to the number
//      of bytes allocated.
//
// If these conditions hold, we convert the malloc to allocate an [RTy]
// element.  This should be extended in the future to handle arrays. TODO
//
static bool PeepholeMallocInst(BasicBlock *BB, BasicBlock::iterator &BI) {
  MallocInst *MI = cast<MallocInst>(*BI);
  if (!MI->isArrayAllocation()) return false;    // No array allocation?

  ConstPoolUInt *Amt = dyn_cast<ConstPoolUInt>(MI->getArraySize());
  if (Amt == 0 || MI->getAllocatedType() != ArrayType::get(Type::SByteTy))
    return false;

  // Get the number of bytes allocated...
  unsigned Size = Amt->getValue();
  const Type *ResultTy = 0;

  // Loop over all of the uses of the malloc instruction, inspecting casts.
  for (Value::use_iterator I = MI->use_begin(), E = MI->use_end();
       I != E; ++I) {
    if (CastInst *CI = dyn_cast<CastInst>(*I)) {
        //cerr << "\t" << CI;
    
      // We only work on casts to pointer types for sure, be conservative
      if (!isa<PointerType>(CI->getType())) {
        cerr << "Found cast of malloc value to non pointer type:\n" << CI;
        return false;
      }

      const Type *DestTy = cast<PointerType>(CI->getType())->getValueType();
      if (isa<ArrayType>(DestTy)) {
        cerr << "Avoided malloc conversion because of type: " << DestTy
             << " TODO.\n";
        return false;
      }
      if (TD.getTypeSize(DestTy) == Size && DestTy != ResultTy) {
        // Does the size of the allocated type match the number of bytes
        // allocated?
        //
        if (ResultTy == 0) {
          ResultTy = DestTy;   // Keep note of this for future uses...
        } else {
          // It's overdefined!  We don't know which type to convert to!
          return false;
        }
      }
    }
  }

  // If we get this far, we have either found, or not, a type that is cast to
  // that is of the same size as the malloc instruction.
  if (!ResultTy) return false;

  // Now we check to see if we can convert the return value of malloc to the
  // specified pointer type.  All this is moot if we can't.
  //
  ValueTypeCache ConvertedTypes;
  if (RetValConvertableToType(MI, PointerType::get(ResultTy), ConvertedTypes)) {
    // Yup, it's convertable, do the transformation now!
    PRINT_PEEPHOLE1("mall-refine:in ", MI);

    // Create a new malloc instruction, and insert it into the method...
    MallocInst *NewMI = new MallocInst(PointerType::get(ResultTy));
    NewMI->setName(MI->getName());
    MI->setName("");
    BI = BB->getInstList().insert(BI, NewMI)+1;

    // Create a new cast instruction to cast it to the old type...
    CastInst *NewCI = new CastInst(NewMI, MI->getType());
    BB->getInstList().insert(BI, NewCI);

    // Move all users of the old malloc instruction over to use the new cast...
    MI->replaceAllUsesWith(NewCI);

    ValueMapCache ValueMap;
    ConvertUsersType(NewCI, NewMI, ValueMap);  // This will delete MI!
        
    BI = BB->begin();  // Rescan basic block.  BI might be invalidated.
    PRINT_PEEPHOLE1("mall-refine:out", NewMI);
    return true;
  }
  return false;
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
  Value            *OffsetVal = AddOp2->getOperand(0);
  Value            *SrcPtr;  // Of type pointer to struct...
  const StructType *StructTy;

  if ((StructTy = getPointedToStruct(AddOp1->getType()))) {
    SrcPtr = AddOp1;                      // Handle the first case...
  } else if (CastInst *AddOp1c = dyn_cast<CastInst>(AddOp1)) {
    SrcPtr = AddOp1c->getOperand(0);      // Handle the second case...
    StructTy = getPointedToStruct(SrcPtr->getType());
  }

  // Only proceed if we have detected all of our conditions successfully...
  if (!StructTy || !SrcPtr || !OffsetVal->getType()->isIntegral())
    return false;

  // See if the cast is of an integer expression that is either a constant,
  // or a value scaled by some amount with a possible offset.
  //
  analysis::ExprType Expr = analysis::ClassifyExpression(OffsetVal);
  unsigned         Offset = 0, Scale = 1;

  // The expression must either be a constant, or a scaled index to be useful
  if (!Expr.Offset && !Expr.Scale)
    return false;

  // Get the offset value if it exists...
  if (Expr.Offset) {
    if (ConstPoolSInt *CPSI = dyn_cast<ConstPoolSInt>(Expr.Offset))
      Offset = (unsigned)CPSI->getValue();
    else {
      ConstPoolUInt *CPUI = cast<ConstPoolUInt>(Expr.Offset);
      Offset = (unsigned)CPUI->getValue();
    }
    assert(Offset != 0 && "Expression analysis failure!");
  }

  // Get the scale value if it exists...
  if (Expr.Scale) {
    if (ConstPoolSInt *CPSI = dyn_cast<ConstPoolSInt>(Expr.Scale))
      Scale = (unsigned)CPSI->getValue();
    else {
      ConstPoolUInt *CPUI = cast<ConstPoolUInt>(Expr.Scale);
      Scale = (unsigned)CPUI->getValue();
    }
    assert(Scale != 1 && "Expression analysis failure!");
  }
  
  // Check to make sure the offset is not negative or really large, outside the
  // scope of this structure...
  //
  if (Offset >= TD.getTypeSize(StructTy))
    return false;

  const StructLayout *SL = TD.getStructLayout(StructTy);
  vector<ConstPoolVal*> Offsets;
  unsigned ActualOffset = Offset;
  const Type *ElTy = getStructOffsetType(StructTy, ActualOffset, Offsets);
  
  if (getPointedToStruct(AddOp1->getType())) {  // case 1
    PRINT_PEEPHOLE2("add-to-gep1:in", AddOp2, *BI);
  } else {
    PRINT_PEEPHOLE3("add-to-gep2:in", AddOp1, AddOp2, *BI);
  }

  GetElementPtrInst *GEP = new GetElementPtrInst(SrcPtr, Offsets);
  //AddOp2->getName());
  BI = BB->getInstList().insert(BI, GEP)+1;
  
  Instruction *AddrSrc = GEP;
  
  if (const ArrayType *AT = dyn_cast<ArrayType>(ElTy)) {
    assert((Scale == 1 || Offset == ActualOffset) &&
           "Cannot handle scaled expression and unused offset in the same "
           "instruction until after GEP array works!");

    // Check to see if we have bottomed out INSIDE of an array reference..
    //
    if (Offset != ActualOffset) {
      // Insert a cast of the "rest" of the offset to the appropriate
      // pointer type.
      CastInst *OffInst =
        new CastInst(ConstPoolUInt::get(Type::ULongTy, 
                                        Offset-ActualOffset),
                     GEP->getType());
      BI = BB->getInstList().insert(BI, OffInst)+1;
      
      // Now insert an ADD to actually adjust the pointer...
      Instruction *AddInst =
        BinaryOperator::create(Instruction::Add, GEP, OffInst);
      BI = BB->getInstList().insert(BI, AddInst)+1;

      PRINT_PEEPHOLE2("add-to-gep:out1", OffInst, AddInst);
      
      AddrSrc = AddInst;
    } else if (Scale != 1) {
      // If the scale factor occurs, then this means that there is an index into
      // this element of the array.  Check to make sure the scale factor is the
      // same as the size of the datatype that we are dealing with.
      //
      assert(Scale == TD.getTypeSize(AT->getElementType()) && 
             "Scaling by something other than the array element size!!");
      
      // TODO: In the future, we will not want to cast the index and scale to
      // pointer types first.  We will want to create a GEP directly here.

      // Now we must actually perform the scaling operation to get an
      // appropriate value to add in... but the scale has to be done in the
      // appropriate destination pointer type, so cast the index value now.
      //
      // Cast the base index pointer
      CastInst *IdxValue = new CastInst(Expr.Var, GEP->getType());
      BI = BB->getInstList().insert(BI, IdxValue)+1;

      // Case the scale amount as well...
      CastInst *ScaleAmt =
        new CastInst(ConstPoolUInt::get(Type::ULongTy, Scale), GEP->getType());
      BI = BB->getInstList().insert(BI, ScaleAmt)+1;

      // Insert the multiply now.  Make sure to make the constant the second arg
      Instruction *ScaledVal =
        BinaryOperator::create(Instruction::Mul, IdxValue, ScaleAmt);
      BI = BB->getInstList().insert(BI, ScaledVal)+1;

      // Now insert an ADD to actually adjust the pointer...
      Instruction *AddInst =
        BinaryOperator::create(Instruction::Add, GEP, ScaledVal);
      BI = BB->getInstList().insert(BI, AddInst)+1;

      PRINT_PEEPHOLE4("add-to-gep:out1", IdxValue, ScaleAmt, ScaledVal, 
                      AddInst);
      AddrSrc = AddInst;
    }
    
    // Insert a cast of the pointer to array of X to be a pointer to the
    // element of the array.
    //
    // Insert a cast of the "rest" of the offset to the appropriate
    // pointer type.
    CastInst *ACI = new CastInst(AddrSrc, AT->getElementType());
    BI = BB->getInstList().insert(BI, ACI)+1;
    AddrSrc = ACI;
    
  } else {
    assert(Offset == ActualOffset && "GEP to middle of non array!");
    assert(Scale == 1 && "Scale factor for expr that is not an array idx!");
  }
  
  Instruction *NCI = new CastInst(AddrSrc, AddOp1->getType());
  ReplaceInstWithInst(BB->getInstList(), BI, NCI);
  PRINT_PEEPHOLE2("add-to-gep:out", GEP, NCI);
  return true;
}

// Peephole optimize the following instructions:
//   %t1 = cast int (uint) * %reg111 to uint (...) *
//   %t2 = call uint (...) * %cast111( uint %key )
//
// Into: %t3 = call int (uint) * %reg111( uint %key )
//       %t2 = cast int %t3 to uint
//
static bool PeepholeCallInst(BasicBlock *BB, BasicBlock::iterator &BI) {
  CallInst *CI = cast<CallInst>(*BI);
  return false;
}


static bool PeepholeOptimize(BasicBlock *BB, BasicBlock::iterator &BI) {
  Instruction *I = *BI;

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
        string Name = CI->getName();
        CI->setName("");
        Src->setName(Name, BB->getParent()->getSymbolTable());
      }
      return true;
    }

    // Peephole optimize the following instructions:
    // %tmp = cast <ty> %V to <ty2>
    // %V  = cast <ty2> %tmp to <ty3>     ; Where ty & ty2 are same size
    //
    // Into: cast <ty> %V to <ty3>
    //
    if (SrcI)
      if (CastInst *CSrc = dyn_cast<CastInst>(SrcI))
        if (isReinterpretingCast(CI) + isReinterpretingCast(CSrc) < 2) {
          // We can only do c-c elimination if, at most, one cast does a
          // reinterpretation of the input data.
          //
          // If legal, make this cast refer the the original casts argument!
          //
          PRINT_PEEPHOLE2("cast-cast:in ", CI, CSrc);
          CI->setOperand(0, CSrc->getOperand(0));
          PRINT_PEEPHOLE1("cast-cast:out", CI);
          return true;
        }

    // Check to see if it's a cast of an instruction that does not depend on the
    // specific type of the operands to do it's job.
    if (!isReinterpretingCast(CI)) {
      ValueTypeCache ConvertedTypes;
      if (RetValConvertableToType(CI, Src->getType(), ConvertedTypes)) {
        PRINT_PEEPHOLE2("CAST-DEST-EXPR-CONV:in ", CI, Src);

#ifdef DEBUG_PEEPHOLE_INSTS
        cerr << "\nCONVERTING EXPR TYPE:\n";
#endif
        ValueMapCache ValueMap;
        ConvertUsersType(CI, Src, ValueMap);  // This will delete CI!

        BI = BB->begin();  // Rescan basic block.  BI might be invalidated.
        PRINT_PEEPHOLE1("CAST-DEST-EXPR-CONV:out", Src);
#ifdef DEBUG_PEEPHOLE_INSTS
        cerr << "DONE CONVERTING EXPR TYPE: \n\n";// << BB->getParent();
#endif
        return true;
      } else {
        ConvertedTypes.clear();
        if (ExpressionConvertableToType(Src, DestTy, ConvertedTypes)) {
          PRINT_PEEPHOLE2("CAST-SRC-EXPR-CONV:in ", CI, Src);
          
#ifdef DEBUG_PEEPHOLE_INSTS
          cerr << "\nCONVERTING SRC EXPR TYPE:\n";
#endif
          ValueMapCache ValueMap;
          Value *E = ConvertExpressionToType(Src, DestTy, ValueMap);
          if (ConstPoolVal *CPV = dyn_cast<ConstPoolVal>(E))
            CI->replaceAllUsesWith(CPV);

          BI = BB->begin();  // Rescan basic block.  BI might be invalidated.
          PRINT_PEEPHOLE1("CAST-SRC-EXPR-CONV:out", E);
#ifdef DEBUG_PEEPHOLE_INSTS
          cerr << "DONE CONVERTING SRC EXPR TYPE: \n\n";// << BB->getParent();
#endif
          return true;
        }
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
#if 1
    if (const StructType *STy = getPointedToStruct(Src->getType()))
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
          const Type *DestPointedTy = DestPTy->getValueType();
          unsigned Depth = 1;
          const StructType *CurSTy = STy;
          const Type *ElTy = 0;
          while (CurSTy) {
            
            // Check for a zero element struct type... if we have one, bail.
            if (CurSTy->getElementTypes().size() == 0) break;
            
            // Grab the first element of the struct type, which must lie at
            // offset zero in the struct.
            //
            ElTy = CurSTy->getElementTypes()[0];

            // Did we find what we're looking for?
            if (losslessCastableTypes(ElTy, DestPointedTy)) break;
            
            // Nope, go a level deeper.
            ++Depth;
            CurSTy = dyn_cast<StructType>(ElTy);
            ElTy = 0;
          }
          
          // Did we find what we were looking for? If so, do the transformation
          if (ElTy) {
            PRINT_PEEPHOLE1("cast-for-first:in", CI);

            // Build the index vector, full of all zeros
            vector<ConstPoolVal *> Indices(Depth,
                                           ConstPoolUInt::get(Type::UByteTy,0));

            // Insert the new T cast instruction... stealing old T's name
            GetElementPtrInst *GEP = new GetElementPtrInst(Src, Indices,
                                                           CI->getName());
            CI->setName("");
            BI = BB->getInstList().insert(BI, GEP)+1;

            // Make the old cast instruction reference the new GEP instead of
            // the old src value.
            //
            CI->setOperand(0, GEP);
            
            PRINT_PEEPHOLE2("cast-for-first:out", GEP, CI);
            return true;
          }
        }
      }
#endif

#if 1
  } else if (MallocInst *MI = dyn_cast<MallocInst>(I)) {
    if (PeepholeMallocInst(BB, BI)) return true;

  } else if (CallInst *CI = dyn_cast<CallInst>(I)) {
    if (PeepholeCallInst(BB, BI)) return true;

  } else if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
    Value *Val     = SI->getOperand(0);
    Value *Pointer = SI->getPointerOperand();
    
    // Peephole optimize the following instructions:
    // %t1 = getelementptr {<...>} * %StructPtr, <element indices>
    // store <elementty> %v, <elementty> * %t1
    //
    // Into: store <elementty> %v, {<...>} * %StructPtr, <element indices>
    //
    if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(Pointer)) {
      // Append any indices that the store instruction has onto the end of the
      // ones that the GEP is carrying...
      //
      vector<ConstPoolVal*> Indices(GEP->getIndices());
      Indices.insert(Indices.end(), SI->getIndices().begin(),
                     SI->getIndices().end());

      PRINT_PEEPHOLE2("gep-store:in", GEP, SI);
      ReplaceInstWithInst(BB->getInstList(), BI,
                          SI = new StoreInst(Val, GEP->getPointerOperand(),
                                             Indices));
      PRINT_PEEPHOLE1("gep-store:out", SI);
      return true;
    }
    
    // Peephole optimize the following instructions:
    // %t = cast <T1>* %P to <T2> * ;; If T1 is losslessly convertable to T2
    // store <T2> %V, <T2>* %t
    //
    // Into: 
    // %t = cast <T2> %V to <T1>
    // store <T1> %t2, <T1>* %P
    //
    if (CastInst *CI = dyn_cast<CastInst>(Pointer))
      if (Value *CastSrc = CI->getOperand(0)) // CSPT = CastSrcPointerType
        if (PointerType *CSPT = dyn_cast<PointerType>(CastSrc->getType()))
          if (losslessCastableTypes(Val->getType(), // convertable types!
                                    CSPT->getValueType()) &&
              !SI->hasIndices()) {      // No subscripts yet!
            PRINT_PEEPHOLE3("st-src-cast:in ", Pointer, Val, SI);

            // Insert the new T cast instruction... stealing old T's name
            CastInst *NCI = new CastInst(Val, CSPT->getValueType(),
                                         CI->getName());
            CI->setName("");
            BI = BB->getInstList().insert(BI, NCI)+1;

            // Replace the old store with a new one!
            ReplaceInstWithInst(BB->getInstList(), BI,
                                SI = new StoreInst(NCI, CastSrc));
            PRINT_PEEPHOLE3("st-src-cast:out", NCI, CastSrc, SI);
            return true;
          }


  } else if (LoadInst *LI = dyn_cast<LoadInst>(I)) {
    Value *Pointer = LI->getPointerOperand();
    
    // Peephole optimize the following instructions:
    // %t1 = getelementptr {<...>} * %StructPtr, <element indices>
    // %V  = load <elementty> * %t1
    //
    // Into: load {<...>} * %StructPtr, <element indices>
    //
    if (GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(Pointer)) {
      // Append any indices that the load instruction has onto the end of the
      // ones that the GEP is carrying...
      //
      vector<ConstPoolVal*> Indices(GEP->getIndices());
      Indices.insert(Indices.end(), LI->getIndices().begin(),
                     LI->getIndices().end());

      PRINT_PEEPHOLE2("gep-load:in", GEP, LI);
      ReplaceInstWithInst(BB->getInstList(), BI,
                          LI = new LoadInst(GEP->getPointerOperand(),
                                            Indices));
      PRINT_PEEPHOLE1("gep-load:out", LI);
      return true;
    }


    // Peephole optimize the following instructions:
    // %t1 = cast <ty> * %t0 to <ty2> *
    // %V  = load <ty2> * %t1
    //
    // Into: %t1 = load <ty> * %t0
    //       %V  = cast <ty> %t1 to <ty2>
    //
    // The idea behind this transformation is that if the expression type
    // conversion engine could not convert the cast into some other nice form,
    // that there is something fundementally wrong with the current shape of
    // the program.  Move the cast through the load and try again.  This will
    // leave the original cast instruction, to presumably become dead.
    //
    if (CastInst *CI = dyn_cast<CastInst>(Pointer)) {
      Value *SrcVal = CI->getOperand(0);
      const PointerType *SrcTy = dyn_cast<PointerType>(SrcVal->getType());
      const Type *ElTy = SrcTy ? SrcTy->getValueType() : 0;

      // Make sure that nothing will be lost in the new cast...
      if (SrcTy && losslessCastableTypes(ElTy, LI->getType())) {
        PRINT_PEEPHOLE2("CL-LoadCast:in ", CI, LI);

        string CName = CI->getName(); CI->setName("");
        LoadInst *NLI = new LoadInst(SrcVal, LI->getName());
        LI->setName("");  // Take over the old load's name

        // Insert the load before the old load
        BI = BB->getInstList().insert(BI, NLI)+1;

        // Replace the old load with a new cast...
        ReplaceInstWithInst(BB->getInstList(), BI, 
                            CI = new CastInst(NLI, LI->getType(), CName));
        PRINT_PEEPHOLE2("CL-LoadCast:out", NLI, CI);

        return true;
      }
    }
  } else if (I->getOpcode() == Instruction::Add &&
             isa<CastInst>(I->getOperand(1))) {

    if (PeepholeOptimizeAddCast(BB, BI, I->getOperand(0),
                                cast<CastInst>(I->getOperand(1))))
      return true;

#endif
  }

  return false;
}




static bool DoRaisePass(Method *M) {
  bool Changed = false;
  for (Method::iterator MI = M->begin(), ME = M->end(); MI != ME; ++MI) {
    BasicBlock *BB = *MI;
    BasicBlock::InstListType &BIL = BB->getInstList();

    for (BasicBlock::iterator BI = BB->begin(); BI != BB->end();) {
      if (opt::DeadCodeElimination::dceInstruction(BIL, BI)) {
        Changed = true; 
#ifdef DEBUG_PEEPHOLE_INSTS
        cerr << "DeadCode Elinated!\n";
#endif
      } else if (PeepholeOptimize(BB, BI))
        Changed = true;
      else
        ++BI;
    }
  }
  return Changed;
}


// RaisePointerReferences::doit - Raise a method representation to a higher
// level.
//
bool RaisePointerReferences::doit(Method *M) {
  if (M->isExternal()) return false;
  bool Changed = false;

#ifdef DEBUG_PEEPHOLE_INSTS
  cerr << "\n\n\nStarting to work on Method '" << M->getName() << "'\n";
#endif

  while (DoRaisePass(M)) Changed = true;

#if 0
  // PtrCasts - Keep a mapping between the pointer values (the key of the 
  // map), and the cast to array pointer (the value) in this map.  This is
  // used when converting pointer math into array addressing.
  // 
  map<Value*, CastInst*> PtrCasts;

  // Insert casts for all incoming pointer values.  Keep track of those casts
  // and the identified incoming values in the PtrCasts map.
  //
  Changed |= DoInsertArrayCasts(M, PtrCasts);

  // Loop over each incoming pointer variable, replacing indexing arithmetic
  // with getelementptr calls.
  //
  Changed |= reduce_apply_bool(PtrCasts.begin(), PtrCasts.end(), 
                               ptr_fun(DoEliminatePointerArithmetic));
#endif

  return Changed;
}
