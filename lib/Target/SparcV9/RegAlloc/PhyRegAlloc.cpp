#include "llvm/CodeGen/PhyRegAlloc.h"

//***TODO: There are several places we add instructions. Validate the order
//         of adding these instructions.



cl::Enum<RegAllocDebugLevel_t> DEBUG_RA("dregalloc", cl::NoFlags,
  "enable register allocation debugging information",
  clEnumValN(RA_DEBUG_None   , "n", "disable debug output"),
  clEnumValN(RA_DEBUG_Normal , "y", "enable debug output"),
  clEnumValN(RA_DEBUG_Verbose, "v", "enable extra debug output"), 0);


//----------------------------------------------------------------------------
// Constructor: Init local composite objects and create register classes.
//----------------------------------------------------------------------------
PhyRegAlloc::PhyRegAlloc(const Method *const M, 
			 const TargetMachine& tm, 
			 MethodLiveVarInfo *const Lvi) 
                        : RegClassList(),
			  Meth(M), TM(tm), LVI(Lvi), LRI(M, tm, RegClassList), 
			  MRI( tm.getRegInfo() ),
                          NumOfRegClasses(MRI.getNumOfRegClasses()),
			  AddedInstrMap(), StackOffsets()

{
  // **TODO: use an actual reserved color list 
  ReservedColorListType *RCL = new ReservedColorListType();

  // create each RegisterClass and put in RegClassList
  for( unsigned int rc=0; rc < NumOfRegClasses; rc++)  
    RegClassList.push_back( new RegClass(M, MRI.getMachineRegClass(rc), RCL) );

  // **TODO: Init to the correct value. Also reset this to the correct
  // value at the start of each instruction. Need a way to track max used
  int curOffset4TmpSpills =0 ; 
}

//----------------------------------------------------------------------------
// This method initally creates interference graphs (one in each reg class)
// and IGNodeList (one in each IG). The actual nodes will be pushed later. 
//----------------------------------------------------------------------------

void PhyRegAlloc::createIGNodeListsAndIGs()
{
  if(DEBUG_RA ) cout << "Creating LR lists ..." << endl;

  // hash map iterator
  LiveRangeMapType::const_iterator HMI = (LRI.getLiveRangeMap())->begin();   

  // hash map end
  LiveRangeMapType::const_iterator HMIEnd = (LRI.getLiveRangeMap())->end();   

    for(  ; HMI != HMIEnd ; ++HMI ) {
      
      if( (*HMI).first ) { 

	LiveRange *L = (*HMI).second;      // get the LiveRange

	if( !L) { 
	  if( DEBUG_RA) {
	    cout << "\n*?!?Warning: Null liver range found for: ";
	    printValue( (*HMI).first) ; cout << endl;
	  }
	  continue;
	}
                                        // if the Value * is not null, and LR  
                                        // is not yet written to the IGNodeList
       if( !(L->getUserIGNode())  ) {  
	                           
	 RegClass *const RC =           // RegClass of first value in the LR
	   //RegClassList [MRI.getRegClassIDOfValue(*(L->begin()))];
	   RegClassList[ L->getRegClass()->getID() ];

	 RC-> addLRToIG( L );           // add this LR to an IG
       }
    }
  }

                                        // init RegClassList
  for( unsigned int rc=0; rc < NumOfRegClasses ; rc++)  
    RegClassList[ rc ]->createInterferenceGraph();

  if( DEBUG_RA)
    cout << "LRLists Created!" << endl;
}



//----------------------------------------------------------------------------
// This method will add all interferences at for a given instruction.
// Interence occurs only if the LR of Def (Inst or Arg) is of the same reg 
// class as that of live var. The live var passed to this function is the 
// LVset AFTER the instruction
//----------------------------------------------------------------------------

void PhyRegAlloc::addInterference(const Value *const Def, 
				  const LiveVarSet *const LVSet,
				  const bool isCallInst) {

  LiveVarSet::const_iterator LIt = LVSet->begin();

  // get the live range of instruction
  const LiveRange *const LROfDef = LRI.getLiveRangeForValue( Def );   

  IGNode *const IGNodeOfDef = LROfDef->getUserIGNode();
  assert( IGNodeOfDef );

  RegClass *const RCOfDef = LROfDef->getRegClass(); 

  // for each live var in live variable set
  for( ; LIt != LVSet->end(); ++LIt) {

    if( DEBUG_RA > 1) {
      cout << "< Def="; printValue(Def);     
      cout << ", Lvar=";  printValue( *LIt); cout  << "> ";
    }

    //  get the live range corresponding to live var
    LiveRange *const LROfVar = LRI.getLiveRangeForValue(*LIt );    

    // LROfVar can be null if it is a const since a const 
    // doesn't have a dominating def - see Assumptions above
    if( LROfVar)   {  

      if(LROfDef == LROfVar)            // do not set interf for same LR
	continue;

      // if 2 reg classes are the same set interference
      if( RCOfDef == LROfVar->getRegClass() ){ 
	RCOfDef->setInterference( LROfDef, LROfVar);  

      }

    else if(DEBUG_RA > 1)  { 
      // we will not have LRs for values not explicitly allocated in the
      // instruction stream (e.g., constants)
      cout << " warning: no live range for " ; 
      printValue( *LIt); cout << endl; }
    
    }
 
  }

}


