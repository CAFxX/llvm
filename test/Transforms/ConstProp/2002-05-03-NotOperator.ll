; This bug has to do with the fact that constant propogation was implemented in
; terms of _logical_ not (! in C) instead of _bitwise_ not (~ in C).  This was
; due to a spec change.

; Fix #2: The unary not instruction now no longer exists. Change to xor.

; RUN: if as < %s | opt -constprop | dis | grep 'int 0'
; RUN: then exit 1
; RUN: else exit 0
; RUN: fi

int "test1"() {
	%R = xor int 123, -1
	ret int %R
}

int "test2"() {
	%R = xor int -123, -1
	ret int %R
}
