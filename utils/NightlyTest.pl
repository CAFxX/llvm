#!/usr/bin/perl -w
#
# Program:  NightlyTest.pl
#
# Synopsis: Perform a series of tests which are designed to be run nightly.
#           This is used to keep track of the status of the LLVM tree, tracking
#           regressions and performance changes.  This generates one web page a
#           day which can be used to access this information.
#
# Syntax:   NightlyTest.pl [OPTIONS] [CVSROOT BUILDDIR WEBDIR]
#   where
# OPTIONS may include one or more of the following:
#  -nocheckout      Do not create, checkout, update, or configure
#                   the source tree.
#  -noremove        Do not remove the BUILDDIR after it has been built.
#  -nofeaturetests  Do not run the feature tests.
#  -noregressiontests Do not run the regression tests.
#  -notest          Do not even attempt to run the test programs. Implies
#                   -norunningtests.
#  -norunningtests  Do not run the Olden benchmark suite with
#                   LARGE_PROBLEM_SIZE enabled.
#  -noexternals     Do not run the external tests (for cases where povray
#                   or SPEC are not installed)
#  -rundejagnu      Runs features and regressions using Dejagnu
#  -parallel        Run two parallel jobs with GNU Make.
#  -release         Build an LLVM Release version
#  -pedantic        Enable additional GCC warnings to detect possible errors.
#  -enable-linscan  Enable linearscan tests
#  -disable-llc     Disable LLC tests in the nightly tester.
#  -disable-jit     Disable JIT tests in the nightly tester.
#  -verbose         Turn on some debug output
#  -debug           Print information useful only to maintainers of this script.
#  -nice            Checkout/Configure/Build with "nice" to reduce impact 
#                   on busy servers.
#  -f2c             Next argument specifies path to F2C utility
#  -gnuplotscript   Next argument specifies gnuplot script to use
#  -templatefile    Next argument specifies template file to use
#  -gccpath         Path to gcc/g++ used to build LLVM
#
# CVSROOT is the CVS repository from which the tree will be checked out,
#  specified either in the full :method:user@host:/dir syntax, or
#  just /dir if using a local repo.
# BUILDDIR is the directory where sources for this test run will be checked out
#  AND objects for this test run will be built. This directory MUST NOT
#  exist before the script is run; it will be created by the cvs checkout
#  process and erased (unless -noremove is specified; see above.)
# WEBDIR is the directory into which the test results web page will be written,
#  AND in which the "index.html" is assumed to be a symlink to the most recent
#  copy of the results. This directory will be created if it does not exist.
# LLVMGCCDIR is the directory in which the LLVM GCC Front End is installed
#  to. This is the same as you would have for a normal LLVM build.
#
use POSIX qw(strftime);
use File::Copy;

my $HOME = $ENV{'HOME'};
my $CVSRootDir = $ENV{'CVSROOT'};
   $CVSRootDir = "/home/vadve/shared/PublicCVS"
     unless $CVSRootDir;
my $BuildDir   = $ENV{'BUILDDIR'};
   $BuildDir   = "$HOME/buildtest"
     unless $BuildDir;
my $WebDir     = $ENV{'WEBDIR'};
   $WebDir     = "$HOME/cvs/testresults-X86"
     unless $WebDir;

# Calculate the date prefix...
@TIME = localtime;
my $DATE = sprintf "%4d-%02d-%02d", $TIME[5]+1900, $TIME[4]+1, $TIME[3];
my $DateString = strftime "%B %d, %Y", localtime;
my $TestStartTime = gmtime;

# Command line argument settings...
my $NOCHECKOUT = 0;
my $NOREMOVE = 0;
my $NOFEATURES = 0;
my $NOREGRESSIONS = 0;
my $NOTEST = 0;
my $NORUNNINGTESTS = 0;
my $NOEXTERNALS = 0;
my $MAKEOPTS = "";
my $PROGTESTOPTS = "";
my $VERBOSE = 0;
my $DEBUG = 0;
my $CONFIGUREARGS = "";
my $NICE = "";
my $RUNDEJAGNU = 0;

sub ReadFile {
  if (open (FILE, $_[0])) {
    undef $/;
    my $Ret = <FILE>;
    close FILE;
    $/ = '\n';
    return $Ret;
  } else {
    print "Could not open file '$_[0]' for reading!";
    return "";
  }
}

sub WriteFile {  # (filename, contents)
  open (FILE, ">$_[0]") or die "Could not open file '$_[0]' for writing!";
  print FILE $_[1];
  close FILE;
}

sub GetRegex {   # (Regex with ()'s, value)
  $_[1] =~ /$_[0]/m;
  if (defined($1)) {
    return $1;
  }
  return "0";
}

sub Touch {
  my @files = @_;
  my $now = time;
  foreach my $file (@files) {
    if (! -f $file) {
      open (FILE, ">$file") or warn "Could not create new file $file";
      close FILE;
    }
    utime $now, $now, $file;
  }
}