//----------------------------------------------------------------------------
// For a call instruction, this method sets the CallInterference flag in 
// the LR of each variable live int the Live Variable Set live after the
// call instruction (except the return value of the call instruction - since
// the return value does not interfere with that call itself).
//----------------------------------------------------------------------------

void PhyRegAlloc::setCallInterferences(const MachineInstr *MInst, 
				       const LiveVarSet *const LVSetAft ) 
{
  // Now find the LR of the return value of the call


  // We do this because, we look at the LV set *after* the instruction
  // to determine, which LRs must be saved across calls. The return value
  // of the call is live in this set - but it does not interfere with call
  // (i.e., we can allocate a volatile register to the return value)

  LiveRange *RetValLR = NULL;

  const Value *RetVal = MRI.getCallInstRetVal( MInst );

  if( RetVal ) {
    RetValLR = LRI.getLiveRangeForValue( RetVal );
    assert( RetValLR && "No LR for RetValue of call");
  }

  if( DEBUG_RA)
    cout << "\n For call inst: " << *MInst;

  LiveVarSet::const_iterator LIt = LVSetAft->begin();

  // for each live var in live variable set after machine inst
  for( ; LIt != LVSetAft->end(); ++LIt) {

   //  get the live range corresponding to live var
    LiveRange *const LR = LRI.getLiveRangeForValue(*LIt ); 

    if( LR && DEBUG_RA) {
      cout << "\n\tLR Aft Call: ";
      LR->printSet();
    }
   

    // LR can be null if it is a const since a const 
    // doesn't have a dominating def - see Assumptions above
    if( LR && (LR != RetValLR) )   {  
      LR->setCallInterference();
      if( DEBUG_RA) {
	cout << "\n  ++Added call interf for LR: " ;
	LR->printSet();
      }
    }

  }

}


//----------------------------------------------------------------------------
// This method will walk thru code and create interferences in the IG of
// each RegClass.
//----------------------------------------------------------------------------

void PhyRegAlloc::buildInterferenceGraphs()
{

  if(DEBUG_RA) cout << "Creating interference graphs ..." << endl;

  Method::const_iterator BBI = Meth->begin();  // random iterator for BBs   

  for( ; BBI != Meth->end(); ++BBI) {          // traverse BBs in random order

    // get the iterator for machine instructions
    const MachineCodeForBasicBlock& MIVec = (*BBI)->getMachineInstrVec();
    MachineCodeForBasicBlock::const_iterator 
      MInstIterator = MIVec.begin();

    // iterate over all the machine instructions in BB
    for( ; MInstIterator != MIVec.end(); ++MInstIterator) {  

      const MachineInstr *const MInst = *MInstIterator; 

      // get the LV set after the instruction
      const LiveVarSet *const LVSetAI = 
	LVI->getLiveVarSetAfterMInst(MInst, *BBI);
    
      const bool isCallInst = TM.getInstrInfo().isCall(MInst->getOpCode());

      if( isCallInst ) {
	//cout << "\nFor call inst: " << *MInst;

	// set the isCallInterference flag of each live range wich extends
	// accross this call instruction. This information is used by graph
	// coloring algo to avoid allocating volatile colors to live ranges
	// that span across calls (since they have to be saved/restored)
	setCallInterferences( MInst,  LVSetAI);
      }


      // iterate over  MI operands to find defs
      for( MachineInstr::val_op_const_iterator OpI(MInst);!OpI.done(); ++OpI) {
	
	if( OpI.isDef() ) {     
	  // create a new LR iff this operand is a def
	  addInterference(*OpI, LVSetAI, isCallInst );

	} //if this is a def

      } // for all operands


      // Also add interference for any implicit definitions in a machine
      // instr (currently, only calls have this).

      unsigned NumOfImpRefs =  MInst->getNumImplicitRefs();
      if(  NumOfImpRefs > 0 ) {
	for(unsigned z=0; z < NumOfImpRefs; z++) 
	  if( MInst->implicitRefIsDefined(z) )
	    addInterference( MInst->getImplicitRef(z), LVSetAI, isCallInst );
      }

    } // for all machine instructions in BB
    
  } // for all BBs in method


  // add interferences for method arguments. Since there are no explict 
  // defs in method for args, we have to add them manually
          
  addInterferencesForArgs();            // add interference for method args

  if( DEBUG_RA)
    cout << "Interference graphs calculted!" << endl;

}




