; test unconditional branch
void %main() {
	br label %Test
Test:
	%X = seteq int 0, 4
	br bool %X, label %Test, label %Label
Label:
	ret void
}