sub AddRecord {
  my ($Val, $Filename) = @_;
  my @Records;
  if (open FILE, "$WebDir/$Filename") {
    @Records = grep !/$DATE/, split "\n", <FILE>;
    close FILE;
  }
  push @Records, "$DATE: $Val";
  WriteFile "$WebDir/$Filename", (join "\n", @Records) . "\n";
}

sub AddPreTag {  # Add pre tags around nonempty list, or convert to "none"
  $_ = shift;
  if (length) { return "<ul><pre>$_</pre></ul>"; } else { "<b>none</b><br>"; }
}

sub ChangeDir { # directory, logical name
  my ($dir,$name) = @_;
  chomp($dir);
  if ( $VERBOSE ) { print "Changing To: $name ($dir)\n"; }
  chdir($dir) || die "Cannot change directory to: $name ($dir) ";
}

sub CopyFile { #filename, newfile
  my ($file, $newfile) = @_;
  chomp($file);
  if ($VERBOSE) { print "Copying $file to $newfile\n"; }
  copy($file, $newfile);
}

sub GetDir {
  my $Suffix = shift;
  opendir DH, $WebDir;
  my @Result = reverse sort grep !/$DATE/, grep /[-0-9]+$Suffix/, readdir DH;
  closedir DH;
  return @Result;
}

# DiffFiles - Diff the current version of the file against the last version of
# the file, reporting things added and removed.  This is used to report, for
# example, added and removed warnings.  This returns a pair (added, removed)
#
sub DiffFiles {
  my $Suffix = shift;
  my @Others = GetDir $Suffix;
  if (@Others == 0) {  # No other files?  We added all entries...
    return (`cat $WebDir/$DATE$Suffix`, "");
  }
  # Diff the files now...
  my @Diffs = split "\n", `diff $WebDir/$DATE$Suffix $WebDir/$Others[0]`;
  my $Added   = join "\n", grep /^</, @Diffs;
  my $Removed = join "\n", grep /^>/, @Diffs;
  $Added =~ s/^< //gm;
  $Removed =~ s/^> //gm;
  return ($Added, $Removed);
}

# FormatTime - Convert a time from 1m23.45 into 83.45
sub FormatTime {
  my $Time = shift;
  if ($Time =~ m/([0-9]+)m([0-9.]+)/) {
    $Time = sprintf("%7.4f", $1*60.0+$2);
  }
  return $Time;
}

sub GetRegexNum {
  my ($Regex, $Num, $Regex2, $File) = @_;
  my @Items = split "\n", `grep '$Regex' $File`;
  return GetRegex $Regex2, $Items[$Num];
}

sub GetQMTestResults { # (filename)
  my ($filename) = @_;
  my @lines;
  my $firstline;
  $/ = "\n"; #Make sure we're going line at a time.
  if (open SRCHFILE, $filename) {
    # Skip stuff before ---TEST RESULTS
    while ( <SRCHFILE> ) {
      if ( m/^--- TEST RESULTS/ ) { last; }
    }
    # Process test results
    push(@lines,"<h3>TEST RESULTS</h3><ol><li>\n");
    my $first_list = 1;
    my $should_break = 1;
    my $nocopy = 0;
    while ( <SRCHFILE> ) {
      if ( length($_) > 1 ) { 
        chomp($_);
        if ( ! m/: PASS[ ]*$/ &&
             ! m/^    qmtest.target:/ && 
             ! m/^      local/ &&
             ! m/^gmake:/ ) {
          if ( m/: XFAIL/ ) {
            $nocopy = 1;
          } elsif ( m/: XPASS/ || m/: FAIL/ ) {
            $nocopy = 0;
            if ( $first_list ) {
              $first_list = 0;
              $should_break = 1;
              push(@lines,"<b>$_</b><br/>\n");
            } else {
              push(@lines,"</li><li><b>$_</b><br/>\n");
            }
          } elsif ( m/^--- STATISTICS/ ) {
            if ( $first_list ) { push(@lines,"<b>PERFECT!</b>"); }
            push(@lines,"</li></ol><h3>STATISTICS</h3><pre>\n");
            $should_break = 0;
            $nocopy = 0;
          } elsif ( m/^--- TESTS WITH/ ) {
            $should_break = 1;
            $first_list = 1;
            $nocopy = 0;
            push(@lines,"</pre><h3>TESTS WITH UNEXPECTED RESULTS</h3><ol><li>\n");
          } elsif ( m/^real / ) {
            last;
          } elsif (!$nocopy) {
            if ( $should_break ) {
              push(@lines,"$_<br/>\n");
            } else {
              push(@lines,"$_\n");
            }
          }
        }
      }
    }
    close SRCHFILE;
  }
  my $content = join("",@lines);
  return "$content</li></ol>\n";
} 