//----------------------------------------------------------------------------
// This method will add interferences for incoming arguments to a method.
//----------------------------------------------------------------------------
void PhyRegAlloc::addInterferencesForArgs()
{
                                              // get the InSet of root BB
  const LiveVarSet *const InSet = LVI->getInSetOfBB( Meth->front() );  

                                              // get the argument list
  const Method::ArgumentListType& ArgList = Meth->getArgumentList();  

                                              // get an iterator to arg list
  Method::ArgumentListType::const_iterator ArgIt = ArgList.begin();          


  for( ; ArgIt != ArgList.end() ; ++ArgIt) {  // for each argument
    addInterference( *ArgIt, InSet, false );  // add interferences between 
                                              // args and LVars at start
    if( DEBUG_RA > 1) {
       cout << " - %% adding interference for  argument ";    
      printValue( (const Value *) *ArgIt); cout  << endl;
    }
  }
}



#if 0

//----------------------------------------------------------------------------
// This method inserts caller saving/restoring instructons before/after
// a call machine instruction.
//----------------------------------------------------------------------------


void PhyRegAlloc::insertCallerSavingCode(const MachineInstr *MInst, 
					 const BasicBlock *BB  ) 
{
  // assert( (TM.getInstrInfo()).isCall( MInst->getOpCode() ) );

  StackOffsets.resetTmpPos();

  hash_set<unsigned> PushedRegSet;

  // Now find the LR of the return value of the call
  // The last *implicit operand* is the return value of a call
  // Insert it to to he PushedRegSet since we must not save that register
  // and restore it after the call.
  // We do this because, we look at the LV set *after* the instruction
  // to determine, which LRs must be saved across calls. The return value
  // of the call is live in this set - but we must not save/restore it.


  const Value *RetVal = MRI.getCallInstRetVal( MInst );

  if( RetVal ) {

    LiveRange *RetValLR = LRI.getLiveRangeForValue( RetVal );
    assert( RetValLR && "No LR for RetValue of call");

    PushedRegSet.insert(
		 MRI.getUnifiedRegNum((RetValLR->getRegClass())->getID(), 
				      RetValLR->getColor() ) );
  }


  const LiveVarSet *LVSetAft =  LVI->getLiveVarSetAfterMInst(MInst, BB);

  LiveVarSet::const_iterator LIt = LVSetAft->begin();

  // for each live var in live variable set after machine inst
  for( ; LIt != LVSetAft->end(); ++LIt) {

   //  get the live range corresponding to live var
    LiveRange *const LR = LRI.getLiveRangeForValue(*LIt );    

    // LR can be null if it is a const since a const 
    // doesn't have a dominating def - see Assumptions above
    if( LR )   {  
      
      if( LR->hasColor() ) {

	unsigned RCID = (LR->getRegClass())->getID();
	unsigned Color = LR->getColor();

	if ( MRI.isRegVolatile(RCID, Color) ) {

	  // if the value is in both LV sets (i.e., live before and after 
	  // the call machine instruction)

	  unsigned Reg =   MRI.getUnifiedRegNum(RCID, Color);
	  
	  if( PushedRegSet.find(Reg) == PushedRegSet.end() ) {
	    
	    // if we haven't already pushed that register

	    unsigned RegType = MRI.getRegType( LR );

	    // Now get two instructions - to push on stack and pop from stack
	    // and add them to InstrnsBefore and InstrnsAfter of the
	    // call instruction

	    int StackOff =  StackOffsets.getNewTmpPosOffFromSP();

	    /**** TODO
		  
	    if( RegType == SaveViaIntReg) {

	      int FreeIntReg = getFreedIntReg(......)


	    }
	    */
	    
	    MachineInstr *AdIBef = 
	      MRI.cpReg2MemMI(Reg, MRI.getStackPointer(), StackOff, RegType ); 

	    MachineInstr *AdIAft = 
	      MRI.cpMem2RegMI(MRI.getStackPointer(), StackOff, Reg, RegType ); 

	    ((AddedInstrMap[MInst])->InstrnsBefore).push_front(AdIBef);
	    ((AddedInstrMap[MInst])->InstrnsAfter).push_back(AdIAft);
	    
	    PushedRegSet.insert( Reg );

	    if(DEBUG_RA) {
	      cerr << "\nFor callee save call inst:" << *MInst;
	      cerr << "\n  -inserted caller saving instrs:\n\t ";
	      cerr << *AdIBef << "\n\t" << *AdIAft  ;
	    }	    
	  } // if not already pushed

	} // if LR has a volatile color
	
      } // if LR has color

    } // if there is a LR for Var
    
  } // for each value in the LV set after instruction
  
}

#endif


//----------------------------------------------------------------------------
// This method is called after register allocation is complete to set the
// allocated reisters in the machine code. This code will add register numbers
// to MachineOperands that contain a Value.
//----------------------------------------------------------------------------

