#include "SparcRegClassInfo.h"
#include "llvm/CodeGen/RegAllocCommon.h"
#include "llvm/Target/Sparc.h"
#include "llvm/Type.h"
#include <iostream>
using std::cerr;

//-----------------------------------------------------------------------------
// Int Register Class - method for coloring a node in the interference graph.
//
// Algorithm:
//     Record the colors/suggested colors of all neighbors.
//
//     If there is a suggested color, try to allocate it
//     If there is no call interf, try to allocate volatile, then non volatile
//     If there is call interf, try to allocate non-volatile. If that fails
//     try to allocate a volatile and insert save across calls
//     If both above fail, spill.
//  
//-----------------------------------------------------------------------------
void SparcIntRegClass::colorIGNode(IGNode * Node, bool IsColorUsedArr[]) const {
  LiveRange *LR = Node->getParentLR();
  unsigned NumNeighbors =  Node->getNumOfNeighbors();   // total # of neighbors

  for (unsigned n=0; n < NumNeighbors; n++) {            // for each neigh 
    IGNode *NeighIGNode = Node->getAdjIGNode(n);
    LiveRange *NeighLR = NeighIGNode->getParentLR();

    if(NeighLR->hasColor())                        // if has a color
      IsColorUsedArr[NeighLR->getColor()] = true; // record that color

    else if (NeighLR->hasSuggestedColor()) {

      // if the neighbout can use the suggested color 
      if(NeighLR->isSuggestedColorUsable()) 
        IsColorUsedArr[NeighLR->getSuggestedColor()] = true; 
    }    
  }

  if( DEBUG_RA ) {
    cerr << "\nColoring LR [CallInt=" << LR->isCallInterference() <<"]:"; 
    printSet(*LR);
  }

  if( LR->hasSuggestedColor() ) {

    unsigned SugCol = LR->getSuggestedColor();

    if( ! IsColorUsedArr[ SugCol ] ) {

      if( LR->isSuggestedColorUsable()  ) {

	// if the suggested color is volatile, we should use it only if
	// there are no call interferences. Otherwise, it will get spilled.

	if (DEBUG_RA)
	  cerr << "\n  -Coloring with sug color: " << SugCol;

	LR->setColor(  LR->getSuggestedColor() );
	return;
      }
       else if(DEBUG_RA)
	 cerr << "\n Couldn't alloc Sug col - LR voloatile & calls interf";

    }
    else if ( DEBUG_RA ) {                // can't allocate the suggested col
      cerr << "  \n  Could NOT allocate the suggested color (already used) ";
      printSet(*LR); cerr << "\n";
    }
  }

  unsigned SearchStart;                 // start pos of color in pref-order
  bool ColorFound= false;               // have we found a color yet?

  //if this Node is between calls
  if( ! LR->isCallInterference() ) { 

    // start with volatiles (we can  allocate volatiles safely)
    SearchStart = SparcIntRegOrder::StartOfAllRegs;  
  }
  else {           
    // start with non volatiles (no non-volatiles)
    SearchStart =  SparcIntRegOrder::StartOfNonVolatileRegs;  
  }

  unsigned c=0;                         // color
 
  // find first unused color
  for( c=SearchStart; c < SparcIntRegOrder::NumOfAvailRegs; c++) { 
    if( ! IsColorUsedArr[ c ] ) { ColorFound = true; break; }
  }

  if( ColorFound) {
    LR->setColor(c);                  // first color found in preffered order
    if (DEBUG_RA) cerr << "\n  Colored after first search with col " << c ; 
  }

  // if color is not found because of call interference
  // try even finding a volatile color and insert save across calls
  //
  else if( LR->isCallInterference() ) 
  { 
    // start from 0 - try to find even a volatile this time
    SearchStart = SparcIntRegOrder::StartOfAllRegs;  

    // find first unused volatile color
    for(c=SearchStart; c < SparcIntRegOrder::StartOfNonVolatileRegs; c++) { 
      if( ! IsColorUsedArr[ c ] ) { ColorFound = true; break; }
    }

    if (ColorFound) { 
       LR->setColor(c);  
       //  get the live range corresponding to live var
       // since LR span across calls, must save across calls 
       //
       LR->markForSaveAcrossCalls();       
       if(DEBUG_RA) cerr << "\n  Colored after SECOND search with col " << c ;
    }
  }


  // If we couldn't find a color regardless of call interference - i.e., we
  // don't have either a volatile or non-volatile color left
  //
  if (!ColorFound)  
    LR->markForSpill();               // no color found - must spill
}