sub GetDejagnuTestResults { # (filename, log)
  my ($filename, $DejagnuLog) = @_;
  my @lines;
  my $firstline;
  $/ = "\n"; #Make sure we're going line at a time.
  if (open SRCHFILE, $filename) {
    # Process test results
    push(@lines,"<h3>UNEXPECTED TEST RESULTS</h3><ol><li>\n");
    my $first_list = 1;
    my $should_break = 1;
    my $nocopy = 0;
    my $readingsum = 0;
    while ( <SRCHFILE> ) {
      if ( length($_) > 1 ) { 
        chomp($_);
        if ( m/^XPASS:/ || m/^FAIL:/ ) {
          $nocopy = 0;
          if ( $first_list ) {
            $first_list = 0;
            $should_break = 1;
            push(@lines,"<b>$_</b><br/>\n");
          } else {
            push(@lines,"</li><li><b>$_</b><br/>\n");
          }
        } elsif ( m/Summary/ ) {
          if ( $first_list ) { push(@lines,"<b>PERFECT!</b>"); }
          push(@lines,"</li></ol><h3>STATISTICS</h3><pre>\n");
          $should_break = 0;
          $nocopy = 0;
          $readingsum = 1;
        } elsif ( $readingsum ) {
          push(@lines,"$_\n");
        }
      }
    }
  }
  push(@lines, "</pre>\n");
  close SRCHFILE;

  #add link to complete testing log
  push(@lines, "<p>A complete log of testing <a href=\"$DejagnuLog\">Feature and Regression</a> is available for further analysis.</p>\n");

  my $content = join("",@lines);
  return "$content</li></ol>\n";
}


#####################################################################
## MAIN PROGRAM
#####################################################################

my $Template = "";
my $PlotScriptFilename = "";

# Parse arguments... 
while (scalar(@ARGV) and ($_ = $ARGV[0], /^[-+]/)) {
  shift;
  last if /^--$/;  # Stop processing arguments on --

  # List command line options here...
  if (/^-nocheckout$/)     { $NOCHECKOUT = 1; next; }
  if (/^-noremove$/)       { $NOREMOVE = 1; next; }
  if (/^-nofeaturetests$/) { $NOFEATURES = 1; next; }
  if (/^-noregressiontests$/){ $NOREGRESSIONS = 1; next; }
  if (/^-notest$/)         { $NOTEST = 1; $NORUNNINGTESTS = 1; next; }
  if (/^-norunningtests$/) { $NORUNNINGTESTS = 1; next; }
  if (/^-parallel$/)       { $MAKEOPTS = "$MAKEOPTS -j2 -l3.0"; next; }
  if (/^-release$/)        { $MAKEOPTS = "$MAKEOPTS ENABLE_OPTIMIZED=1"; next; }
  if (/^-pedantic$/)       { 
      $MAKEOPTS   = "$MAKEOPTS CompileOptimizeOpts='-O3 -DNDEBUG -finline-functions -Wpointer-arith -Wcast-align -Wno-deprecated -Wold-style-cast -Wabi -Woverloaded-virtual -ffor-scope'"; 
      next; 
  }
  if (/^-enable-linscan$/) { $PROGTESTOPTS .= " ENABLE_LINEARSCAN=1"; next; }
  if (/^-disable-llc$/)    { $PROGTESTOPTS .= " DISABLE_LLC=1";
                             $CONFIGUREARGS .= " --disable-llc_diffs"; next; }
  if (/^-disable-jit$/)    { $PROGTESTOPTS .= " DISABLE_JIT=1";
                             $CONFIGUREARGS .= " --disable-jit"; next; }
  if (/^-verbose$/)        { $VERBOSE = 1; next; }
  if (/^-debug$/)          { $DEBUG = 1; next; }
  if (/^-nice$/)           { $NICE = "nice "; next; }
  if (/^-f2c$/)            {
    $CONFIGUREARGS .= " --with-f2c=$ARGV[0]"; shift; next;
  }
  if (/^-gnuplotscript$/)  { $PlotScriptFilename = $ARGV[0]; shift; next; }
  if (/^-templatefile$/)   { $Template = $ARGV[0]; shift; next; }
  if (/^-gccpath/)         { 
    $CONFIGUREARGS .= " CC=$ARGV[0]/gcc CXX=$ARGV[0]/g++"; shift; next; 
  }
  if (/^-noexternals$/)    { $NOEXTERNALS = 1; next; }
  if(/^-rundejagnu$/) { $RUNDEJAGNU = 1; next; }

  print "Unknown option: $_ : ignoring!\n";
}

if ($ENV{'LLVMGCCDIR'}) {
  $CONFIGUREARGS .= " --with-llvmgccdir=" . $ENV{'LLVMGCCDIR'};
}
if ($CONFIGUREARGS !~ /--disable-jit/) {
  $CONFIGUREARGS .= " --enable-jit";
}

die "Must specify 0 or 3 options!" if (@ARGV != 0 and @ARGV != 3);

if (@ARGV == 3) {
  $CVSRootDir = $ARGV[0];
  $BuildDir   = $ARGV[1];
  $WebDir     = $ARGV[2];
}

my $Prefix = "$WebDir/$DATE";

#define the file names we'll use
my $BuildLog = "$Prefix-Build-Log.txt";
my $CVSLog = "$Prefix-CVS-Log.txt";
my $FeatureTestsLog = "$Prefix-FeatureTests-Log.txt";
my $RegressionTestsLog = "$Prefix-RegressionTests-Log.txt";
my $OldenTestsLog = "$Prefix-Olden-tests.txt";
my $SingleSourceLog = "$Prefix-SingleSource-ProgramTest.txt.gz";
my $MultiSourceLog = "$Prefix-MultiSource-ProgramTest.txt.gz";
my $ExternalLog = "$Prefix-External-ProgramTest.txt.gz";
my $DejagnuLog = "$Prefix-Dejagnu-testrun.log";
my $DejagnuSum = "$Prefix-Dejagnu-testrun.sum";
my $DejagnuTestsLog = "$Prefix-DejagnuTests-Log.txt";