void PhyRegAlloc::updateMachineCode()
{

  Method::const_iterator BBI = Meth->begin();  // random iterator for BBs   

  for( ; BBI != Meth->end(); ++BBI) {          // traverse BBs in random order

    // get the iterator for machine instructions
    MachineCodeForBasicBlock& MIVec = (*BBI)->getMachineInstrVec();
    MachineCodeForBasicBlock::iterator MInstIterator = MIVec.begin();

    // iterate over all the machine instructions in BB
    for( ; MInstIterator != MIVec.end(); ++MInstIterator) {  
      
      MachineInstr *MInst = *MInstIterator; 

      // if this machine instr is call, insert caller saving code

      if( (TM.getInstrInfo()).isCall( MInst->getOpCode()) )
	MRI.insertCallerSavingCode(MInst,  *BBI, *this );

      // If there are instructions to be added, *before* this machine
      // instruction, add them now.
      
      if( AddedInstrMap[ MInst ] ) {

	deque<MachineInstr *> &IBef = (AddedInstrMap[MInst])->InstrnsBefore;

	if( ! IBef.empty() ) {

	  deque<MachineInstr *>::iterator AdIt; 

	  for( AdIt = IBef.begin(); AdIt != IBef.end() ; ++AdIt ) {

	    if( DEBUG_RA)
	      cerr << " *$* PREPENDed instr " << *AdIt << endl;
	  	    
	    MInstIterator = MIVec.insert( MInstIterator, *AdIt );
	    ++MInstIterator;
	  }

	}

      }

      // reset the stack offset for temporary variables since we may
      // need that to spill
      StackOffsets.resetTmpPos();

      //for(MachineInstr::val_op_const_iterator OpI(MInst);!OpI.done();++OpI) {

      for(unsigned OpNum=0; OpNum < MInst->getNumOperands(); ++OpNum) {

	MachineOperand& Op = MInst->getOperand(OpNum);

	if( Op.getOperandType() ==  MachineOperand::MO_VirtualRegister || 
	    Op.getOperandType() ==  MachineOperand::MO_CCRegister) {

	  const Value *const Val =  Op.getVRegValue();

	  // delete this condition checking later (must assert if Val is null)
	  if( !Val) {
            if (DEBUG_RA)
              cout << "Warning: NULL Value found for operand" << endl;
	    continue;
	  }
	  assert( Val && "Value is NULL");   

	  LiveRange *const LR = LRI.getLiveRangeForValue(Val);

	  if ( !LR ) {

	    // nothing to worry if it's a const or a label

            if (DEBUG_RA) {
              cout << "*NO LR for operand : " << Op ;
	      cout << " [reg:" <<  Op.getAllocatedRegNum() << "]";
	      cout << " in inst:\t" << *MInst << endl;
            }

	    // if register is not allocated, mark register as invalid
	    if( Op.getAllocatedRegNum() == -1)
	      Op.setRegForValue( MRI.getInvalidRegNum()); 
	    

	    continue;
	  }
	
	  unsigned RCID = (LR->getRegClass())->getID();

	  if( LR->hasColor() ) {
	    Op.setRegForValue( MRI.getUnifiedRegNum(RCID, LR->getColor()) );
	  }
	  else {

	    // LR did NOT receive a color (register). Now, insert spill code
	    // for spilled opeands in this machine instruction

	    assert(0 && "LR must be spilled");
	    //	    insertCode4SpilledLR(LR, MInst, *BBI, OpNum );

	  }
	}

      } // for each operand


      // If there are instructions to be added *after* this machine
      // instruction, add them now
      
      if( AddedInstrMap[ MInst ] && 
	  ! (AddedInstrMap[ MInst ]->InstrnsAfter).empty() ) {

	// if there are delay slots for this instruction, the instructions
	// added after it must really go after the delayed instruction(s)
	// So, we move the InstrAfter of the current instruction to the 
	// corresponding delayed instruction
	
	unsigned delay;
	if((delay=TM.getInstrInfo().getNumDelaySlots(MInst->getOpCode())) >0){ 
	  move2DelayedInstr(MInst,  *(MInstIterator+delay) );

	  if(DEBUG_RA)  cout<< "\nMoved an added instr after the delay slot";
	}
       
	else {
	

	  // Here we can add the "instructions after" to the current
	  // instruction since there are no delay slots for this instruction

	  deque<MachineInstr *> &IAft = (AddedInstrMap[MInst])->InstrnsAfter;
	  
	  if( ! IAft.empty() ) {     
	    
	    deque<MachineInstr *>::iterator AdIt; 
	    
	    ++MInstIterator;   // advance to the next instruction
	    
	    for( AdIt = IAft.begin(); AdIt != IAft.end() ; ++AdIt ) {
	      
	      if(DEBUG_RA) 
		cerr << " *#* APPENDed instr opcode: "  << *AdIt << endl;
	      
	      MInstIterator = MIVec.insert( MInstIterator, *AdIt );
	      ++MInstIterator;
	    }

	    // MInsterator already points to the next instr. Since the
	    // for loop also increments it, decrement it to point to the
	    // instruction added last
	    --MInstIterator;  
	    
	  }
	  
	}  // if not delay
	
      }
      
    } // for each machine instruction
  }
}



