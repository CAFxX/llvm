#include "llvm/CodeGen/LiveRangeInfo.h"
#include <iostream>
using std::cerr;

//---------------------------------------------------------------------------
// Constructor
//---------------------------------------------------------------------------
LiveRangeInfo::LiveRangeInfo(const Method *const M, 
			     const TargetMachine& tm,
			     std::vector<RegClass *> &RCL)
                             : Meth(M), LiveRangeMap(), TM(tm),
                               RegClassList(RCL), MRI(tm.getRegInfo())
{ }


//---------------------------------------------------------------------------
// Destructor: Deletes all LiveRanges in the LiveRangeMap
//---------------------------------------------------------------------------
LiveRangeInfo::~LiveRangeInfo() {
  LiveRangeMapType::iterator MI =  LiveRangeMap.begin(); 

  for( ; MI != LiveRangeMap.end() ; ++MI) {  
    if (MI->first && MI->second) {
      LiveRange *LR = MI->second;

      // we need to be careful in deleting LiveRanges in LiveRangeMap
      // since two/more Values in the live range map can point to the same
      // live range. We have to make the other entries NULL when we delete
      // a live range.

      LiveRange::iterator LI = LR->begin();
      
      for( ; LI != LR->end() ; ++LI)
        LiveRangeMap[*LI] = 0;
      
      delete LR;
    }
  }
}


//---------------------------------------------------------------------------
// union two live ranges into one. The 2nd LR is deleted. Used for coalescing.
// Note: the caller must make sure that L1 and L2 are distinct and both
// LRs don't have suggested colors
//---------------------------------------------------------------------------
void LiveRangeInfo::unionAndUpdateLRs(LiveRange *const L1, LiveRange *L2)
{
  assert( L1 != L2);
  L1->setUnion( L2 );                   // add elements of L2 to L1
  ValueSet::iterator L2It;

  for( L2It = L2->begin() ; L2It != L2->end(); ++L2It) {

    //assert(( L1->getTypeID() == L2->getTypeID()) && "Merge:Different types");

    L1->add( *L2It );                   // add the var in L2 to L1
    LiveRangeMap[ *L2It ] = L1;         // now the elements in L2 should map 
                                        //to L1    
  }


  // Now if LROfDef(L1) has a suggested color, it will remain.
  // But, if LROfUse(L2) has a suggested color, the new range
  // must have the same color.

  if(L2->hasSuggestedColor())
    L1->setSuggestedColor( L2->getSuggestedColor() );


  if( L2->isCallInterference() )
    L1->setCallInterference();
  
 
  L1->addSpillCost( L2->getSpillCost() ); // add the spill costs

  delete L2;                        // delete L2 as it is no longer needed
}



