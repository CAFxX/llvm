//===-- ExecutionAnnotations.h ---------------------------------*- C++ -*--===//
//
// This header file defines annotations used by the execution engine.
//
//===----------------------------------------------------------------------===//

#ifndef LLI_EXECUTION_ANNOTATIONS_H
#define LLI_EXECUTION_ANNOTATIONS_H

//===----------------------------------------------------------------------===//
// Support for MethodInfo annotations
//===----------------------------------------------------------------------===//

// This annotation (attached only to Function objects) is used to cache useful
// information about the function, including the number of types present in the
// function, and the number of values for each type.
//
// This annotation object is created on demand, and attaches other annotation
// objects to the instructions in the function when it's created.
//
static AnnotationID MethodInfoAID(
	            AnnotationManager::getID("Interpreter::FunctionInfo"));

struct MethodInfo : public Annotation {
  MethodInfo(Function *F);
  std::vector<unsigned> NumPlaneElements;


  // Create - Factory function to allow MethodInfo annotations to be
  // created on demand.
  //
  static Annotation *Create(AnnotationID AID, const Annotable *O, void *) {
    assert(AID == MethodInfoAID);
    return new MethodInfo(cast<Function>((Value*)O));  // Simply invoke the ctor
  }

private:
  unsigned getValueSlot(const Value *V);
};


//===----------------------------------------------------------------------===//
// Support for the SlotNumber annotation
//===----------------------------------------------------------------------===//

// This annotation (attached only to Argument & Instruction objects) is used to
// hold the the slot number for the value in its type plane.
//
// Entities have this annotation attached to them when the containing
// function has it's MethodInfo created (by the MethodInfo ctor).
//
static AnnotationID SlotNumberAID(
	            AnnotationManager::getID("Interpreter::SlotNumber"));

struct SlotNumber : public Annotation {
  unsigned SlotNum;   // Ranges from 0->

  SlotNumber(unsigned sn) : Annotation(SlotNumberAID), 
			    SlotNum(sn) {}
};




//===----------------------------------------------------------------------===//
// Support for the InstNumber annotation
//===----------------------------------------------------------------------===//

// This annotation (attached only to Instruction objects) is used to hold the
// instruction number of the instruction, and the slot number for the value in
// its type plane.  InstNumber's are used for user interaction, and for
// calculating which value slot to store the result of the instruction in.
//
// Instructions have this annotation attached to them when the containing
// function has it's MethodInfo created (by the MethodInfo ctor).
//
struct InstNumber : public SlotNumber {
  unsigned InstNum;   // Ranges from 1->

  InstNumber(unsigned in, unsigned sn) : SlotNumber(sn), InstNum(in) {}
};


//===----------------------------------------------------------------------===//
// Support for the Breakpoint annotation
//===----------------------------------------------------------------------===//

static AnnotationID BreakpointAID(
	            AnnotationManager::getID("Interpreter::Breakpoint"));
// Just use an Annotation directly, Breakpoint is currently just a marker


//===----------------------------------------------------------------------===//
// Support for the GlobalAddress annotation
//===----------------------------------------------------------------------===//

// This annotation (attached only to GlobalValue objects) is used to hold the
// address of the chunk of memory that represents a global value.  For
// Functions, this pointer is the Function object pointer that represents it.
// For global variables, this is the dynamically allocated (and potentially
// initialized) chunk of memory for the global.  This annotation is created on
// demand.
//
static AnnotationID GlobalAddressAID(
	            AnnotationManager::getID("Interpreter::GlobalAddress"));

struct GlobalAddress : public Annotation {
  void *Ptr;   // The pointer itself
  bool Delete; // Should I delete them memory on destruction?

  GlobalAddress(void *ptr, bool d) : Annotation(GlobalAddressAID), Ptr(ptr), 
                                     Delete(d) {}
  ~GlobalAddress() { if (Delete) free(Ptr); }
  
  // Create - Factory function to allow GlobalAddress annotations to be
  // created on demand.
  //
  static Annotation *Create(AnnotationID AID, const Annotable *O, void *);
};

#endif