#if 0


//----------------------------------------------------------------------------
// This method inserts spill code for AN operand whose LR was spilled.
// This method may be called several times for a single machine instruction
// if it contains many spilled operands. Each time it is called, it finds
// a register which is not live at that instruction and also which is not
// used by other spilled operands of the same instruction. Then it uses
// this register temporarily to accomodate the spilled value.
//----------------------------------------------------------------------------
void PhyRegAlloc::insertCode4SpilledLR(const LiveRange *LR, 
				       const MachineInstr *MInst,
				       const BasisBlock *BB,
				       const unsigned OpNum) {

  MachineOperand& Op = MInst->getOperand(OpNum);
  bool isDef =  MInst->operandIsDefined(OpNum);
  unsigned RegType = MRI.getRegType( LR );
  int SpillOff = LR->getSpillOffFromFP();
  RegClass *RC = LR->getRegClass();
  const LiveVarSet *LVSetBef =  LVI->getLiveVarSetBeforeMInst(MInst, BB);
  int TmpOff = StackOffsets.getNewTmpPosOffFromSP();
  MachineInstr *MIBef,  *AdIMid, *MIAft;
  int TmpReg;

  TmpReg = getUsableRegAtMI(RC, RegType, MInst,LVSetBef, MIBef, MIAft);
  TmpReg = getUnifiedRegNum( RC->getID(), TmpReg );
  
  
  if( !isDef ) {

    // for a USE, we have to load the value of LR from stack to a TmpReg
    // and use the TmpReg as one operand of instruction

    // actual loading instruction
    AdIMid = MRI.cpMem2RegMI(MRI.getFramePointer(), SpillOff, TmpReg, RegType);

    if( MIBef )
      ((AddedInstrMap[MInst])->InstrnsBefore).push_back(MIBef);

    ((AddedInstrMap[MInst])->InstrnsBefore).push_back(AdiMid);

    if( MIAft)
      ((AddedInstrMap[MInst])->InstrnsAfter).push_front(MIAft);

    
  } 
  else {   // if this is a Def

    // for a DEF, we have to store the value produced by this instruction
    // on the stack position allocated for this LR

    // actual storing instruction
    AdIMid = MRI.cpReg2MemMI(TmpReg, MRI.getFramePointer(), SpillOff, RegType);

    if( MIBef )
      ((AddedInstrMap[MInst])->InstrnsBefore).push_back(MIBef);

    ((AddedInstrMap[MInst])->InstrnsBefore).push_back(AdiMid);

    if( MIAft)
      ((AddedInstrMap[MInst])->InstrnsAfter).push_front(MIAft);

  }  // if !DEF


  Op.setRegForValue( TmpReg );    // set the opearnd


}


//----------------------------------------------------------------------------
// We can use the following method to get a temporary register to be used
// BEFORE any given machine instruction. If there is a register available,
// this method will simply return that register and set MIBef = MIAft = NULL.
// Otherwise, it will return a register and MIAft and MIBef will contain
// two instructions used to free up this returned register.
//----------------------------------------------------------------------------

int PhyRegAlloc::getUsableRegAtMI(const RegClass *RC, 
				  const int RegType,
				  const MachineInstr *MInst, 
				  const LiveVarSet *LVSetBef,
				  MachineInstr *MIBef,
				  MachineInstr *MIAft) {

  int Reg =  getUnusedRegAtMI(RC, MInst, LVSetBef);

  if( Reg != -1) {
    // we found an unused register, so we can simply used
    MIBef = MIAft = NULL;
  }
  else {
    // we couldn't find an unused register. Generate code to ree up a reg by
    // saving it on stack and restoring after the instruction

    Reg = getRegNotUsedByThisInst(RC, MInst);
    MIBef = cpReg2MemMI(Reg, MRI.getFramePointer(), TmpOff, RegType );
    MIAft = cpMem2RegMI(MEI.getFramePointer(), TmpOff, Reg, RegType );
  }

  return Reg;
}

