//===-- CommandLine.cpp - Command line parser implementation --------------===//
//
// This class implements a command line argument processor that is useful when
// creating a tool.  It provides a simple, minimalistic interface that is easily
// extensible and supports nonlocal (library) command line options.
//
// Note that rather than trying to figure out what this code does, you could try
// reading the library documentation located in docs/CommandLine.html
//
//===----------------------------------------------------------------------===//

#include "Support/CommandLine.h"
#include "Support/STLExtras.h"
#include <algorithm>
#include <map>
#include <set>
#include <iostream>

using namespace cl;
using std::map;
using std::pair;
using std::vector;
using std::string;
using std::cerr;

//===----------------------------------------------------------------------===//
// Basic, shared command line option processing machinery...
//

// Return the global command line option vector.  Making it a function scoped
// static ensures that it will be initialized correctly before its first use.
//
static map<string, Option*> &getOpts() {
  static map<string,Option*> CommandLineOptions;
  return CommandLineOptions;
}

static vector<Option*> &getPositionalOpts() {
  static vector<Option*> Positional;
  return Positional;
}

static void AddArgument(const string &ArgName, Option *Opt) {
  if (getOpts().find(ArgName) != getOpts().end()) {
    cerr << "CommandLine Error: Argument '" << ArgName
	 << "' defined more than once!\n";
  } else {
    // Add argument to the argument map!
    getOpts().insert(std::make_pair(ArgName, Opt));
  }
}

static const char *ProgramName = 0;
static const char *ProgramOverview = 0;

static inline bool ProvideOption(Option *Handler, const char *ArgName,
                                 const char *Value, int argc, char **argv,
                                 int &i) {
  // Enforce value requirements
  switch (Handler->getValueExpectedFlag()) {
  case ValueRequired:
    if (Value == 0 || *Value == 0) {  // No value specified?
      if (i+1 < argc) {     // Steal the next argument, like for '-o filename'
        Value = argv[++i];
      } else {
        return Handler->error(" requires a value!");
      }
    }
    break;
  case ValueDisallowed:
    if (*Value != 0)
      return Handler->error(" does not allow a value! '" + 
                            string(Value) + "' specified.");
    break;
  case ValueOptional: break;
  default: cerr << "Bad ValueMask flag! CommandLine usage error:" 
                << Handler->getValueExpectedFlag() << "\n"; abort();
  }

  // Run the handler now!
  return Handler->addOccurance(ArgName, Value);
}

static bool ProvidePositionalOption(Option *Handler, string &Arg) {
  int Dummy;
  return ProvideOption(Handler, "", Arg.c_str(), 0, 0, Dummy);
}


// Option predicates...
static inline bool isGrouping(const Option *O) {
  return O->getFormattingFlag() == cl::Grouping;
}
static inline bool isPrefixedOrGrouping(const Option *O) {
  return isGrouping(O) || O->getFormattingFlag() == cl::Prefix;
}

// getOptionPred - Check to see if there are any options that satisfy the
// specified predicate with names that are the prefixes in Name.  This is
// checked by progressively stripping characters off of the name, checking to
// see if there options that satisfy the predicate.  If we find one, return it,
// otherwise return null.
//
static Option *getOptionPred(std::string Name, unsigned &Length,
                             bool (*Pred)(const Option*)) {
  
  map<string, Option*>::iterator I = getOpts().find(Name);
  if (I != getOpts().end() && Pred(I->second)) {
    Length = Name.length();
    return I->second;
  }

  if (Name.size() == 1) return 0;
  do {
    Name.erase(Name.end()-1, Name.end());   // Chop off the last character...
    I = getOpts().find(Name);

    // Loop while we haven't found an option and Name still has at least two
    // characters in it (so that the next iteration will not be the empty
    // string...
  } while ((I == getOpts().end() || !Pred(I->second)) && Name.size() > 1);

  if (I != getOpts().end() && Pred(I->second)) {
    Length = Name.length();
    return I->second;      // Found one!
  }
  return 0;                // No option found!
}

static bool RequiresValue(const Option *O) {
  return O->getNumOccurancesFlag() == cl::Required ||
         O->getNumOccurancesFlag() == cl::OneOrMore;
}

static bool EatsUnboundedNumberOfValues(const Option *O) {
  return O->getNumOccurancesFlag() == cl::ZeroOrMore ||
         O->getNumOccurancesFlag() == cl::OneOrMore;
}