//-----------------------------------------------------------------------------
// Float Register Class - method for coloring a node in the interference graph.
//
// Algorithm:
//
//     If the LR is a double try to allocate f32 - f63
//     If the above fails or LR is single precision
//        If the LR does not interfere with a call
//	   start allocating from f0
//	Else start allocating from f6
//     If a color is still not found because LR interferes with a call
//        Search in f0 - f6. If found mark for spill across calls.
//     If a color is still not fond, mark for spilling
//
//----------------------------------------------------------------------------
void SparcFloatRegClass::colorIGNode(IGNode * Node,bool IsColorUsedArr[]) const{
  LiveRange *LR = Node->getParentLR();
  unsigned NumNeighbors =  Node->getNumOfNeighbors();   // total # of neighbors

  for(unsigned n=0; n < NumNeighbors; n++) {            // for each neigh 
    IGNode *NeighIGNode = Node->getAdjIGNode(n);
    LiveRange *NeighLR = NeighIGNode->getParentLR();

      if( NeighLR->hasColor() )   {                     // if neigh has a color
      	IsColorUsedArr[ NeighLR->getColor() ] = true; // record that color
	if (NeighLR->getType() == Type::DoubleTy)
	  IsColorUsedArr[ (NeighLR->getColor()) + 1 ] = true;  
      }
      else if( NeighLR->hasSuggestedColor() )   {   // if neigh has sugg color

	if( NeighLR-> isSuggestedColorUsable() ) {

	  // if the neighbout can use the suggested color 
	  
	  IsColorUsedArr[ NeighLR->getSuggestedColor() ] = true;
	  if (NeighLR->getType() == Type::DoubleTy)
	    IsColorUsedArr[ (NeighLR->getSuggestedColor()) + 1 ] = true;  
	}

      }

  }


  // **NOTE: We don't check for call interferences in allocating suggested
  // color in this class since ALL registers are volatile. If this fact
  // changes, we should change the following part 
  //- see SparcIntRegClass::colorIGNode()

  if( LR->hasSuggestedColor() ) {
    if( ! IsColorUsedArr[ LR->getSuggestedColor() ] ) {
      LR->setColor(  LR->getSuggestedColor() );
      return;
    } else if (DEBUG_RA)  {                 // can't allocate the suggested col
      cerr << " Could NOT allocate the suggested color for LR ";
      printSet(*LR); cerr << "\n";
    }
  }


  int ColorFound = -1;               // have we found a color yet?
  bool isCallInterf = LR->isCallInterference();

  // if value is a double - search the double only reigon (f32 - f63)
  // i.e. we try to allocate f32 - f63 first for doubles since singles
  // cannot go there. By doing that, we provide more space for singles
  // in f0 - f31
  //
  if (LR->getType() == Type::DoubleTy)       
    ColorFound = findFloatColor( LR, 32, 64, IsColorUsedArr );
    

  if( ColorFound >= 0 ) {               // if we could find a color
    LR->setColor(ColorFound);                
    return;
  } else { 

    // if we didn't find a color becuase the LR was single precision or
    // all f32-f63 range is filled, we try to allocate a register from
    // the f0 - f31 region 

    unsigned SearchStart;                 // start pos of color in pref-order

    //if this Node is between calls (i.e., no call interferences )
    if( ! isCallInterf ) {
      // start with volatiles (we can  allocate volatiles safely)
      SearchStart = SparcFloatRegOrder::StartOfAllRegs;  
    }
    else {           
      // start with non volatiles (no non-volatiles)
      SearchStart =  SparcFloatRegOrder::StartOfNonVolatileRegs;  
    }
    
    ColorFound = findFloatColor( LR, SearchStart, 32, IsColorUsedArr );
  }



  if( ColorFound >= 0 ) {               // if we could find a color
    LR->setColor(ColorFound);                  
    return;
  }
  else if( isCallInterf ) { 

    // We are here because there is a call interference and no non-volatile
    // color could be found.
    // Now try to allocate even a volatile color

    ColorFound = findFloatColor( LR, SparcFloatRegOrder::StartOfAllRegs, 
				SparcFloatRegOrder::StartOfNonVolatileRegs,
				IsColorUsedArr);
  }



  if( ColorFound >= 0 ) {
    LR->setColor(ColorFound);         // first color found in preffered order
    LR->markForSaveAcrossCalls();  
  } else {
    // we are here because no color could be found
    LR->markForSpill();               // no color found - must spill
  }
}


//-----------------------------------------------------------------------------
// Helper method for coloring a node of Float Reg class.
// Finds the first available color in the range [Start,End] depending on the
// type of the Node (i.e., float/double)
//-----------------------------------------------------------------------------

int SparcFloatRegClass::findFloatColor(const LiveRange *LR, 
				       unsigned Start, unsigned End, 
				       bool IsColorUsedArr[]) const {
  bool ColorFound = false;
  unsigned c;

  if (LR->getType() == Type::DoubleTy) { 
    // find first unused color for a double 
    for (c=Start; c < End ; c+= 2)
      if (!IsColorUsedArr[c] && !IsColorUsedArr[c+1]) 
	return c;
  } else {
    // find first unused color for a single
    for (c = Start; c < End; c++)
      if (!IsColorUsedArr[c])
        return c;
  }
  
  return -1;
}
