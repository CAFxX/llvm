//===-- PluginLoader.cpp - Implement -load command line option ------------===//
//
// This file implements the -load <plugin> command line option processor.  When
// linked into a program, this new command line option is available that allows
// users to load shared objects into the running program.
//
// Note that there are no symbols exported by the .o file generated for this
// .cpp file.  Because of this, a program must link against support.o instead of
// support.a: otherwise this translation unit will not be included.
//
//===----------------------------------------------------------------------===//

#include "Support/CommandLine.h"
#include <dlfcn.h>
#include <link.h>

namespace {
  struct PluginLoader {
    void operator=(const std::string &Filename) {
      if (dlopen(Filename.c_str(), RTLD_NOW) == 0)
        std::cerr << "Error opening '" << Filename << "': " << dlerror()
                  << "\n  -load request ignored.\n";
    }
  };
}

// This causes operator= above to be invoked for every -load option.
static cl::opt<PluginLoader, false, cl::parser<string> >
LoadOpt("load", cl::ZeroOrMore, cl::value_desc("plugin.so"),
        cl::desc("Load the specified plugin"));