//----------------------------------------------------------------------------
// This method is called to get a new unused register that can be used to
// accomodate a spilled value. 
// This method may be called several times for a single machine instruction
// if it contains many spilled operands. Each time it is called, it finds
// a register which is not live at that instruction and also which is not
// used by other spilled operands of the same instruction.
//----------------------------------------------------------------------------
int PhyRegAlloc::getUnusedRegAtMI(const RegClass *RC, 
				  const MachineInstr *MInst, 
				  const LiveVarSet *LVSetBef) {

  unsigned NumAvailRegs =  RC->getNumOfAvailRegs();
  
  bool *IsColorUsedArr = RC->getIsColorUsedArr();
  
  for(unsigned i=0; i <  NumAvailRegs; i++);
      IsColorUsedArr[i] = false;
      
  LiveVarSet::const_iterator LIt = LVSetBef->begin();

  // for each live var in live variable set after machine inst
  for( ; LIt != LVSetBef->end(); ++LIt) {

   //  get the live range corresponding to live var
    LiveRange *const LRofLV = LRI.getLiveRangeForValue(*LIt );    

    // LR can be null if it is a const since a const 
    // doesn't have a dominating def - see Assumptions above
    if( LRofLV )     
      if( LRofLV->hasColor() ) 
	IsColorUsedArr[ LRofLV->getColor() ] = true;
  }

  // It is possible that one operand of this MInst was already spilled
  // and it received some register temporarily. If that's the case,
  // it is recorded in machine operand. We must skip such registers.

  setRegsUsedByThisInst(RC, MInst);

  unsigned c;                         // find first unused color
  for( c=0; c < NumAvailRegs; c++)  
     if( ! IsColorUsedArr[ c ] ) break;
   
  if(c < NumAvailRegs) 
    return c;
  else 
    return -1;


}


#endif

//----------------------------------------------------------------------------
// This method modifies the IsColorUsedArr of the register class passed to it.
// It sets the bits corresponding to the registers used by this machine
// instructions. Explicit operands are set.
//----------------------------------------------------------------------------
void PhyRegAlloc::setRegsUsedByThisInst(RegClass *RC, 
				       const MachineInstr *MInst ) {

 bool *IsColorUsedArr = RC->getIsColorUsedArr();
  
 for(unsigned OpNum=0; OpNum < MInst->getNumOperands(); ++OpNum) {
    
   const MachineOperand& Op = MInst->getOperand(OpNum);

    if( Op.getOperandType() ==  MachineOperand::MO_VirtualRegister || 
	Op.getOperandType() ==  MachineOperand::MO_CCRegister) {

      const Value *const Val =  Op.getVRegValue();

      if( !Val ) 
	if( MRI.getRegClassIDOfValue( Val )== RC->getID() ) {   
	  int Reg;
	  if( (Reg=Op.getAllocatedRegNum()) != -1)
	    IsColorUsedArr[ Reg ] = true;
	
	}
    }
 }
 
 // If there are implicit references, mark them as well

 for(unsigned z=0; z < MInst->getNumImplicitRefs(); z++) {

   LiveRange *const LRofImpRef = 
     LRI.getLiveRangeForValue( MInst->getImplicitRef(z)  );    

   if( LRofImpRef )     
     if( LRofImpRef->hasColor() ) 
       IsColorUsedArr[ LRofImpRef->getColor() ] = true;
 }



}



//----------------------------------------------------------------------------
// Get any other register in a register class, other than what is used
// by operands of a machine instruction.
//----------------------------------------------------------------------------
int PhyRegAlloc::getRegNotUsedByThisInst(RegClass *RC, 
					 const MachineInstr *MInst) {

  bool *IsColorUsedArr = RC->getIsColorUsedArr();
  unsigned NumAvailRegs =  RC->getNumOfAvailRegs();


  for(unsigned i=0; i < NumAvailRegs ; i++)
    IsColorUsedArr[i] = false;

  setRegsUsedByThisInst(RC, MInst);

  unsigned c;                         // find first unused color
  for( c=0; c <  RC->getNumOfAvailRegs(); c++)  
     if( ! IsColorUsedArr[ c ] ) break;
   
  if(c < NumAvailRegs) 
    return c;
  else 
    assert( 0 && "FATAL: No free register could be found in reg class!!");

}





//----------------------------------------------------------------------------
// If there are delay slots for an instruction, the instructions
// added after it must really go after the delayed instruction(s).
// So, we move the InstrAfter of that instruction to the 
// corresponding delayed instruction using the following method.

//----------------------------------------------------------------------------
void PhyRegAlloc:: move2DelayedInstr(const MachineInstr *OrigMI,
				     const MachineInstr *DelayedMI) {


  // "added after" instructions of the original instr
  deque<MachineInstr *> &OrigAft = (AddedInstrMap[OrigMI])->InstrnsAfter;

  // "added instructions" of the delayed instr
  AddedInstrns *DelayAdI = AddedInstrMap[DelayedMI];

  if(! DelayAdI )  {                // create a new "added after" if necessary
    DelayAdI = new AddedInstrns();
    AddedInstrMap[DelayedMI] =  DelayAdI;
  }

  // "added after" instructions of the delayed instr
  deque<MachineInstr *> &DelayedAft = DelayAdI->InstrnsAfter;

  // go thru all the "added after instructions" of the original instruction
  // and append them to the "addded after instructions" of the delayed
  // instructions

  deque<MachineInstr *>::iterator OrigAdIt; 
	    
  for( OrigAdIt = OrigAft.begin(); OrigAdIt != OrigAft.end() ; ++OrigAdIt ) { 
    DelayedAft.push_back( *OrigAdIt );
  }    

  // empty the "added after instructions" of the original instruction
  OrigAft.clear();
    
}

