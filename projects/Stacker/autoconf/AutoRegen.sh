#!/bin/sh
die () {
	echo "$@" 1>&2
	exit 1
}
test -d autoconf && test -f autoconf/configure.ac && cd autoconf
[ -f configure.ac ] || die "Can't find 'autoconf' dir; please cd into it first"
echo "Regenerating aclocal.m4 with aclocal"
aclocal || die "aclocal failed"
autoconf --version | egrep '2\.5[0-9]' > /dev/null
if test $? -ne 0
then
	die "Your autoconf was not detected as being 2.5x"
fi
echo "Regenerating configure with autoconf 2.5x"
autoconf -o ../configure configure.ac || die "autoconf failed"
cd ..
exit 0