if ($VERBOSE) {
  print "INITIALIZED\n";
  print "CVS Root = $CVSRootDir\n";
  print "BuildDir = $BuildDir\n";
  print "WebDir   = $WebDir\n";
  print "Prefix   = $Prefix\n";
  print "CVSLog   = $CVSLog\n";
  print "BuildLog = $BuildLog\n";
}

if (! -d $WebDir) {
  mkdir $WebDir, 0777;
  warn "Warning: $WebDir did not exist; creating it.\n";
}

#
# Create the CVS repository directory
#
if (!$NOCHECKOUT) {
  if (-d $BuildDir) {
    if (!$NOREMOVE) {
      system "rm -rf $BuildDir"; 
    } else {
       die "CVS checkout directory $BuildDir already exists!";
    }
  }
  mkdir $BuildDir or die "Could not create CVS checkout directory $BuildDir!";
}

ChangeDir( $BuildDir, "CVS checkout directory" );


#
# Check out the llvm tree, saving CVS messages to the cvs log...
#
$CVSOPT = "";
$CVSOPT = "-z3" if $CVSRootDir =~ /^:ext:/; # Use compression if going over ssh.
if (!$NOCHECKOUT) {
  if ( $VERBOSE ) { print "CHECKOUT STAGE\n"; }
  system "( time -p $NICE cvs $CVSOPT -d $CVSRootDir co -APR llvm; cd llvm/projects ; " .
     "$NICE cvs $CVSOPT -d $CVSRootDir co -APR llvm-test ) > $CVSLog 2>&1";
  ChangeDir( $BuildDir , "CVS Checkout directory") ;
}

ChangeDir( "llvm" , "llvm source directory") ;

if (!$NOCHECKOUT) {
  if ( $VERBOSE ) { print "UPDATE STAGE\n"; }
  system "$NICE cvs update -PdRA >> $CVSLog 2>&1" ;
}

if ( $Template eq "" ) {
  $Template = "$BuildDir/llvm/utils/NightlyTestTemplate.html";
}
die "Template file $Template is not readable" if ( ! -r "$Template" );

if ( $PlotScriptFilename eq "" ) {
  $PlotScriptFilename = "$BuildDir/llvm/utils/NightlyTest.gnuplot";
}
die "GNUPlot Script $PlotScriptFilename is not readable" if ( ! -r "$PlotScriptFilename" );

# Read in the HTML template file...
if ( $VERBOSE ) { print "READING TEMPLATE\n"; }
my $TemplateContents = ReadFile $Template;

#
# Get some static statistics about the current state of CVS
#
my $CVSCheckoutTime = GetRegex "([0-9.]+)", `grep '^real' $CVSLog`;
my $NumFilesInCVS = `egrep '^U' $CVSLog | wc -l` + 0;
my $NumDirsInCVS  = `egrep '^cvs (checkout|server|update):' $CVSLog | wc -l` + 0;
$LOC = `utils/countloc.sh`;

#
# Build the entire tree, saving build messages to the build log
#
if (!$NOCHECKOUT) {
  if ( $VERBOSE ) { print "CONFIGURE STAGE\n"; }
  system "(time -p $NICE ./configure $CONFIGUREARGS --enable-spec --with-objroot=.) > $BuildLog 2>&1";

  if ( $VERBOSE ) { print "BUILD STAGE\n"; }
  # Build the entire tree, capturing the output into $BuildLog
  system "(time -p $NICE gmake $MAKEOPTS) >> $BuildLog 2>&1";
}


#
# Get some statistics about the build...
#
my @Linked = split '\n', `grep Linking $BuildLog`;
my $NumExecutables = scalar(grep(/executable/, @Linked));
my $NumLibraries   = scalar(grep(!/executable/, @Linked));
my $NumObjects     = `grep '^Compiling' $BuildLog | wc -l` + 0;

my $ConfigTimeU = GetRegexNum "^user", 0, "([0-9.]+)", "$BuildLog";
my $ConfigTimeS = GetRegexNum "^sys", 0, "([0-9.]+)", "$BuildLog";
my $ConfigTime  = $ConfigTimeU+$ConfigTimeS;  # ConfigTime = User+System
my $ConfigWallTime = GetRegexNum "^real", 0,"([0-9.]+)","$BuildLog";

my $BuildTimeU = GetRegexNum "^user", 1, "([0-9.]+)", "$BuildLog";
my $BuildTimeS = GetRegexNum "^sys", 1, "([0-9.]+)", "$BuildLog";
my $BuildTime  = $BuildTimeU+$BuildTimeS;  # BuildTime = User+System
my $BuildWallTime = GetRegexNum "^real", 1, "([0-9.]+)","$BuildLog";

