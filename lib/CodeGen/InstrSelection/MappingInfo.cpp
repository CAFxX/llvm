//===- MappingInfo.cpp - create LLVM info and output to .s file ---------===//
//
// Create Map from LLVM BB and Instructions and Machine Instructions
// and output the information as .byte directives to the .s file
//
//===--------------------------------------------------------------------===//

#include "llvm/CodeGen/MappingInfo.h"
#include "llvm/Pass.h"
#include "llvm/Module.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineCodeForBasicBlock.h"
#include "llvm/CodeGen/MachineCodeForInstruction.h"
#include <map>
#include <vector>
#include <iostream>
using std::vector;



// MappingInfo - This method collects mapping info 
// for the mapping from LLVM to machine code.
//
namespace {
  class getMappingInfoForFunction : public Pass { 
    std::ostream &Out;
  private:
    std::map<const Function*, int> Fkey; //key of function to num
    std::map<const MachineInstr*, int> BBkey; //key basic block to num
    std::map<const MachineInstr*, int> MIkey; //key machine instruction to num
    vector<vector<int> > BBmap;
    vector<vector<int> > MImap;

    void createFunctionKey(Module *M);
    void createBasicBlockKey(Module *M);    
    void createMachineInstructionKey(Module *M);
    void createBBToMImap(Module *M);
    void createLLVMToMImap(Module *M);
    void writeNumber(int X);

  public:
    getMappingInfoForFunction(std::ostream &out) : Out(out){}

    const char* getPassName() const {
      return "Sparc CollectMappingInfoForInstruction";
    }
    
    bool run(Module &M);
  };
}


//pass definition
Pass *MappingInfoForFunction(std::ostream &out){
  return (new getMappingInfoForFunction(out));
}

//function definitions :
//create and output maps to the .s file
bool getMappingInfoForFunction::run(Module &m) {
  Module *M = &m;

  //map for Function to Function number
  createFunctionKey(M);
      
  //map for BB to LLVM instruction number
  createBasicBlockKey(M);
      
  //map from Machine Instruction to Machine Instruction number
  createMachineInstructionKey(M);
      
  //map of Basic Block to first Machine Instruction and number 
  // of instructions go thro each function
  createBBToMImap(M);
  
  //map of LLVM Instruction to Machine Instruction 
  createLLVMToMImap(M);
  
  
  // Write map to the sparc assembly stream
  // Start by writing out the basic block to first and last
  // machine instruction map to the .s file
  Out << "\n\n!BB TO MI MAP\n";
  Out << "\t.section \".data\"\n\t.align 8\n";
  Out << "\t.global BBMIMap\n";
  Out << "BBMIMap:\n";
  //add stream object here that will contain info about the map
  //add object to write this out to the .s file
  for (vector<vector<int> >::iterator BBmapI = 
	 BBmap.begin(), BBmapE = BBmap.end(); BBmapI != BBmapE;
       ++BBmapI){
    writeNumber((*BBmapI)[0]);
    writeNumber((*BBmapI)[1]);
    writeNumber((*BBmapI)[2]);
    writeNumber((*BBmapI)[3]);
  }
  
  Out << "\t.type BBMIMap,#object\n";
  Out << "\t.size BBMIMap,"<<BBmap.size() << "\n";
  
  //output length info
  Out <<"\n\n!LLVM BB MAP Length\n\t.section \".bbdata";
  Out << "\",#alloc,#write\n\t.global BBMIMap_length\n\t.align 4\n\t.type BBMIMap_length,";
  Out <<"#object\n\t.size BBMIMap_length,4\nBBMIMap_length:\n\t.word "
      << ((BBmap.size())*4)<<"\n\n\n\n";
 

  //Now write out the LLVM instruction to the corresponding
  //machine instruction map
  Out << "!LLVM I TO MI MAP\n";
  Out << "\t.section\".data\"\n\t.align 8\n";
  Out << "\t.global LMIMap\n";
  Out << "LMIMap:\n";
  //add stream object here that will contain info about the map
  //add object to write this out to the .s file
  for (vector<vector<int> >::iterator MImapI = 
	 MImap.begin(), MImapE = MImap.end(); MImapI != MImapE;
       ++MImapI){
    writeNumber((*MImapI)[0]);
    writeNumber((*MImapI)[1]);
    writeNumber((*MImapI)[2]);
    writeNumber((*MImapI)[3]);
  }
  Out << "\t.type LMIMap,#object\n";
  Out << "\t.size LMIMap,"<<MImap.size() << "\n";
  //output length info
  Out <<"\n\n!LLVM MI MAP Length\n\t.section\".llvmdata";
  Out << "\",#alloc,#write\n\t.global LMIMap_length\n\t.align 4\n\t.type LMIMap_length,";
  Out <<"#object\n\t.size LMIMap_length,4\nLMIMap_length:\n\t.word "
      << ((MImap.size())*4)<<"\n\n\n\n";

  return false; 
}  

