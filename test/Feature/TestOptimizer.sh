#!/bin/sh
LD_LIBRARY_PATH=../../lib/Debug
export LD_LIBRARY_PATH

AS=../../tools/Debug/as
DIS=../../tools/Debug/dis
OPT=../../tools/Debug/opt

echo "======== Running optimizier test on $1"

(
  $AS < $1 | $OPT -q -inline -dce -constprop -dce |$DIS| $AS > $1.bc.1 || exit 1

  # Should not be able to optimize further!
  $OPT -q -constprop -dce < $1.bc.1 > $1.bc.2 || exit 2

  $DIS < $1.bc.1 > $1.ll.1 || exit 3
  $DIS < $1.bc.2 > $1.ll.2 || exit 3
  diff $1.ll.[12] || exit 3

  # Try out SCCP & CleanGCC
  $AS < $1 | $OPT -q -inline -dce -cleangcc -sccp -dce \
           | $DIS | $AS > $1.bc.3 || exit 1

  # Should not be able to optimize further!
  $OPT -q -sccp -dce < $1.bc.3 > $1.bc.4 || exit 2
  $DIS < $1.bc.3 > $1.ll.3 || exit 3
  $DIS < $1.bc.4 > $1.ll.4 || exit 3
  diff $1.ll.[34] || exit 3
  rm $1.bc.[1234] $1.ll.[1234]
  
  touch Output/$1.opt  # Success!
)|| ./Failure.sh "$1 Optimizer"
