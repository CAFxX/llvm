#!/bin/sh
# test that every step outputs something that is consumable by 
# another step

rm -f test.bc.temp[12]

LD_LIBRARY_PATH=../lib/Debug:/usr/dcs/software/evaluation/encap/gcc-3.0.2/lib/
export LD_LIBRARY_PATH

AS=../tools/Debug/as
DIS=../tools/Debug/dis
export AS
export DIS


# Two full cycles are needed for bitwise stability

$AS  < $1      > $1.bc.1 || exit 1
$DIS < $1.bc.1 > $1.ll.1 || exit 2
$AS  < $1.ll.1 > $1.bc.2 || exit 3
$DIS < $1.bc.2 > $1.ll.2 || exit 4

diff $1.ll.[12] || exit 7

# FIXME: When we sort things correctly and deterministically, we can reenable this
#diff $1.bc.[12] || exit 8

rm $1.[bl][cl].[12]

