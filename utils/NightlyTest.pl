#!/usr/dcs/software/supported/bin/perl -w
#
# Program:  NightlyTest.pl
#
# Synopsis: Perform a series of tests which are designed to be run nightly.
#           This is used to keep track of the status of the LLVM tree, tracking
#           regressions and performance changes.  This generates one web page a
#           day which can be used to access this information.
#
# Syntax:   NightlyTest.pl <CVSRootDir> <BuildDir> <WebDir>
#
use POSIX qw(strftime);

my $HOME = $ENV{HOME};
my $CVSRootDir = "/home/vadve/vadve/Research/DynOpt/CVSRepository";
my $BuildDir   = "$HOME/buildtest";
my $WebDir     = "$HOME/cvs/testresults-X86";

# Calculate the date prefix...
@TIME = localtime;
my $DATE = sprintf "%4d-%02d-%02d", $TIME[5]+1900, $TIME[4]+1, $TIME[3];
my $DateString = strftime "%B %d, %Y", localtime;

sub ReadFile {
  if (open (FILE, $_[0])) {
    my $Ret = <FILE>;
    close FILE;
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
  return $1;
}

sub AddPreTag {  # Add pre tags around nonempty list, or convert to "none"
  $_ = shift;
  if (length) { return "<ul><pre>$_</pre></ul>"; } else { "<b>none</b><br>"; }
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


# Command line argument settings...
my $NOCHECKOUT = 0;
my $NOREMOVE   = 0;
my $NOTEST     = 0;
my $MAKEOPTS   = "";

# Parse arguments...
while (scalar(@ARGV) and ($_ = $ARGV[0], /^[-+]/)) {
  shift;
  last if /^--$/;  # Stop processing arguments on --

  # List command line options here...
  if (/^-nocheckout$/) { $NOCHECKOUT = 1; next; }
  if (/^-noremove$/)   { $NOREMOVE   = 1; next; }
  if (/^-notest$/)     { $NOTEST     = 1; next; }
  if (/^-parallel$/)   { $MAKEOPTS   = "-j2 -l3.0"; next; }

  print "Unknown option: $_ : ignoring!\n";
}

die "Must specify 0 or 3 options!" if (@ARGV != 0 and @ARGV != 3);

# FIXME: This should just be utils/...
my $Template   = "$HOME/llvm/utils/NightlyTestTemplate.html";

if (@ARGV == 3) {
  $CVSRootDir = $ARGV[0];
  $BuildDir   = $ARGV[1];
  $WebDir     = $ARGV[2];
}

my $Prefix = "$WebDir/$DATE";

if (0) {
  print "CVS Root = $CVSRootDir\n";
  print "BuildDir = $BuildDir\n";
  print "WebDir   = $WebDir\n";
  print "Prefix   = $Prefix\n";
}


#
# Create the CVS repository directory
#
if (!$NOCHECKOUT) {
  mkdir $BuildDir or die "Could not create CVS checkout directory!";
}
chdir $BuildDir or die "Could not change to CVS checkout directory!";


#
# Check out the llvm tree, saving CVS messages to the cvs log...
#
system "(time -p cvs -d $CVSRootDir co llvm) > $Prefix-CVS-Log.txt 2>&1"
  if (!$NOCHECKOUT);

chdir "llvm" or die "Could not change into llvm directory!";

system "cvs up -P -d > /dev/null 2>&1" if (!$NOCHECKOUT);

# Read in the HTML template file...
undef $/;
my $TemplateContents = ReadFile $Template;


#
# Get some static statistics about the current state of CVS
#
my $CVSCheckoutTime = GetRegex "([0-9.]+)", `grep '^real' $Prefix-CVS-Log.txt`;
my $NumFilesInCVS = `grep ^U $Prefix-CVS-Log.txt | wc -l` + 0;
my $NumDirsInCVS  = `grep '^cvs checkout' $Prefix-CVS-Log.txt | wc -l` + 0;
$LOC = GetRegex "([0-9]+) +total", `wc -l \`utils/getsrcs.sh\` | grep total`;

#
# Build the entire tree, saving build messages to the build log
#
if (!$NOCHECKOUT) {
  # Change the Makefile.config to build into the local directory...
  rename "Makefile.config", "Makefile.config.orig";
  system "sed '/^LLVM_OBJ_DIR/d' < Makefile.config.orig > Makefile.config";
  system "echo >> Makefile.config";
  system "echo 'LLVM_OBJ_DIR := .' >> Makefile.config";

  # Change the Makefile.config to not strip executables...
  system "echo 'KEEP_SYMBOLS := 1' >> Makefile.config";

  # Build the entire tree, capturing the output into $Prefix-Build-Log.txt
  system "(time -p gmake $MAKEOPTS) > $Prefix-Build-Log.txt 2>&1";
}


#
# Get some statistics about the build...
#
my @Linked = split '\n', `grep Linking $Prefix-Build-Log.txt`;
my $NumExecutables = scalar(grep(/executable/, @Linked));
my $NumLibraries   = scalar(grep(!/executable/, @Linked));
my $NumObjects     = `grep '^Compiling' $Prefix-Build-Log.txt | wc -l` + 0;
my $BuildTimeU = GetRegex "([0-9.]+)", `grep '^user' $Prefix-Build-Log.txt`;
my $BuildTimeS = GetRegex "([0-9.]+)", `grep '^sys' $Prefix-Build-Log.txt`;
my $BuildWallTime = GetRegex "([0-9.]+)", `grep '^real' $Prefix-Build-Log.txt`;
my $BuildTime  = $BuildTimeU+$BuildTimeS;  # BuildTime = User+System
my $BuildError = "";
if (`grep '^gmake: .*Error' $Prefix-Build-Log.txt | wc -l` + 0) {
  $BuildError = "<h3>Build error: compilation <a href=\"$DATE-Build-Log.txt\">"
              . "aborted</a></h3>";
}


#
# Get warnings from the build
#
my @Warn = split "\n", `grep -E 'warning:|Entering dir' $Prefix-Build-Log.txt`;
my @Warnings;
my $CurDir = "";

foreach $Warning (@Warn) {
  if ($Warning =~ m/Entering directory \`([^\`]+)\'/) {
    $CurDir = $1;                 # Keep track of directory warning is in...
    if ($CurDir =~ m|$BuildDir/llvm/(.*)|) { # Remove buildir prefix if included
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
$WarningsAdded = AddPreTag $WarningsAdded;
$WarningsRemoved = AddPreTag $WarningsRemoved;


#
# Get some statistics about CVS commits over the current day...
#
@CVSHistory = split "\n", `cvs history -D '1 day ago' -a -xAMROCGUW`;
#print join "\n", @CVSHistory; print "\n";

# Extract some information from the CVS history... use a hash so no duplicate
# stuff is stored.
my (%AddedFiles, %ModifiedFiles, %RemovedFiles,
    %UsersCommitted, %UsersUpdated);

my $DateRE = "[-:0-9 ]+\\+[0-9]+";

# Loop over every record from the CVS history, filling in the hashes.
foreach $File (@CVSHistory) {
  my ($Type, $Date, $UID, $Rev, $Filename);
  if ($File =~ /([AMRUGC])\s($DateRE)\s([^\s]+)\s+([0-9.]+)\s+([^\s]+)\s+([^\s]+)/) {
    ($Type, $Date, $UID, $Rev, $Filename) = ($1, $2, $3, $4, "$6/$5");
  } elsif ($File =~ /([W])\s($DateRE)\s([^\s]+) +([^\s]+)\s+([^\s]+)/) {
    ($Type, $Date, $UID, $Rev, $Filename) = ($1, $2, $3, $4, "$6/$5");
  } elsif ($File =~ /([O]) ($DateRE) ([^ ]+) +([^\s]+)/) {
    ($Type, $Date, $UID, $Rev, $Filename) = ($1, $2, $3, "", "$4/");
  } else {
    print "UNMATCHABLE: $File\n";
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
my $AddedFilesList = AddPreTag join "\n", keys %AddedFiles;
my $ModifiedFilesList = AddPreTag join "\n", keys %ModifiedFiles;
my $RemovedFilesList = AddPreTag join "\n", keys %RemovedFiles;

#
# Run the nightly programs tests...
#
chdir "test/Programs" or die "Could not change into programs testdir!";

# Run the programs tests... creating a report.nightly.html file
system "gmake $MAKEOPTS report.nightly.html TEST=nightly "
     . "> $Prefix-ProgramTest.txt 2>&1" if (!$NOTEST);

my $ProgramsTable = ReadFile "report.nightly.html";

#
# Create a list of the tests which were run...
#
system "grep -E 'TEST-(PASS|FAIL)' < $Prefix-ProgramTest.txt "
     . "| sort --key=3 > $Prefix-Tests.txt";

my ($RTestsAdded, $RTestsRemoved) = DiffFiles "-Tests.txt";

my @RawTestsAddedArray = split '\n', $RTestsAdded;
my @RawTestsRemovedArray = split '\n', $RTestsRemoved;

my %OldTests = map {GetRegex('TEST-....: (.+)', $_)=>$_} @RawTestsRemovedArray;
my %NewTests = map {GetRegex('TEST-....: (.+)', $_)=>$_} @RawTestsAddedArray;

my ($TestsAdded, $TestsRemoved, $TestsFixed, $TestsBroken) = ("","","","");

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

$TestsAdded   = AddPreTag $TestsAdded;
$TestsRemoved = AddPreTag $TestsRemoved;
$TestsFixed   = AddPreTag $TestsFixed;
$TestsBroken  = AddPreTag $TestsBroken;


#
# Get a list of the previous days that we can link to...
#
my @PrevDays = map {s/.html//; $_} GetDir ".html";

splice @PrevDays, 20;  # Trim down list to something reasonable...

my $PrevDaysList =     # Format list for sidebar
  join "\n  ", map { "<a href=\"$_.html\">$_</a><br>" } @PrevDays;


#
# Remove the cvs tree...
#
chdir $WebDir or die "Could not change into web directory!";
system "rm -rf $BuildDir" if (!$NOCHECKOUT and !$NOREMOVE);


#
# Print out information...
#
if (0) {
  print "DateString: $DateString\n";
  print "CVS Checkout: $CVSCheckoutTime seconds\n";
  print "Files/Dirs/LOC in CVS: $NumFilesInCVS/$NumDirsInCVS/$LOC\n";

  print "Build Time: $BuildTime seconds\n";
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

# Main HTML file...
my $Output;
eval "\$Output = <<ENDOFFILE;$TemplateContents\nENDOFFILE\n";
WriteFile "$DATE.html", $Output;

# Change the index.html symlink...
system "ln -sf $DATE.html index.html";

sub AddRecord {
  my ($Val, $Filename) = @_;
  my @Records;
  if (open FILE, $Filename) {
    @Records = grep !/$DATE/, split "\n", <FILE>;
    close FILE;
  }
  push @Records, "$DATE: $Val";
  WriteFile $Filename, (join "\n", @Records) . "\n";
  return @Records;
}

# Add information to the files which accumulate information for graphs...
AddRecord($LOC, "running_loc.txt");
AddRecord($BuildTime, "running_build_time.txt");
