; Level raise is making an incorrect transformation, which causes incorrect 
; bytecode to be generated.
;
; RUN: as < %s | opt -raise | dis
;

	%Village = type { [4 x \3 *], \2 *, { \2 *, { int, int, int, \5 * } *, \2 * }, { int, int, int, { \2 *, { int, int, int, \6 * } *, \2 * }, { \2 *, { int, int, int, \6 * } *, \2 * }, { \2 *, { int, int, int, \6 * } *, \2 * }, { \2 *, { int, int, int, \6 * } *, \2 * } }, int, int }
implementation

%Village *"get_results"(%Village * %village)
begin
bb0:					;[#uses=1]
	%cast121 = cast int 24 to %Village *		; <%Village *> [#uses=1]
	%reg123 = add %Village * %village, %cast121		; <%Village *> [#uses=1]
	%idx = getelementptr %Village * %reg123, uint 0, ubyte 0, uint 0		; <%Village *> [#uses=1]
	%reg118 = load %Village** %idx
	ret %Village *%reg118
end
