; This testcase tests for various features the basicaa test should be able to 
; determine, as noted in the comments.

; RUN: if as < %s | opt -basicaa -load-vn -gcse -instcombine -dce | dis | grep REMOVE
; RUN: then exit 1
; RUN: else exit 0
; RUN: fi


; Array test:  Test that operations on one local array do not invalidate 
; operations on another array.  Important for scientific codes.
;
int %different_array_test(uint %A, uint %B) {
	%Array1 = alloca int, uint 100
	%Array2 = alloca int, uint 200

	%pointer = getelementptr int* %Array1, uint %A
	%val = load int* %pointer

	%pointer2 = getelementptr int* %Array2, uint %B
	store int 7, int* %pointer2

	%REMOVE = load int* %pointer ; redundant with above load
	%retval = sub int %REMOVE, %val
	ret int %retval
}

; Constant index test: Constant indexes into the same array should not 
; interfere with each other.  Again, important for scientific codes.
;
int %constant_array_index_test() {
	%Array = alloca int, uint 100
	%P1 = getelementptr int* %Array, uint 7
	%P2 = getelementptr int* %Array, uint 6
	
	%A = load int* %P1
	store int 1, int* %P2   ; Should not invalidate load
	%BREMOVE = load int* %P1
	%Val = sub int %A, %BREMOVE
	ret int %Val
}

