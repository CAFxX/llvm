;This isn't really an assembly file, its just here to run the test.
;This test just makes sure that llvm-ar can generate a symbol table for
;xpg4 style archives
;RUN: cp %p/xpg4.a xpg4_copy.a
;RUN: llvm-ranlib xpg4_copy.a
;RUN: llvm-ar t xpg4_copy.a > %t1
;RUN: sed -e '/^;.*/d' %s >%t2
;RUN: diff %t2 %t1
evenlen
oddlen
very_long_bytecode_file_name.bc
IsNAN.o