//---------------------------------------------------------------------------
// Method for constructing all live ranges in a method. It creates live 
// ranges for all values defined in the instruction stream. Also, it
// creates live ranges for all incoming arguments of the method.
//---------------------------------------------------------------------------
void LiveRangeInfo::constructLiveRanges()
{  

  if( DEBUG_RA) 
    cerr << "Consturcting Live Ranges ...\n";

  // first find the live ranges for all incoming args of the method since
  // those LRs start from the start of the method
      
                                                 // get the argument list
  const Method::ArgumentListType& ArgList = Meth->getArgumentList();           
                                                 // get an iterator to arg list
  Method::ArgumentListType::const_iterator ArgIt = ArgList.begin(); 

             
  for( ; ArgIt != ArgList.end() ; ++ArgIt) {     // for each argument
    LiveRange * ArgRange = new LiveRange();      // creates a new LR and 
    const Value *const Val = (const Value *) *ArgIt;

    assert( Val);

    ArgRange->add(Val);     // add the arg (def) to it
    LiveRangeMap[Val] = ArgRange;

    // create a temp machine op to find the register class of value
    //const MachineOperand Op(MachineOperand::MO_VirtualRegister);

    unsigned rcid = MRI.getRegClassIDOfValue( Val );
    ArgRange->setRegClass(RegClassList[ rcid ] );

    			   
    if( DEBUG_RA > 1) {     
      cerr << " adding LiveRange for argument ";    
      printValue((const Value *) *ArgIt); cerr << "\n";
    }
  }

  // Now suggest hardware registers for these method args 
  MRI.suggestRegs4MethodArgs(Meth, *this);



  // Now find speical LLVM instructions (CALL, RET) and LRs in machine
  // instructions.


  Method::const_iterator BBI = Meth->begin();    // random iterator for BBs   
  for( ; BBI != Meth->end(); ++BBI) {            // go thru BBs in random order

    // Now find all LRs for machine the instructions. A new LR will be created 
    // only for defs in the machine instr since, we assume that all Values are
    // defined before they are used. However, there can be multiple defs for
    // the same Value in machine instructions.

    // get the iterator for machine instructions
    const MachineCodeForBasicBlock& MIVec = (*BBI)->getMachineInstrVec();
    MachineCodeForBasicBlock::const_iterator MInstIterator = MIVec.begin();

    // iterate over all the machine instructions in BB
    for( ; MInstIterator != MIVec.end(); MInstIterator++) {  
      
      const MachineInstr * MInst = *MInstIterator; 

      // Now if the machine instruction is a  call/return instruction,
      // add it to CallRetInstrList for processing its implicit operands

      if(TM.getInstrInfo().isReturn(MInst->getOpCode()) ||
	 TM.getInstrInfo().isCall(MInst->getOpCode()))
	CallRetInstrList.push_back( MInst ); 
 
             
      // iterate over  MI operands to find defs
      for (MachineInstr::val_const_op_iterator OpI(MInst); !OpI.done(); ++OpI) {
	if(DEBUG_RA) {
	  MachineOperand::MachineOperandType OpTyp = 
	    OpI.getMachineOperand().getOperandType();

	  if (OpTyp == MachineOperand::MO_CCRegister) {
	    cerr << "\n**CC reg found. Is Def=" << OpI.isDef() << " Val:";
	    printValue( OpI.getMachineOperand().getVRegValue() );
	    cerr << "\n";
	  }
	}

	// create a new LR iff this operand is a def
	if( OpI.isDef() ) {     
	  const Value *const Def = *OpI;

	  // Only instruction values are accepted for live ranges here
	  if( Def->getValueType() != Value::InstructionVal ) {
	    cerr << "\n**%%Error: Def is not an instruction val. Def=";
	    printValue( Def ); cerr << "\n";
	    continue;
	  }

	  LiveRange *DefRange = LiveRangeMap[Def]; 

	  // see LR already there (because of multiple defs)
	  if( !DefRange) {                  // if it is not in LiveRangeMap
	    DefRange = new LiveRange();     // creates a new live range and 
	    DefRange->add( Def );           // add the instruction (def) to it
	    LiveRangeMap[ Def ] = DefRange; // update the map

	    if( DEBUG_RA > 1) { 	    
	      cerr << "  creating a LR for def: ";    
	      printValue(Def); cerr  << "\n";
	    }

	    // set the register class of the new live range
	    //assert( RegClassList.size() );
	    MachineOperand::MachineOperandType OpTy = 
	      OpI.getMachineOperand().getOperandType();

	    bool isCC = ( OpTy == MachineOperand::MO_CCRegister);
	    unsigned rcid = MRI.getRegClassIDOfValue( 
			    OpI.getMachineOperand().getVRegValue(), isCC );


	    if(isCC && DEBUG_RA) {
	      cerr  << "\a**created a LR for a CC reg:";
	      printValue( OpI.getMachineOperand().getVRegValue() );
	    }

	    DefRange->setRegClass( RegClassList[ rcid ] );

	  }
	  else {
	    DefRange->add( Def );           // add the opearand to def range
                                            // update the map - Operand points 
	                                    // to the merged set
	    LiveRangeMap[ Def ] = DefRange; 

	    if( DEBUG_RA > 1) { 
	      cerr << "   added to an existing LR for def: ";  
	      printValue( Def ); cerr  << "\n";
	    }
	  }

	} // if isDef()
	
      } // for all opereands in machine instructions

    } // for all machine instructions in the BB

  } // for all BBs in method
  

  // Now we have to suggest clors for call and return arg live ranges.
  // Also, if there are implicit defs (e.g., retun value of a call inst)
  // they must be added to the live range list

  suggestRegs4CallRets();

  if( DEBUG_RA) 
    cerr << "Initial Live Ranges constructed!\n";

}


//---------------------------------------------------------------------------
// If some live ranges must be colored with specific hardware registers
// (e.g., for outgoing call args), suggesting of colors for such live
// ranges is done using target specific method. Those methods are called
// from this function. The target specific methods must:
//    1) suggest colors for call and return args. 
//    2) create new LRs for implicit defs in machine instructions
//---------------------------------------------------------------------------
void LiveRangeInfo::suggestRegs4CallRets()
{

  CallRetInstrListType::const_iterator It =  CallRetInstrList.begin();

  for( ; It !=  CallRetInstrList.end(); ++It ) {

    const MachineInstr *MInst = *It;
    MachineOpCode OpCode =  MInst->getOpCode();

    if( (TM.getInstrInfo()).isReturn(OpCode)  )
      MRI.suggestReg4RetValue( MInst, *this);

    else if( (TM.getInstrInfo()).isCall( OpCode ) )
      MRI.suggestRegs4CallArgs( MInst, *this, RegClassList );
    
    else 
      assert( 0 && "Non call/ret instr in  CallRetInstrList" );
  }

}