my $BuildError = "";
if (`grep '^gmake[^:]*: .*Error' $BuildLog | wc -l` + 0 ||
    `grep '^gmake: \*\*\*.*Stop.' $BuildLog | wc -l`+0) {
  $BuildError = "<h3><font color='red'>Build error: compilation " .
                "<a href=\"$DATE-Build-Log.txt\">aborted</a></font></h3>";
  if ($VERBOSE) { print "BUILD ERROR\n"; }
}

if ($BuildError) { $NOFEATURES = 1; $NOREGRESSIONS = 1; $RUNDEJAGNU=0; }

# Get results of feature tests.
my $FeatureTestResults; # String containing the results of the feature tests
my $FeatureTime;        # System+CPU Time for feature tests
my $FeatureWallTime;    # Wall Clock Time for feature tests
if (!$NOFEATURES) {
  if ( $VERBOSE ) { print "FEATURE TEST STAGE\n"; }
  my $feature_output = "$FeatureTestsLog";

  # Run the feature tests so we can summarize the results
  system "(time -p gmake $MAKEOPTS -C test Feature.t) > $feature_output 2>&1";

  # Extract test results
  $FeatureTestResults = GetQMTestResults("$feature_output");

  # Extract time of feature tests
  my $FeatureTimeU = GetRegexNum "^user", 0, "([0-9.]+)", "$feature_output";
  my $FeatureTimeS = GetRegexNum "^sys", 0, "([0-9.]+)", "$feature_output";
  $FeatureTime  = $FeatureTimeU+$FeatureTimeS;  # FeatureTime = User+System
  $FeatureWallTime = GetRegexNum "^real", 0,"([0-9.]+)","$feature_output";
  # Run the regression tests so we can summarize the results
} else {
  $FeatureTestResults = "Skipped by user choice.";
  $FeatureTime     = "0.0";
  $FeatureWallTime = "0.0";
}

if (!$NOREGRESSIONS) {
  if ( $VERBOSE ) { print "REGRESSION TEST STAGE\n"; }
  my $regression_output = "$RegressionTestsLog";

  # Run the regression tests so we can summarize the results
  system "(time -p gmake $MAKEOPTS -C test Regression.t) > $regression_output 2>&1";

  # Extract test results
  $RegressionTestResults = GetQMTestResults("$regression_output");

  # Extract time of regressions tests
  my $RegressionTimeU = GetRegexNum "^user", 0, "([0-9.]+)", "$regression_output";
  my $RegressionTimeS = GetRegexNum "^sys", 0, "([0-9.]+)", "$regression_output";
  $RegressionTime  = $RegressionTimeU+$RegressionTimeS;  # RegressionTime = User+System
  $RegressionWallTime = GetRegexNum "^real", 0,"([0-9.]+)","$regression_output";
} else {
  $RegressionTestResults = "Skipped by user choice.";
  $RegressionTime     = "0.0";
  $RegressionWallTime = "0.0";
}

my $DejangnuTestResults; # String containing the results of the dejagnu
if($RUNDEJAGNU) {
  if($VERBOSE) { print "DEJAGNU FEATURE/REGRESSION TEST STAGE\n"; }
  
  my $dejagnu_output = "$DejagnuTestsLog";
  
  #Run the feature and regression tests, results are put into testrun.sum
  #Full log in testrun.log
  system "time -p gmake $MAKEOPTS check-dejagnu > $dejagnu_output 2>&1";

  #Extract time of dejagnu tests
  my $DejagnuTimeU = GetRegexNum "^user", 0, "([0-9.]+)", "$dejagnu_output";
  my $DejagnuTimeS = GetRegexNum "^sys", 0, "([0-9.]+)", "$dejagnu_output";
  $DejagnuTime  = $DejagnuTimeU+$DejagnuTimeS;  # DejagnuTime = User+System
  $DejagnuWallTime = GetRegexNum "^real", 0,"([0-9.]+)","$dejagnu_output";

  #Copy the testrun.log and testrun.sum to our webdir
  CopyFile("test/testrun.log", $DejagnuLog);
  CopyFile("test/testrun.sum", $DejagnuSum);

  $DejagnuTestResults = GetDejagnuTestResults($DejagnuSum, $DejagnuLog);
} else {
  $DejagnuTestResults = "Skipped by user choice.";
  $DejagnuTime     = "0.0";
  $DejagnuWallTime = "0.0";
}

if ($DEBUG) {
  print $FeatureTestResults;
  print $RegressionTestResults;
  print $DejagnuTestResults;
}

if ( $VERBOSE ) { print "BUILD INFORMATION COLLECTION STAGE\n"; }
#
# Get warnings from the build
#
my @Warn = split "\n", `egrep 'warning:|Entering dir' $BuildLog`;
my @Warnings;
my $CurDir = "";