//write out information as .byte directives
void getMappingInfoForFunction::writeNumber(int X) {
  do {
    int tmp = X & 127;
    X >>= 7;
    if (X) tmp |= 128;
    Out << "\t.byte " << tmp << "\n";    
  } while(X);
}

//Assign a number to each Function 
void getMappingInfoForFunction::createFunctionKey(Module *M){
  int i = 0;
  int j = 0;
  for (Module::iterator FI = M->begin(), FE = M->end();
       FI != FE; ++FI){
    if(FI->size() <=1) continue;
    Fkey[FI] = i;
    ++i;
  }
}
     
//Assign a Number to each BB
void getMappingInfoForFunction::createBasicBlockKey(Module *M){
  int i = 0;
  for (Module::iterator FI = M->begin(), FE = M->end(); 
       FI != FE; ++FI){
    //	int i = 0;
    if(FI->size() <= 1) continue;
    for (Function::iterator BI = FI->begin(), BE = FI->end(); 
	 BI != BE; ++BI){
      MachineCodeForBasicBlock &miBB = MachineCodeForBasicBlock::get(BI);
      BBkey[miBB[0]] = i;
      i = i+(miBB.size());
    }
  }
}

void getMappingInfoForFunction::createMachineInstructionKey(Module *M){
  for (Module::iterator FI = M->begin(), FE = M->end(); 
       FI != FE; ++FI){
    if(FI->size() <= 1) continue;
    for (Function::iterator BI=FI->begin(), BE=FI->end(); 
	 BI != BE; ++BI){
      MachineCodeForBasicBlock &miBB = MachineCodeForBasicBlock::get(BI);
      int j = 0;
      for (MachineCodeForBasicBlock::iterator miI = miBB.begin(),
	     miE = miBB.end(); miI != miE; ++miI, ++j){
	MIkey[*miI] = j;
      }
    }
  }
}

void getMappingInfoForFunction::createBBToMImap(Module *M){
  //go thro each function in the module
  for (Module::iterator FI = M->begin(), FE = M->end();
       FI != FE; ++FI){	
    if(FI->size() <= 1)continue;
    //go thro each basic block in that function 
    int i = 0;
    for (Function::iterator BI = FI->begin(), 
	   BE = FI->end(); BI != BE; ++BI){
      //create a Map record
      //get the corresponding machine instruction 
      MachineCodeForBasicBlock &miBB = MachineCodeForBasicBlock::get(BI);
      //add record into the map
      BBmap.push_back(vector<int>());
      vector<int> &oneBB = BBmap.back();
      oneBB.reserve(4);

      //add the function number
      oneBB.push_back(Fkey[FI]);
      //add the machine instruction number
      oneBB.push_back( i );
      oneBB.push_back( BBkey[ miBB[0] ]);
      //add the number of instructions
      oneBB.push_back(miBB.size());
      ++i;

    }
  }
}

void getMappingInfoForFunction::createLLVMToMImap(Module *M){
  
  for (Module::iterator FI = M->begin(), FE = M->end();
       FI != FE; ++FI){
    if(FI->size() <= 1) continue;
    int i =0;
    for (Function::iterator BI = FI->begin(),  BE = FI->end(); 
	 BI != BE; ++BI, ++i){
      int j = 0;
      for (BasicBlock::iterator II = BI->begin(), 
	     IE = BI->end(); II != IE; ++II, ++j){
	MachineCodeForInstruction& miI = 
	  MachineCodeForInstruction::get(II);
	for (MachineCodeForInstruction::iterator miII = miI.begin(), 
	       miIE = miI.end(); miII != miIE; ++miII){
	  
	  vector<int> oneMI;
	  oneMI.push_back(Fkey[FI]);
	  oneMI.push_back(i);
	  oneMI.push_back(j);
	  oneMI.push_back(MIkey[*miII]);
	  MImap.push_back(oneMI);
	}
      }
    } 
  }
}


