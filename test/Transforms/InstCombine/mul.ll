; This test makes sure that mul instructions are properly eliminated.
;

; RUN: if as < %s | opt -instcombine -dce | dis | grep mul
; RUN: then exit 1
; RUN: else exit 0
; RUN: fi

implementation

int "test1"(int %A)
begin
	%B = mul int %A, 1
	ret int %B
end

int "test2"(int %A)
begin
	%B = mul int %A, 2   ; Should convert to an add instruction
	ret int %B
end

int "test3"(int %A)
begin
	%B = mul int %A, 0   ; This should disappear entirely
	ret int %B
end