foreach $Warning (@Warn) {
  if ($Warning =~ m/Entering directory \`([^\`]+)\'/) {
    $CurDir = $1;                 # Keep track of directory warning is in...
    if ($CurDir =~ m#$BuildDir/llvm/(.*)#) { # Remove buildir prefix if included
      $CurDir = $1;
    }
  } else {
    push @Warnings, "$CurDir/$Warning";     # Add directory to warning...
  }
}
my $WarningsFile =  join "\n", @Warnings;
my $WarningsList = AddPreTag $WarningsFile;
$WarningsFile =~ s/:[0-9]+:/::/g;

# Emit the warnings file, so we can diff...
WriteFile "$WebDir/$DATE-Warnings.txt", $WarningsFile . "\n";
my ($WarningsAdded, $WarningsRemoved) = DiffFiles "-Warnings.txt";

# Output something to stdout if something has changed
print "ADDED   WARNINGS:\n$WarningsAdded\n\n" if (length $WarningsAdded);
print "REMOVED WARNINGS:\n$WarningsRemoved\n\n" if (length $WarningsRemoved);

$WarningsAdded = AddPreTag $WarningsAdded;
$WarningsRemoved = AddPreTag $WarningsRemoved;

#
# Get some statistics about CVS commits over the current day...
#
if ($VERBOSE) { print "CVS HISTORY ANALYSIS STAGE\n"; }
@CVSHistory = split "\n", `cvs history -D '1 day ago' -a -xAMROCGUW`;
#print join "\n", @CVSHistory; print "\n";

# Extract some information from the CVS history... use a hash so no duplicate
# stuff is stored.
my (%AddedFiles, %ModifiedFiles, %RemovedFiles, %UsersCommitted, %UsersUpdated);

my $DateRE = '[-/:0-9 ]+\+[0-9]+';

# Loop over every record from the CVS history, filling in the hashes.
foreach $File (@CVSHistory) {
  my ($Type, $Date, $UID, $Rev, $Filename);
  if ($File =~ /([AMRUGC]) ($DateRE) ([^ ]+) +([^ ]+) +([^ ]+) +([^ ]+)/) {
    ($Type, $Date, $UID, $Rev, $Filename) = ($1, $2, $3, $4, "$6/$5");
  } elsif ($File =~ /([W]) ($DateRE) ([^ ]+)/) {
    ($Type, $Date, $UID, $Rev, $Filename) = ($1, $2, $3, "", "");
  } elsif ($File =~ /([O]) ($DateRE) ([^ ]+) +([^ ]+)/) {
    ($Type, $Date, $UID, $Rev, $Filename) = ($1, $2, $3, "", "$4/");
  } else {
    print "UNMATCHABLE: $File\n";
    next;
  }
  # print "$File\nTy = $Type Date = '$Date' UID=$UID Rev=$Rev File = '$Filename'\n";

  if ($Filename =~ /^llvm/) {
    if ($Type eq 'M') {        # Modified
      $ModifiedFiles{$Filename} = 1;
      $UsersCommitted{$UID} = 1;
    } elsif ($Type eq 'A') {   # Added
      $AddedFiles{$Filename} = 1;
      $UsersCommitted{$UID} = 1;
    } elsif ($Type eq 'R') {   # Removed
      $RemovedFiles{$Filename} = 1;
      $UsersCommitted{$UID} = 1;
    } else {
      $UsersUpdated{$UID} = 1;
    }
  }
}

my $UserCommitList = join "\n", keys %UsersCommitted;
my $UserUpdateList = join "\n", keys %UsersUpdated;
my $AddedFilesList = AddPreTag join "\n", sort keys %AddedFiles;
my $ModifiedFilesList = AddPreTag join "\n", sort keys %ModifiedFiles;
my $RemovedFilesList = AddPreTag join "\n", sort keys %RemovedFiles;

my $TestError = 1;
my $SingleSourceProgramsTable = "!";
my $MultiSourceProgramsTable = "!";
my $ExternalProgramsTable = "!";


sub TestDirectory {
  my $SubDir = shift;

  ChangeDir( "projects/llvm-test/$SubDir", "Programs Test Subdirectory" );

  my $ProgramTestLog = "$Prefix-$SubDir-ProgramTest.txt";

  # Run the programs tests... creating a report.nightly.html file
  if (!$NOTEST) {
    system "gmake -k $MAKEOPTS $PROGTESTOPTS report.nightly.html "
         . "TEST=nightly > $ProgramTestLog 2>&1";
  } else {
    system "gunzip ${ProgramTestLog}.gz";
  }

  my $ProgramsTable;
  if (`grep '^gmake[^:]: .*Error' $ProgramTestLog | wc -l` + 0){
    $TestError = 1;
    $ProgramsTable = "<font color=white><h2>Error running tests!</h2></font>";
    print "ERROR TESTING\n";
  } elsif (`grep '^gmake[^:]: .*No rule to make target' $ProgramTestLog | wc -l` + 0) {
    $TestError = 1;
    $ProgramsTable =
      "<font color=white><h2>Makefile error running tests!</h2></font>";
    print "ERROR TESTING\n";
  } else {
    $TestError = 0;
    $ProgramsTable = ReadFile "report.nightly.html";

    #
    # Create a list of the tests which were run...
    #
    system "egrep 'TEST-(PASS|FAIL)' < $ProgramTestLog "
         . "| sort > $Prefix-$SubDir-Tests.txt";
  }

  # Compress the test output
  system "gzip -f $ProgramTestLog";
  ChangeDir( "../../..", "Programs Test Parent Directory" );
  return $ProgramsTable;
}

# If we built the tree successfully, run the nightly programs tests...
if ($BuildError eq "") {
  if ( $VERBOSE ) {
    print "SingleSource TEST STAGE\n";
  }
  $SingleSourceProgramsTable = TestDirectory("SingleSource");
  if ( $VERBOSE ) {
    print "MultiSource TEST STAGE\n";
  }
  $MultiSourceProgramsTable = TestDirectory("MultiSource");
  if ( ! $NOEXTERNALS ) {
    if ( $VERBOSE ) {
      print "External TEST STAGE\n";
    }
    $ExternalProgramsTable = TestDirectory("External");
    system "cat $Prefix-SingleSource-Tests.txt $Prefix-MultiSource-Tests.txt ".
         " $Prefix-External-Tests.txt | sort > $Prefix-Tests.txt";
  } else {
    if ( $VERBOSE ) {
      print "External TEST STAGE SKIPPED\n";
    }
    system "cat $Prefix-SingleSource-Tests.txt $Prefix-MultiSource-Tests.txt ".
         " | sort > $Prefix-Tests.txt";
  }
}

if ( $VERBOSE ) { print "TEST INFORMATION COLLECTION STAGE\n"; }
my ($TestsAdded, $TestsRemoved, $TestsFixed, $TestsBroken) = ("","","","");

if ($TestError) {
  $TestsAdded   = "<b>error testing</b><br>";
  $TestsRemoved = "<b>error testing</b><br>";
  $TestsFixed   = "<b>error testing</b><br>";
  $TestsBroken  = "<b>error testing</b><br>";
} else {
  my ($RTestsAdded, $RTestsRemoved) = DiffFiles "-Tests.txt";

  my @RawTestsAddedArray = split '\n', $RTestsAdded;
  my @RawTestsRemovedArray = split '\n', $RTestsRemoved;

  my %OldTests = map {GetRegex('TEST-....: (.+)', $_)=>$_}
    @RawTestsRemovedArray;
  my %NewTests = map {GetRegex('TEST-....: (.+)', $_)=>$_}
    @RawTestsAddedArray;

  foreach $Test (keys %NewTests) {
    if (!exists $OldTests{$Test}) {  # TestAdded if in New but not old
      $TestsAdded = "$TestsAdded$Test\n";
    } else {
      if ($OldTests{$Test} =~ /TEST-PASS/) {  # Was the old one a pass?
        $TestsBroken = "$TestsBroken$Test\n";  # New one must be a failure
      } else {
        $TestsFixed = "$TestsFixed$Test\n";    # No, new one is a pass.
      }
    }
  }
  foreach $Test (keys %OldTests) {  # TestRemoved if in Old but not New
    $TestsRemoved = "$TestsRemoved$Test\n" if (!exists $NewTests{$Test});
  }

  print "\nTESTS ADDED:  \n\n$TestsAdded\n\n"   if (length $TestsAdded);
  print "\nTESTS REMOVED:\n\n$TestsRemoved\n\n" if (length $TestsRemoved);
  print "\nTESTS FIXED:  \n\n$TestsFixed\n\n"   if (length $TestsFixed);
  print "\nTESTS BROKEN: \n\n$TestsBroken\n\n"  if (length $TestsBroken);

  $TestsAdded   = AddPreTag $TestsAdded;
  $TestsRemoved = AddPreTag $TestsRemoved;
  $TestsFixed   = AddPreTag $TestsFixed;
  $TestsBroken  = AddPreTag $TestsBroken;
}


# If we built the tree successfully, runs of the Olden suite with
# LARGE_PROBLEM_SIZE on so that we can get some "running" statistics.
if ($BuildError eq "") {
  if ( $VERBOSE ) { print "OLDEN TEST SUITE STAGE\n"; }
  my ($NATTime, $CBETime, $LLCTime, $JITTime, $OptTime, $BytecodeSize,
      $MachCodeSize) = ("","","","","","","");
  if (!$NORUNNINGTESTS) {
    ChangeDir( "$BuildDir/llvm/projects/llvm-test/MultiSource/Benchmarks/Olden",
      "Olden Test Directory");

    # Clean out previous results...
    system "$NICE gmake $MAKEOPTS clean > /dev/null 2>&1";

    # Run the nightly test in this directory, with LARGE_PROBLEM_SIZE and
    # GET_STABLE_NUMBERS enabled!
    system "gmake -k $MAKEOPTS $PROGTESTOPTS report.nightly.raw.out TEST=nightly " .
           " LARGE_PROBLEM_SIZE=1 GET_STABLE_NUMBERS=1 > /dev/null 2>&1";
    system "cp report.nightly.raw.out $OldenTestsLog";
  } else {
    system "gunzip ${OldenTestsLog}.gz";
  }

  # Now we know we have $OldenTestsLog as the raw output file.  Split
  # it up into records and read the useful information.
  my @Records = split />>> ========= /, ReadFile "$OldenTestsLog";
  shift @Records;  # Delete the first (garbage) record

  # Loop over all of the records, summarizing them into rows for the running
  # totals file.
  my $WallTimeRE = "[A-Za-z0-9.: ]+\\(([0-9.]+) wall clock";
  foreach $Rec (@Records) {
    my $rNATTime = GetRegex 'TEST-RESULT-nat-time: program\s*([.0-9m]+)', $Rec;
    my $rCBETime = GetRegex 'TEST-RESULT-cbe-time: program\s*([.0-9m]+)', $Rec;
    my $rLLCTime = GetRegex 'TEST-RESULT-llc-time: program\s*([.0-9m]+)', $Rec;
    my $rJITTime = GetRegex 'TEST-RESULT-jit-time: program\s*([.0-9m]+)', $Rec;
    my $rOptTime = GetRegex "TEST-RESULT-compile: $WallTimeRE", $Rec;
    my $rBytecodeSize = GetRegex 'TEST-RESULT-compile: *([0-9]+)', $Rec;
    my $rMachCodeSize = GetRegex 'TEST-RESULT-jit-machcode: *([0-9]+).*bytes of machine code', $Rec;

    $NATTime .= " " . FormatTime($rNATTime);
    $CBETime .= " " . FormatTime($rCBETime);
    $LLCTime .= " " . FormatTime($rLLCTime);
    $JITTime .= " " . FormatTime($rJITTime);
    $OptTime .= " $rOptTime";
    $BytecodeSize .= " $rBytecodeSize";
    $MachCodeSize .= " $rMachCodeSize";
  }

  # Now that we have all of the numbers we want, add them to the running totals
  # files.
  AddRecord($NATTime, "running_Olden_nat_time.txt");
  AddRecord($CBETime, "running_Olden_cbe_time.txt");
  AddRecord($LLCTime, "running_Olden_llc_time.txt");
  AddRecord($JITTime, "running_Olden_jit_time.txt");
  AddRecord($OptTime, "running_Olden_opt_time.txt");
  AddRecord($BytecodeSize, "running_Olden_bytecode.txt");
  AddRecord($MachCodeSize, "running_Olden_machcode.txt");

  system "gzip -f $OldenTestsLog";
}


#
# Get a list of the previous days that we can link to...
#
my @PrevDays = map {s/.html//; $_} GetDir ".html";

if ((scalar @PrevDays) > 20) {
  splice @PrevDays, 20;  # Trim down list to something reasonable...
}

# Format list for sidebar
my $PrevDaysList = join "\n  ", map { "<a href=\"$_.html\">$_</a><br>" } @PrevDays;

#
# Start outputting files into the web directory
#
ChangeDir( $WebDir, "Web Directory" );

# Make sure we don't get errors running the nightly tester the first time
# because of files that don't exist.
Touch ('running_build_time.txt', 'running_Olden_llc_time.txt',
       'running_loc.txt', 'running_Olden_machcode.txt',
       'running_Olden_bytecode.txt', 'running_Olden_nat_time.txt',
       'running_Olden_cbe_time.txt', 'running_Olden_opt_time.txt',
       'running_Olden_jit_time.txt');

# Add information to the files which accumulate information for graphs...
AddRecord($LOC, "running_loc.txt");
AddRecord($BuildTime, "running_build_time.txt");

if ( $VERBOSE ) {
  print "GRAPH GENERATION STAGE\n";
}
#
# Rebuild the graphs now...
#
$GNUPLOT = "/usr/bin/gnuplot";
$GNUPLOT = "gnuplot" if ! -x $GNUPLOT;
system ("$GNUPLOT", $PlotScriptFilename);

#
# Remove the cvs tree...
#
system ( "$NICE rm -rf $BuildDir") if (!$NOCHECKOUT and !$NOREMOVE);

#
# Print out information...
#
if ($VERBOSE) {
  print "DateString: $DateString\n";
  print "CVS Checkout: $CVSCheckoutTime seconds\n";
  print "Files/Dirs/LOC in CVS: $NumFilesInCVS/$NumDirsInCVS/$LOC\n";
  print "Build Time: $BuildTime seconds\n";
  print "Feature Test Time: $FeatureTime seconds\n";
  print "Regression Test Time: $RegressionTime seconds\n";
  print "Libraries/Executables/Objects built: $NumLibraries/$NumExecutables/$NumObjects\n";

  print "WARNINGS:\n  $WarningsList\n";

  print "Users committed: $UserCommitList\n";
  print "Added Files: \n  $AddedFilesList\n";
  print "Modified Files: \n  $ModifiedFilesList\n";
  print "Removed Files: \n  $RemovedFilesList\n";

  print "Previous Days =\n  $PrevDaysList\n";
}


#
# Output the files...
#

if ( $VERBOSE ) {
  print "OUTPUT STAGE\n";
}
# Main HTML file...
my $Output;
my $TestFinishTime = gmtime;
my $TestPlatform = `uname -a`;
eval "\$Output = <<ENDOFFILE;$TemplateContents\nENDOFFILE\n";
WriteFile "$DATE.html", $Output;
system ( "ln -sf $DATE.html index.html" );

# Change the index.html symlink...

# vim: sw=2 ai
