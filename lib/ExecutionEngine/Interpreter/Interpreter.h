//===-- Interpreter.h ------------------------------------------*- C++ -*--===//
//
// This header file defines the interpreter structure
//
//===----------------------------------------------------------------------===//

#ifndef LLI_INTERPRETER_H
#define LLI_INTERPRETER_H

// Uncomment this line to enable profiling of structure field accesses.
#define PROFILE_STRUCTURE_FIELDS 1


#include "llvm/Module.h"
#include "llvm/Method.h"
#include "llvm/BasicBlock.h"
#include "Support/DataTypes.h"
#include "llvm/Assembly/CachedWriter.h"

extern CachedWriter CW;     // Object to accellerate printing of LLVM

struct MethodInfo;          // Defined in ExecutionAnnotations.h
class CallInst;
class ReturnInst;
class BranchInst;
class AllocationInst;

typedef uint64_t PointerTy;

union GenericValue {
  bool            BoolVal;
  unsigned char   UByteVal;
  signed   char   SByteVal;
  unsigned short  UShortVal;
  signed   short  ShortVal;
  unsigned int    UIntVal;
  signed   int    IntVal;
  uint64_t        ULongVal;
  int64_t         LongVal;
  double          DoubleVal;
  float           FloatVal;
  PointerTy       PointerVal;
};

typedef std::vector<GenericValue> ValuePlaneTy;

// ExecutionContext struct - This struct represents one stack frame currently
// executing.
//
struct ExecutionContext {
  Method               *CurMethod;  // The currently executing method
  BasicBlock           *CurBB;      // The currently executing BB
  BasicBlock::iterator  CurInst;    // The next instruction to execute
  MethodInfo           *MethInfo;   // The MethInfo annotation for the method
  std::vector<ValuePlaneTy>  Values;// ValuePlanes for each type

  BasicBlock           *PrevBB;     // The previous BB or null if in first BB
  CallInst             *Caller;     // Holds the call that called subframes.
                                    // NULL if main func or debugger invoked fn
};

// Interpreter - This class represents the entirety of the interpreter.
//
class Interpreter {
  Module *CurMod;              // The current Module being executed (0 if none)
  int ExitCode;                // The exit code to be returned by the lli util
  bool Profile;                // Profiling enabled?
  bool Trace;                  // Tracing enabled?
  int CurFrame;                // The current stack frame being inspected

  // The runtime stack of executing code.  The top of the stack is the current
  // method record.
  std::vector<ExecutionContext> ECStack;

public:
  Interpreter();
  inline ~Interpreter() { CW.setModule(0); delete CurMod; }

  // getExitCode - return the code that should be the exit code for the lli
  // utility.
  inline int getExitCode() const { return ExitCode; }

  // enableProfiling() - Turn profiling on, clear stats?
  void enableProfiling() { Profile = true; }
  void enableTracing() { Trace = true; }

  void handleUserInput();

  // User Interation Methods...
  void loadModule(const std::string &Filename);
  bool flushModule();
  bool callMethod(const std::string &Name);      // return true on failure
  void setBreakpoint(const std::string &Name);
  void infoValue(const std::string &Name);
  void print(const std::string &Name);
  static void print(const Type *Ty, GenericValue V);
  static void printValue(const Type *Ty, GenericValue V);

  // Hack until we can parse command line args...
  bool callMainMethod(const std::string &MainName,
                      const std::vector<std::string> &InputFilename);

  void list();             // Do the 'list' command
  void printStackTrace();  // Do the 'backtrace' command

  // Code execution methods...
  void callMethod(Method *Meth, const std::vector<GenericValue> &ArgVals);
  bool executeInstruction(); // Execute one instruction...

  void stepInstruction();  // Do the 'step' command
  void nextInstruction();  // Do the 'next' command
  void run();              // Do the 'run' command
  void finish();           // Do the 'finish' command

  // Opcode Implementations
  void executeCallInst(CallInst *I, ExecutionContext &SF);
  void executeRetInst(ReturnInst *I, ExecutionContext &SF);
  void executeBrInst(BranchInst *I, ExecutionContext &SF);
  void executeAllocInst(AllocationInst *I, ExecutionContext &SF);
  GenericValue callExternalMethod(Method *Meth, 
                                  const std::vector<GenericValue> &ArgVals);
  void exitCalled(GenericValue GV);

  // getCurrentMethod - Return the currently executing method
  inline Method *getCurrentMethod() const {
    return CurFrame < 0 ? 0 : ECStack[CurFrame].CurMethod;
  }

  // isStopped - Return true if a program is stopped.  Return false if no
  // program is running.
  //
  inline bool isStopped() const { return !ECStack.empty(); }

private:  // Helper functions
  // getCurrentExecutablePath() - Return the directory that the lli executable
  // lives in.
  //
  std::string getCurrentExecutablePath() const;

  // printCurrentInstruction - Print out the instruction that the virtual PC is
  // at, or fail silently if no program is running.
  //
  void printCurrentInstruction();

  // printStackFrame - Print information about the specified stack frame, or -1
  // for the default one.
  //
  void printStackFrame(int FrameNo = -1);

  // LookupMatchingNames - Search the current method namespace, then the global
  // namespace looking for values that match the specified name.  Return ALL
  // matches to that name.  This is obviously slow, and should only be used for
  // user interaction.
  //
  std::vector<Value*> LookupMatchingNames(const std::string &Name);

  // ChooseOneOption - Prompt the user to choose among the specified options to
  // pick one value.  If no options are provided, emit an error.  If a single 
  // option is provided, just return that option.
  //
  Value *ChooseOneOption(const std::string &Name,
                         const std::vector<Value*> &Opts);


  void initializeExecutionEngine();
  void initializeExternalMethods();
};

#endif