//--------------------------------------------------------------------------
// The following method coalesces live ranges when possible. This method
// must be called after the interference graph has been constructed.


/* Algorithm:
   for each BB in method
     for each machine instruction (inst)
       for each definition (def) in inst
         for each operand (op) of inst that is a use
           if the def and op are of the same register type
	     if the def and op do not interfere //i.e., not simultaneously live
	       if (degree(LR of def) + degree(LR of op)) <= # avail regs
	         if both LRs do not have suggested colors
		    merge2IGNodes(def, op) // i.e., merge 2 LRs 

*/
//---------------------------------------------------------------------------
void LiveRangeInfo::coalesceLRs()  
{
  if( DEBUG_RA) 
    cerr << "\nCoalscing LRs ...\n";

  Method::const_iterator BBI = Meth->begin();  // random iterator for BBs   

  for( ; BBI != Meth->end(); ++BBI) {          // traverse BBs in random order

    // get the iterator for machine instructions
    const MachineCodeForBasicBlock& MIVec = (*BBI)->getMachineInstrVec();
    MachineCodeForBasicBlock::const_iterator MInstIterator = MIVec.begin();

    // iterate over all the machine instructions in BB
    for( ; MInstIterator != MIVec.end(); ++MInstIterator) {  
      
      const MachineInstr * MInst = *MInstIterator; 

      if( DEBUG_RA > 1) {
	cerr << " *Iterating over machine instr ";
	MInst->dump();
	cerr << "\n";
      }


      // iterate over  MI operands to find defs
      for(MachineInstr::val_const_op_iterator DefI(MInst);!DefI.done();++DefI){
	
	if( DefI.isDef() ) {            // iff this operand is a def

	  LiveRange *const LROfDef = getLiveRangeForValue( *DefI );
	  assert( LROfDef );
	  RegClass *const RCOfDef = LROfDef->getRegClass();

	  MachineInstr::val_const_op_iterator UseI(MInst);
	  for( ; !UseI.done(); ++UseI){ // for all uses

 	    LiveRange *const LROfUse = getLiveRangeForValue( *UseI );

	    if( ! LROfUse ) {           // if LR of use is not found

	      //don't warn about labels
	      if (!((*UseI)->getType())->isLabelType() && DEBUG_RA) {
		cerr<<" !! Warning: No LR for use "; printValue(*UseI);
		cerr << "\n";
	      }
	      continue;                 // ignore and continue
	    }

	    if( LROfUse == LROfDef)     // nothing to merge if they are same
	      continue;

	    //RegClass *const RCOfUse = LROfUse->getRegClass();
	    //if( RCOfDef == RCOfUse ) {  // if the reg classes are the same

	    if( MRI.getRegType(LROfDef) == MRI.getRegType(LROfUse) ) {

	      // If the two RegTypes are the same

	      if( ! RCOfDef->getInterference(LROfDef, LROfUse) ) {

		unsigned CombinedDegree =
		  LROfDef->getUserIGNode()->getNumOfNeighbors() + 
		  LROfUse->getUserIGNode()->getNumOfNeighbors();

		if( CombinedDegree <= RCOfDef->getNumOfAvailRegs() ) {

		  // if both LRs do not have suggested colors
		  if( ! (LROfDef->hasSuggestedColor() &&  
		         LROfUse->hasSuggestedColor() ) ) {
		    
		    RCOfDef->mergeIGNodesOfLRs(LROfDef, LROfUse);
		    unionAndUpdateLRs(LROfDef, LROfUse);
		  }


		} // if combined degree is less than # of regs

	      } // if def and use do not interfere

	    }// if reg classes are the same

	  } // for all uses

	} // if def

      } // for all defs

    } // for all machine instructions

  } // for all BBs

  if( DEBUG_RA) 
    cerr << "\nCoalscing Done!\n";

}





/*--------------------------- Debug code for printing ---------------*/


void LiveRangeInfo::printLiveRanges()
{
  LiveRangeMapType::iterator HMI = LiveRangeMap.begin();   // hash map iterator
  cerr << "\nPrinting Live Ranges from Hash Map:\n";
  for( ; HMI != LiveRangeMap.end() ; ++HMI) {
    if( HMI->first && HMI->second ) {
      cerr <<" "; printValue((*HMI).first);  cerr << "\t: "; 
      HMI->second->printSet(); cerr << "\n";
    }
  }
}


