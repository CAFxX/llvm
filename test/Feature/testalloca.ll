%inners = type {float, {ubyte } }
%struct = type { int , {float, {ubyte } } , ulong }

implementation

int "test function"(int %i0, int %j0)
begin
    alloca [ubyte], uint 5
    %ptr = alloca int                       ; yields {int*}:ptr
    store int 3, int* %ptr                  ; yields {void}
    %val = load int* %ptr                   ; yields {int}:val = int %3

    %sptr = alloca %struct                  ; yields {%struct*}:sptr
    %nsptr = getelementptr %struct * %sptr, ubyte 1  ; yields {inners*}:nsptr
    %ubsptr = getelementptr %inners * %nsptr, ubyte 1  ; yields {{ubyte}*}:ubsptr
    store ubyte 4, {ubyte} * %ubsptr, ubyte 0
    
    %fptr = getelementptr %struct * %sptr, ubyte 1, ubyte 0  ; yields {float*}:fptr
    store float 4.0, float * %fptr
    
    ret int 3
end