void cl::ParseCommandLineOptions(int &argc, char **argv,
                                 const char *Overview = 0) {
  assert((!getOpts().empty() || !getPositionalOpts().empty()) &&
         "No options specified, or ParseCommandLineOptions called more"
         " than once!");
  ProgramName = argv[0];  // Save this away safe and snug
  ProgramOverview = Overview;
  bool ErrorParsing = false;

  map<string, Option*> &Opts = getOpts();
  vector<Option*> &PositionalOpts = getPositionalOpts();

  // Check out the positional arguments to collect information about them.
  unsigned NumPositionalRequired = 0;
  Option *ConsumeAfterOpt = 0;
  if (!PositionalOpts.empty()) {
    if (PositionalOpts[0]->getNumOccurancesFlag() == cl::ConsumeAfter) {
      assert(PositionalOpts.size() > 1 &&
             "Cannot specify cl::ConsumeAfter without a positional argument!");
      ConsumeAfterOpt = PositionalOpts[0];
    }

    // Calculate how many positional values are _required_.
    bool UnboundedFound = false;
    for (unsigned i = ConsumeAfterOpt != 0, e = PositionalOpts.size();
         i != e; ++i) {
      Option *Opt = PositionalOpts[i];
      if (RequiresValue(Opt))
        ++NumPositionalRequired;
      else if (ConsumeAfterOpt) {
        // ConsumeAfter cannot be combined with "optional" positional options
        // unless there is only one positional argument...
        if (PositionalOpts.size() > 2)
          ErrorParsing |=
            Opt->error(" error - this positional option will never be matched, "
                       "because it does not Require a value, and a "
                       "cl::ConsumeAfter option is active!");
      } else if (UnboundedFound) {  // This option does not "require" a value...
        // Make sure this option is not specified after an option that eats all
        // extra arguments, or this one will never get any!
        //
        ErrorParsing |= Opt->error(" error - option can never match, because "
                                   "another positional argument will match an "
                                   "unbounded number of values, and this option"
                                   " does not require a value!");
      }
      UnboundedFound |= EatsUnboundedNumberOfValues(Opt);
    }
  }

  // PositionalVals - A vector of "positional" arguments we accumulate into to
  // processes at the end...
  //
  vector<string> PositionalVals;

  // Loop over all of the arguments... processing them.
  bool DashDashFound = false;  // Have we read '--'?
  for (int i = 1; i < argc; ++i) {
    Option *Handler = 0;
    const char *Value = "";
    const char *ArgName = "";

    // Check to see if this is a positional argument.  This argument is
    // considered to be positional if it doesn't start with '-', if it is "-"
    // itself, or if we have see "--" already.
    //
    if (argv[i][0] != '-' || argv[i][1] == 0 || DashDashFound) {
      // Positional argument!
      if (!PositionalOpts.empty()) {
        PositionalVals.push_back(argv[i]);

        // All of the positional arguments have been fulfulled, give the rest to
        // the consume after option... if it's specified...
        //
        if (PositionalVals.size() == NumPositionalRequired && 
            ConsumeAfterOpt != 0) {
          for (++i; i < argc; ++i)
            PositionalVals.push_back(argv[i]);
          break;   // Handle outside of the argument processing loop...
        }

        // Delay processing positional arguments until the end...
        continue;
      }
    } else {               // We start with a '-', must be an argument...
      ArgName = argv[i]+1;
      while (*ArgName == '-') ++ArgName;  // Eat leading dashes

      if (*ArgName == 0 && !DashDashFound) {   // Is this the mythical "--"?
        DashDashFound = true;  // Yup, take note of that fact...
        continue;              // Don't try to process it as an argument iself.
      }

      const char *ArgNameEnd = ArgName;
      while (*ArgNameEnd && *ArgNameEnd != '=')
	++ArgNameEnd; // Scan till end of argument name...

      Value = ArgNameEnd;
      if (*Value)           // If we have an equals sign...
	++Value;            // Advance to value...

      if (*ArgName != 0) {
	string RealName(ArgName, ArgNameEnd);
	// Extract arg name part
        map<string, Option*>::iterator I = Opts.find(RealName);

	if (I == Opts.end() && !*Value && RealName.size() > 1) {
          // Check to see if this "option" is really a prefixed or grouped
          // argument...
          //
          unsigned Length = 0;
          Option *PGOpt = getOptionPred(RealName, Length, isPrefixedOrGrouping);

          // If the option is a prefixed option, then the value is simply the
          // rest of the name...  so fall through to later processing, by
          // setting up the argument name flags and value fields.
          //
          if (PGOpt && PGOpt->getFormattingFlag() == cl::Prefix) {
            ArgNameEnd = ArgName+Length;
            Value = ArgNameEnd;
            I = Opts.find(string(ArgName, ArgNameEnd));
            assert(I->second == PGOpt);
          } else if (PGOpt) {
            // This must be a grouped option... handle all of them now...
            assert(isGrouping(PGOpt) && "Broken getOptionPred!");

            do {
              // Move current arg name out of RealName into RealArgName...
              string RealArgName(RealName.begin(), RealName.begin()+Length);
              RealName.erase(RealName.begin(), RealName.begin()+Length);

	      // Because ValueRequired is an invalid flag for grouped arguments,
	      // we don't need to pass argc/argv in...
	      //
              assert(PGOpt->getValueExpectedFlag() != cl::ValueRequired &&
                     "Option can not be cl::Grouping AND cl::ValueRequired!");
              int Dummy;
	      ErrorParsing |= ProvideOption(PGOpt, RealArgName.c_str(), "",
                                            0, 0, Dummy);

              // Get the next grouping option...
              if (!RealName.empty())
                PGOpt = getOptionPred(RealName, Length, isGrouping);
            } while (!RealName.empty() && PGOpt);

            if (RealName.empty())    // Processed all of the options, move on
              continue;              // to the next argv[] value...

            // If RealName is not empty, that means we did not match one of the
            // options!  This is an error.
            //
            I = Opts.end();
          }
	}

        Handler = I != Opts.end() ? I->second : 0;
      }
    }

    if (Handler == 0) {
      cerr << "Unknown command line argument '" << argv[i] << "'.  Try: "
	   << argv[0] << " --help'\n";
      ErrorParsing = true;
      continue;
    }

    ErrorParsing |= ProvideOption(Handler, ArgName, Value, argc, argv, i);
  }

  // Check and handle positional arguments now...
  if (NumPositionalRequired > PositionalVals.size()) {
    cerr << "Not enough positional command line arguments specified!\n";
    cerr << "Must specify at least " << NumPositionalRequired
         << " positional arguments: See: " << argv[0] << " --help\n";
    ErrorParsing = true;


  } else if (ConsumeAfterOpt == 0) {
    // Positional args have already been handled if ConsumeAfter is specified...
    unsigned ValNo = 0, NumVals = PositionalVals.size();
    for (unsigned i = 0, e = PositionalOpts.size(); i != e; ++i) {
      if (RequiresValue(PositionalOpts[i])) {
        ProvidePositionalOption(PositionalOpts[i], PositionalVals[ValNo++]);
        --NumPositionalRequired;  // We fulfilled our duty...
      }

      // If we _can_ give this option more arguments, do so now, as long as we
      // do not give it values that others need.  'Done' controls whether the
      // option even _WANTS_ any more.
      //
      bool Done = PositionalOpts[i]->getNumOccurancesFlag() == cl::Required;
      while (NumVals-ValNo > NumPositionalRequired && !Done) {
        switch (PositionalOpts[i]->getNumOccurancesFlag()) {
        case cl::Optional:
          Done = true;          // Optional arguments want _at most_ one value
          // FALL THROUGH
        case cl::ZeroOrMore:    // Zero or more will take all they can get...
        case cl::OneOrMore:     // One or more will take all they can get...
          ProvidePositionalOption(PositionalOpts[i], PositionalVals[ValNo++]);
          break;
        default:
          assert(0 && "Internal error, unexpected NumOccurances flag in "
                 "positional argument processing!");
        }
      }
    }
  } else {
    assert(ConsumeAfterOpt && NumPositionalRequired <= PositionalVals.size());
    unsigned ValNo = 0;
    for (unsigned j = 1, e = PositionalOpts.size(); j != e; ++j)
      if (RequiresValue(PositionalOpts[j]))
        ErrorParsing |=
          ProvidePositionalOption(PositionalOpts[j], PositionalVals[ValNo++]);
    
    // Handle over all of the rest of the arguments to the
    // cl::ConsumeAfter command line option...
    for (; ValNo != PositionalVals.size(); ++ValNo)
      ErrorParsing |= ProvidePositionalOption(ConsumeAfterOpt,
                                              PositionalVals[ValNo]);
  }

  // Loop over args and make sure all required args are specified!
  for (map<string, Option*>::iterator I = Opts.begin(), 
	 E = Opts.end(); I != E; ++I) {
    switch (I->second->getNumOccurancesFlag()) {
    case Required:
    case OneOrMore:
      if (I->second->getNumOccurances() == 0) {
	I->second->error(" must be specified at least once!");
        ErrorParsing = true;
      }
      // Fall through
    default:
      break;
    }
  }

  // Free all of the memory allocated to the map.  Command line options may only
  // be processed once!
  Opts.clear();
  PositionalOpts.clear();

  // If we had an error processing our arguments, don't let the program execute
  if (ErrorParsing) exit(1);
}

