// XFAIL: linux,sun,darwin

union foo {
  struct { char A, B; } X;
  int C;
};

union foo V = { {1, 2} };
