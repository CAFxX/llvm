//===- BugDriver.h - Top-Level BugPoint class -------------------*- C++ -*-===//
//
// This class contains all of the shared state and information that is used by
// the BugPoint tool to track down errors in optimizations.  This class is the
// main driver class that invokes all sub-functionality.
//
//===----------------------------------------------------------------------===//

#ifndef BUGDRIVER_H
#define BUGDRIVER_H

#include <vector>
#include <string>
class PassInfo;
class Module;
class Function;

class BugDriver {
  const std::string ToolName;  // Name of bugpoint
  Module *Program;             // The raw program, linked together
  std::vector<const PassInfo*> PassesToRun;
public:
  BugDriver(const char *toolname) : ToolName(toolname), Program(0) {}

  // Set up methods... these methods are used to copy information about the
  // command line arguments into instance variables of BugDriver.
  //
  bool addSources(const std::vector<std::string> &FileNames);
  template<class It>
  void addPasses(It I, It E) { PassesToRun.insert(PassesToRun.end(), I, E); }

  /// run - The top level method that is invoked after all of the instance
  /// variables are set up from command line arguments.
  ///
  bool run();

  /// debugCrash - This method is called when some pass crashes on input.  It
  /// attempts to prune down the testcase to something reasonable, and figure
  /// out exactly which pass is crashing.
  ///
  bool debugCrash();

  /// debugPassCrash - This method is called when the specified pass crashes on
  /// Program as input.  It tries to reduce the testcase to something that still
  /// crashes, but it smaller.
  ///
  bool debugPassCrash(const PassInfo *PI);

  /// debugMiscompilation - This method is used when the passes selected are not
  /// crashing, but the generated output is semantically different from the
  /// input.
  bool debugMiscompilation();

private:
  /// ParseInputFile - Given a bytecode or assembly input filename, parse and
  /// return it, or return null if not possible.
  ///
  Module *ParseInputFile(const std::string &InputFilename) const;

  /// removeFile - Delete the specified file
  ///
  void removeFile(const std::string &Filename) const;

  /// writeProgramToFile - This writes the current "Program" to the named
  /// bytecode file.  If an error occurs, true is returned.
  ///
  bool writeProgramToFile(const std::string &Filename) const;


  /// EmitProgressBytecode - This function is used to output the current Program
  /// to a file named "bugpoing-ID.bc".
  ///
  void EmitProgressBytecode(const PassInfo *Pass, const std::string &ID);
  
  /// runPasses - Run the specified passes on Program, outputting a bytecode
  /// file and writting the filename into OutputFile if successful.  If the
  /// optimizations fail for some reason (optimizer crashes), return true,
  /// otherwise return false.  If DeleteOutput is set to true, the bytecode is
  /// deleted on success, and the filename string is undefined.  This prints to
  /// cout a single line message indicating whether compilation was successful
  /// or failed.
  ///
  bool runPasses(const std::vector<const PassInfo*> &PassesToRun,
                 std::string &OutputFilename, bool DeleteOutput = false) const;

  /// runPasses - Just like the method above, but this just returns true or
  /// false indicating whether or not the optimizer crashed on the specified
  /// input (true = crashed).
  ///
  bool runPasses(const std::vector<const PassInfo*> &PassesToRun,
                 bool DeleteOutput = true) const {
    std::string Filename;
    return runPasses(PassesToRun, Filename, DeleteOutput);
  }

  /// runPass - Run only the specified pass on the program.
  bool runPass(const PassInfo *P, bool DeleteOutput = true) const {
    return runPasses(std::vector<const PassInfo*>(1, P), DeleteOutput);
  }
  
  /// extractFunctionFromModule - This method is used to extract the specified
  /// (non-external) function from the current program, slim down the module,
  /// and then return it.  This does not modify Program at all, it modifies a
  /// copy, which it returns.
  Module *extractFunctionFromModule(Function *F) const;

};

#endif