//===----------------------------------------------------------------------===//
// Option Base class implementation
//

bool Option::error(string Message, const char *ArgName = 0) {
  if (ArgName == 0) ArgName = ArgStr;
  if (ArgName[0] == 0)
    cerr << HelpStr;  // Be nice for positional arguments
  else
    cerr << "-" << ArgName;
  cerr << " option" << Message << "\n";
  return true;
}

bool Option::addOccurance(const char *ArgName, const string &Value) {
  NumOccurances++;   // Increment the number of times we have been seen

  switch (getNumOccurancesFlag()) {
  case Optional:
    if (NumOccurances > 1)
      return error(": may only occur zero or one times!", ArgName);
    break;
  case Required:
    if (NumOccurances > 1)
      return error(": must occur exactly one time!", ArgName);
    // Fall through
  case OneOrMore:
  case ZeroOrMore:
  case ConsumeAfter: break;
  default: return error(": bad num occurances flag value!");
  }

  return handleOccurance(ArgName, Value);
}

// addArgument - Tell the system that this Option subclass will handle all
// occurances of -ArgStr on the command line.
//
void Option::addArgument(const char *ArgStr) {
  if (ArgStr[0])
    AddArgument(ArgStr, this);
  else if (getFormattingFlag() == Positional)
    getPositionalOpts().push_back(this);
  else if (getNumOccurancesFlag() == ConsumeAfter) {
    assert((getPositionalOpts().empty() ||
            getPositionalOpts().front()->getNumOccurancesFlag() != ConsumeAfter)
           && "Cannot specify more than one option with cl::ConsumeAfter "
           "specified!");
    getPositionalOpts().insert(getPositionalOpts().begin(), this);
  }
}


