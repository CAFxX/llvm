; Test that: extern int X[]  and int X[] = { 1, 2, 3, 4 } are resolved 
; correctly.  This doesn't have constantexprs
;
; RUN: if as < %s | opt -funcresolve | dis | grep external
; RUN: then exit 1
; RUN: else exit 0
; RUN: fi
;

%X = external global int
%X = global [4 x int] [ int 1, int 2, int 3, int 4 ]

implementation   ; Functions:

int %foo(int %x) {
bb1:                                    ;[#uses=0]
	%G = getelementptr int* %X, long 1
	store int 5, int* %G
	%F = getelementptr int* %X, long 2
	%val = load int* %F
        ret int %val
}