//----------------------------------------------------------------------------
// This method prints the code with registers after register allocation is
// complete.
//----------------------------------------------------------------------------
void PhyRegAlloc::printMachineCode()
{

  cout << endl << ";************** Method ";
  cout << Meth->getName() << " *****************" << endl;

  Method::const_iterator BBI = Meth->begin();  // random iterator for BBs   

  for( ; BBI != Meth->end(); ++BBI) {          // traverse BBs in random order

    cout << endl ; printLabel( *BBI); cout << ": ";

    // get the iterator for machine instructions
    MachineCodeForBasicBlock& MIVec = (*BBI)->getMachineInstrVec();
    MachineCodeForBasicBlock::iterator MInstIterator = MIVec.begin();

    // iterate over all the machine instructions in BB
    for( ; MInstIterator != MIVec.end(); ++MInstIterator) {  
      
      MachineInstr *const MInst = *MInstIterator; 


      cout << endl << "\t";
      cout << TargetInstrDescriptors[MInst->getOpCode()].opCodeString;
      

      //for(MachineInstr::val_op_const_iterator OpI(MInst);!OpI.done();++OpI) {

      for(unsigned OpNum=0; OpNum < MInst->getNumOperands(); ++OpNum) {

	MachineOperand& Op = MInst->getOperand(OpNum);

	if( Op.getOperandType() ==  MachineOperand::MO_VirtualRegister || 
	    Op.getOperandType() ==  MachineOperand::MO_CCRegister /*|| 
	    Op.getOperandType() ==  MachineOperand::MO_PCRelativeDisp*/ ) {

	  const Value *const Val = Op.getVRegValue () ;
	  // ****this code is temporary till NULL Values are fixed
	  if( ! Val ) {
	    cout << "\t<*NULL*>";
	    continue;
	  }

	  // if a label or a constant
	  if( (Val->getValueType() == Value::BasicBlockVal)  ) {

	    cout << "\t"; printLabel(	Op.getVRegValue	() );
	  }
	  else {
	    // else it must be a register value
	    const int RegNum = Op.getAllocatedRegNum();

	    cout << "\t" << "%" << MRI.getUnifiedRegName( RegNum );
	  }

	} 
	else if(Op.getOperandType() ==  MachineOperand::MO_MachineRegister) {
	  cout << "\t" << "%" << MRI.getUnifiedRegName(Op.getMachineRegNum());
	}

	else 
	  cout << "\t" << Op;      // use dump field
      }

    

      unsigned NumOfImpRefs =  MInst->getNumImplicitRefs();
      if(  NumOfImpRefs > 0 ) {
	
	cout << "\tImplicit:";

	for(unsigned z=0; z < NumOfImpRefs; z++) {
	  printValue(  MInst->getImplicitRef(z) );
	  cout << "\t";
	}
	
      }

    } // for all machine instructions


    cout << endl;

  } // for all BBs

  cout << endl;
}


//----------------------------------------------------------------------------
//
//----------------------------------------------------------------------------

void PhyRegAlloc::colorCallRetArgs()
{

  CallRetInstrListType &CallRetInstList = LRI.getCallRetInstrList();
  CallRetInstrListType::const_iterator It = CallRetInstList.begin();

  for( ; It != CallRetInstList.end(); ++It ) {

    const MachineInstr *const CRMI = *It;
    unsigned OpCode =  CRMI->getOpCode();
 
    // get the added instructions for this Call/Ret instruciton
    AddedInstrns *AI = AddedInstrMap[ CRMI ];
    if ( !AI ) { 
      AI = new AddedInstrns();
      AddedInstrMap[ CRMI ] = AI;
    }

    // Tmp stack poistions are needed by some calls that have spilled args
    // So reset it before we call each such method
    StackOffsets.resetTmpPos();  

    if( (TM.getInstrInfo()).isCall( OpCode ) )
      MRI.colorCallArgs( CRMI, LRI, AI, *this );
    
    else if (  (TM.getInstrInfo()).isReturn(OpCode) ) 
      MRI.colorRetValue( CRMI, LRI, AI );
    
    else assert( 0 && "Non Call/Ret instrn in CallRetInstrList\n" );

  }

}



//----------------------------------------------------------------------------

//----------------------------------------------------------------------------
void PhyRegAlloc::colorIncomingArgs()
{
  const BasicBlock *const FirstBB = Meth->front();
  const MachineInstr *FirstMI = *((FirstBB->getMachineInstrVec()).begin());
  assert( FirstMI && "No machine instruction in entry BB");

  AddedInstrns *AI = AddedInstrMap[ FirstMI ];
  if ( !AI ) { 
    AI = new AddedInstrns();
    AddedInstrMap[ FirstMI  ] = AI;
  }

  MRI.colorMethodArgs(Meth, LRI, AI );
}