// getValueStr - Get the value description string, using "DefaultMsg" if nothing
// has been specified yet.
//
static const char *getValueStr(const Option &O, const char *DefaultMsg) {
  if (O.ValueStr[0] == 0) return DefaultMsg;
  return O.ValueStr;
}

//===----------------------------------------------------------------------===//
// cl::alias class implementation
//

// Return the width of the option tag for printing...
unsigned alias::getOptionWidth() const {
  return std::strlen(ArgStr)+6;
}

// Print out the option for the alias...
void alias::printOptionInfo(unsigned GlobalWidth) const {
  unsigned L = std::strlen(ArgStr);
  cerr << "  -" << ArgStr << string(GlobalWidth-L-6, ' ') << " - "
       << HelpStr << "\n";
}



//===----------------------------------------------------------------------===//
// Parser Implementation code...
//

// parser<bool> implementation
//
bool parser<bool>::parseImpl(Option &O, const string &Arg, bool &Value) {
  if (Arg == "" || Arg == "true" || Arg == "TRUE" || Arg == "True" || 
      Arg == "1") {
    Value = true;
  } else if (Arg == "false" || Arg == "FALSE" || Arg == "False" || Arg == "0") {
    Value = false;
  } else {
    return O.error(": '" + Arg +
                   "' is invalid value for boolean argument! Try 0 or 1");
  }
  return false;
}

// Return the width of the option tag for printing...
unsigned parser<bool>::getOptionWidth(const Option &O) const {
  return std::strlen(O.ArgStr)+6;
}

