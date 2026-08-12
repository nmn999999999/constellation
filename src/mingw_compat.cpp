// MinGW static-linking shim.
//
// The static winpthread library (libpthread.a) references __intrinsic_setjmpex,
// an SEH setjmp intrinsic normally exported by the Universal C Runtime. This
// program never creates threads, so that reference is never executed. Defining
// it here lets the executables link winpthread statically and run on stock
// Windows with no MinGW runtime DLLs.
//
// Returning 0 matches setjmp's first-return semantics.
extern "C" int __intrinsic_setjmpex(void * /*jmp_buf*/) {
    return 0;
}