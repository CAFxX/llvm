
SparcV8 backend skeleton
------------------------

This directory houses a 32-bit SPARC V8 backend employing a expander-based
instruction selector.  It is not yet functionally complete.  Watch
this space for more news coming soon!

Current shootout results as of 28-Sept-2004
-------------------------------------------

Working: ackermann fib2 hash hello lists matrix methcall nestedloop
         objinst sieve strcat random ary3 
Broken: heapsort

To-do
-----

* support 64-bit (double FP, long, ulong) arguments to functions
* support functions with more than 6 args
* support setcc on longs
* support basic binary operations on longs
* support casting <=32-bit integers, bools to long
* support casting 64-bit integers to FP types

$Date$