// printOptionInfo - Print out information about this option.  The 
// to-be-maintained width is specified.
//
void parser<bool>::printOptionInfo(const Option &O, unsigned GlobalWidth) const{
  unsigned L = std::strlen(O.ArgStr);
  cerr << "  -" << O.ArgStr << string(GlobalWidth-L-6, ' ') << " - "
       << O.HelpStr << "\n";
}



// parser<int> implementation
//
bool parser<int>::parseImpl(Option &O, const string &Arg, int &Value) {
  const char *ArgStart = Arg.c_str();
  char *End;
  Value = (int)strtol(ArgStart, &End, 0);
  if (*End != 0) 
    return O.error(": '" + Arg + "' value invalid for integer argument!");
  return false;
}

// Return the width of the option tag for printing...
unsigned parser<int>::getOptionWidth(const Option &O) const {
  return std::strlen(O.ArgStr)+std::strlen(getValueStr(O, "int"))+9;
}

// printOptionInfo - Print out information about this option.  The 
// to-be-maintained width is specified.
//
void parser<int>::printOptionInfo(const Option &O, unsigned GlobalWidth) const{
  cerr << "  -" << O.ArgStr << "=<" << getValueStr(O, "int") << ">"
       << string(GlobalWidth-getOptionWidth(O), ' ') << " - "
       << O.HelpStr << "\n";
}


// parser<double> implementation
//
bool parser<double>::parseImpl(Option &O, const string &Arg, double &Value) {
  const char *ArgStart = Arg.c_str();
  char *End;
  Value = strtod(ArgStart, &End);
  if (*End != 0) 
    return O.error(": '" +Arg+ "' value invalid for floating point argument!");
  return false;
}

// Return the width of the option tag for printing...
unsigned parser<double>::getOptionWidth(const Option &O) const {
  return std::strlen(O.ArgStr)+std::strlen(getValueStr(O, "number"))+9;
}

// printOptionInfo - Print out information about this option.  The 
// to-be-maintained width is specified.
//
void parser<double>::printOptionInfo(const Option &O,
                                     unsigned GlobalWidth) const{
  cerr << "  -" << O.ArgStr << "=<" << getValueStr(O, "number") << ">"
       << string(GlobalWidth-getOptionWidth(O), ' ')
       << " - " << O.HelpStr << "\n";
}


// parser<string> implementation
//

// Return the width of the option tag for printing...
unsigned parser<string>::getOptionWidth(const Option &O) const {
  return std::strlen(O.ArgStr)+std::strlen(getValueStr(O, "string"))+9;
}

// printOptionInfo - Print out information about this option.  The 
// to-be-maintained width is specified.
//
void parser<string>::printOptionInfo(const Option &O,
                                     unsigned GlobalWidth) const{
  cerr << "  -" << O.ArgStr << " <" << getValueStr(O, "string") << ">"
       << string(GlobalWidth-getOptionWidth(O), ' ')
       << " - " << O.HelpStr << "\n";
}

// generic_parser_base implementation
//

// Return the width of the option tag for printing...
unsigned generic_parser_base::getOptionWidth(const Option &O) const {
  if (O.hasArgStr()) {
    unsigned Size = std::strlen(O.ArgStr)+6;
    for (unsigned i = 0, e = getNumOptions(); i != e; ++i)
      Size = std::max(Size, (unsigned)std::strlen(getOption(i))+8);
    return Size;
  } else {
    unsigned BaseSize = 0;
    for (unsigned i = 0, e = getNumOptions(); i != e; ++i)
      BaseSize = std::max(BaseSize, (unsigned)std::strlen(getOption(i))+8);
    return BaseSize;
  }
}

// printOptionInfo - Print out information about this option.  The 
// to-be-maintained width is specified.
//
void generic_parser_base::printOptionInfo(const Option &O,
                                          unsigned GlobalWidth) const {
  if (O.hasArgStr()) {
    unsigned L = std::strlen(O.ArgStr);
    cerr << "  -" << O.ArgStr << string(GlobalWidth-L-6, ' ')
         << " - " << O.HelpStr << "\n";

    for (unsigned i = 0, e = getNumOptions(); i != e; ++i) {
      unsigned NumSpaces = GlobalWidth-strlen(getOption(i))-8;
      cerr << "    =" << getOption(i) << string(NumSpaces, ' ') << " - "
           << getDescription(i) << "\n";
    }
  } else {
    if (O.HelpStr[0])
      cerr << "  " << O.HelpStr << "\n"; 
    for (unsigned i = 0, e = getNumOptions(); i != e; ++i) {
      unsigned L = std::strlen(getOption(i));
      cerr << "    -" << getOption(i) << string(GlobalWidth-L-8, ' ') << " - "
           << getDescription(i) << "\n";
    }
  }
}


