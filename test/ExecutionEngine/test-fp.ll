
double %test(double* %DP, double %Arg) {
	%D = load double* %DP
	%V = add double %D, 1.0
	%W = sub double %V, %V
	%X = mul double %W, %W
	%Y = div double %X, %X
	%Z = rem double %Y, %Y
	%Q = add double %Z, %Arg
	store double %Q, double* %DP
	ret double %Z
}

int %main() { 
  %X = alloca double
  call double %test(double* %X, double 1.0)
  ret int 0 
}
