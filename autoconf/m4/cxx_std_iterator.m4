# Check for standard iterator extension.  This is modified from
# http://www.gnu.org/software/ac-archive/htmldoc/ac_cxx_have_ext_hash_set.html
AC_DEFUN([AC_CXX_HAVE_STD_ITERATOR],
[AC_CACHE_CHECK(whether the compiler has the standard iterator,
ac_cv_cxx_have_std_iterator,
[AC_REQUIRE([AC_CXX_NAMESPACES])
  AC_LANG_SAVE
  AC_LANG_CPLUSPLUS
  AC_TRY_COMPILE([#include <iterator>
#ifdef HAVE_NAMESPACES
using namespace std;
#endif],[iterator<int,int,int> t; return 0;],
  ac_cv_cxx_have_std_iterator=yes, ac_cv_cxx_have_std_iterator=no)
  AC_LANG_RESTORE
])
HAVE_STD_ITERATOR=0
if test "$ac_cv_cxx_have_std_iterator" = yes
then
   HAVE_STD_ITERATOR=1
fi
AC_SUBST(HAVE_STD_ITERATOR)])


