; This test makes sure that these instructions are properly eliminated.
;

; RUN: if as < %s | opt -instcombine -dce | dis | grep set
; RUN: then exit 1
; RUN: else exit 0
; RUN: fi

implementation

bool "test1"(int %A) {
	%B = seteq int %A, %A
	ret bool %B
}

bool "test2"(int %A) {
	%B = setne int %A, %A
	ret bool %B
}

bool "test3"(int %A) {
	%B = setlt int %A, %A
	ret bool %B
}

bool "test4"(int %A) {
	%B = setgt int %A, %A
	ret bool %B
}

bool "test5"(int %A) {
	%B = setle int %A, %A
	ret bool %B
}

bool "test6"(int %A) {
	%B = setge int %A, %A
	ret bool %B
}