//===----------------------------------------------------------------------===//
// --help and --help-hidden option implementation
//
namespace {

class HelpPrinter {
  unsigned MaxArgLen;
  const Option *EmptyArg;
  const bool ShowHidden;

  // isHidden/isReallyHidden - Predicates to be used to filter down arg lists.
  inline static bool isHidden(pair<string, Option *> &OptPair) {
    return OptPair.second->getOptionHiddenFlag() >= Hidden;
  }
  inline static bool isReallyHidden(pair<string, Option *> &OptPair) {
    return OptPair.second->getOptionHiddenFlag() == ReallyHidden;
  }

public:
  HelpPrinter(bool showHidden) : ShowHidden(showHidden) {
    EmptyArg = 0;
  }

  void operator=(bool Value) {
    if (Value == false) return;

    // Copy Options into a vector so we can sort them as we like...
    vector<pair<string, Option*> > Options;
    copy(getOpts().begin(), getOpts().end(), std::back_inserter(Options));

    // Eliminate Hidden or ReallyHidden arguments, depending on ShowHidden
    Options.erase(std::remove_if(Options.begin(), Options.end(), 
                         std::ptr_fun(ShowHidden ? isReallyHidden : isHidden)),
		  Options.end());

    // Eliminate duplicate entries in table (from enum flags options, f.e.)
    {  // Give OptionSet a scope
      std::set<Option*> OptionSet;
      for (unsigned i = 0; i != Options.size(); ++i)
        if (OptionSet.count(Options[i].second) == 0)
          OptionSet.insert(Options[i].second);   // Add new entry to set
        else
          Options.erase(Options.begin()+i--);    // Erase duplicate
    }

    if (ProgramOverview)
      cerr << "OVERVIEW:" << ProgramOverview << "\n";

    cerr << "USAGE: " << ProgramName << " [options]";

    // Print out the positional options...
    vector<Option*> &PosOpts = getPositionalOpts();
    Option *CAOpt = 0;   // The cl::ConsumeAfter option, if it exists...
    if (!PosOpts.empty() && PosOpts[0]->getNumOccurancesFlag() == ConsumeAfter)
      CAOpt = PosOpts[0];

    for (unsigned i = CAOpt != 0, e = PosOpts.size(); i != e; ++i) {
      cerr << " " << PosOpts[i]->HelpStr;
      switch (PosOpts[i]->getNumOccurancesFlag()) {
      case Optional:    cerr << "?"; break;
      case ZeroOrMore:  cerr << "*"; break;
      case Required:    break;
      case OneOrMore:   cerr << "+"; break;
      case ConsumeAfter:
      default:
        assert(0 && "Unknown NumOccurances Flag Value!");
      }
    }

    // Print the consume after option info if it exists...
    if (CAOpt) cerr << " " << CAOpt->HelpStr;

    cerr << "\n\n";

    // Compute the maximum argument length...
    MaxArgLen = 0;
    for (unsigned i = 0, e = Options.size(); i != e; ++i)
      MaxArgLen = std::max(MaxArgLen, Options[i].second->getOptionWidth());

    cerr << "OPTIONS:\n";
    for (unsigned i = 0, e = Options.size(); i != e; ++i)
      Options[i].second->printOptionInfo(MaxArgLen);

    // Halt the program if help information is printed
    exit(1);
  }
};



// Define the two HelpPrinter instances that are used to print out help, or
// help-hidden...
//
HelpPrinter NormalPrinter(false);
HelpPrinter HiddenPrinter(true);

cl::opt<HelpPrinter, true, parser<bool> > 
HOp("help", cl::desc("display available options (--help-hidden for more)"),
    cl::location(NormalPrinter));

cl::opt<HelpPrinter, true, parser<bool> >
HHOp("help-hidden", cl::desc("display all available options"),
     cl::location(HiddenPrinter), cl::Hidden);

} // End anonymous namespace