//----------------------------------------------------------------------------
// Used to generate a label for a basic block
//----------------------------------------------------------------------------
void PhyRegAlloc::printLabel(const Value *const Val)
{
  if( Val->hasName() )
    cout  << Val->getName();
  else
    cout << "Label" <<  Val;
}


//----------------------------------------------------------------------------
// This method calls setSugColorUsable method of each live range. This
// will determine whether the suggested color of LR is  really usable.
// A suggested color is not usable when the suggested color is volatile
// AND when there are call interferences
//----------------------------------------------------------------------------

void PhyRegAlloc::markUnusableSugColors()
{
  if(DEBUG_RA ) cout << "\nmarking unusable suggested colors ..." << endl;

  // hash map iterator
  LiveRangeMapType::const_iterator HMI = (LRI.getLiveRangeMap())->begin();   
  LiveRangeMapType::const_iterator HMIEnd = (LRI.getLiveRangeMap())->end();   

    for(  ; HMI != HMIEnd ; ++HMI ) {
      
      if( (*HMI).first ) { 

	LiveRange *L = (*HMI).second;      // get the LiveRange

	if(L) { 
	  if( L->hasSuggestedColor() ) {

	    int RCID = (L->getRegClass())->getID();
	    if( MRI.isRegVolatile( RCID,  L->getSuggestedColor()) &&
		L->isCallInterference() )
	      L->setSuggestedColorUsable( false );
	    else
	      L->setSuggestedColorUsable( true );
	  }
	} // if L->hasSuggestedColor()
      }
    } // for all LR's in hash map
}



//----------------------------------------------------------------------------
// The following method will set the stack offsets of the live ranges that
// are decided to be spillled. This must be called just after coloring the
// LRs using the graph coloring algo. For each live range that is spilled,
// this method allocate a new spill position on the stack.
//----------------------------------------------------------------------------

void PhyRegAlloc::allocateStackSpace4SpilledLRs()
{
  if(DEBUG_RA ) cout << "\nsetting LR stack offsets ..." << endl;

  // hash map iterator
  LiveRangeMapType::const_iterator HMI = (LRI.getLiveRangeMap())->begin();   
  LiveRangeMapType::const_iterator HMIEnd = (LRI.getLiveRangeMap())->end();   

    for(  ; HMI != HMIEnd ; ++HMI ) {
      
      if( (*HMI).first ) { 
	LiveRange *L = (*HMI).second;      // get the LiveRange
	if(L)
	  if( ! L->hasColor() ) 
	    L->setSpillOffFromFP( StackOffsets.getNewSpillOffFromFP() );   
      }
    } // for all LR's in hash map

    StackOffsets.setEndOfSpillRegion();

}






//----------------------------------------------------------------------------
// The entry pont to Register Allocation
//----------------------------------------------------------------------------

void PhyRegAlloc::allocateRegisters()
{

  // make sure that we put all register classes into the RegClassList 
  // before we call constructLiveRanges (now done in the constructor of 
  // PhyRegAlloc class).

  constructLiveRanges();                // create LR info

  if( DEBUG_RA )
    LRI.printLiveRanges();
  
  createIGNodeListsAndIGs();            // create IGNode list and IGs

  buildInterferenceGraphs();            // build IGs in all reg classes
  
  
  if( DEBUG_RA ) {
    // print all LRs in all reg classes
    for( unsigned int rc=0; rc < NumOfRegClasses  ; rc++)  
      RegClassList[ rc ]->printIGNodeList(); 
    
    // print IGs in all register classes
    for( unsigned int rc=0; rc < NumOfRegClasses ; rc++)  
      RegClassList[ rc ]->printIG();       
  }
  
  LRI.coalesceLRs();                    // coalesce all live ranges
  
  if( DEBUG_RA) {
    // print all LRs in all reg classes
    for( unsigned int rc=0; rc < NumOfRegClasses  ; rc++)  
      RegClassList[ rc ]->printIGNodeList(); 
    
    // print IGs in all register classes
    for( unsigned int rc=0; rc < NumOfRegClasses ; rc++)  
      RegClassList[ rc ]->printIG();       
  }


  // mark un-usable suggested color before graph coloring algorithm.
  // When this is done, the graph coloring algo will not reserve
  // suggested color unnecessarily - they can be used by another LR
  markUnusableSugColors(); 

  // color all register classes using the graph coloring algo
  for( unsigned int rc=0; rc < NumOfRegClasses ; rc++)  
    RegClassList[ rc ]->colorAllRegs();    

  // Atter grpah coloring, if some LRs did not receive a color (i.e, spilled)
  // a poistion for such spilled LRs
  allocateStackSpace4SpilledLRs();

  // color incoming args and call args
  colorIncomingArgs();
  colorCallRetArgs();

 
  updateMachineCode(); 
  if (DEBUG_RA) {
    Meth->getMachineCode().dump();
    printMachineCode();                   // only for DEBUGGING
  }
}



