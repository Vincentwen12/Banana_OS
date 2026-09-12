#ifndef CONTEXT_H
#define CONTEXT_H

#include "axion.h"

/*
 * Minimal setjmp/longjmp for kernel context unwind (W6 process exit).
 *
 * The kernel is compiled with the SysV ABI; the callee-saved registers
 * rbx/rbp/r12-r15 plus rsp and the return address are all that need to be
 * saved to resume a kernel stack frame later. `kernel_setjmp` returns 0 on the
 * initial call and the `val` passed to `kernel_longjmp` when unwinding back.
 */
typedef uint64_t jmp_buf[8];

int  kernel_setjmp(jmp_buf env);
void kernel_longjmp(jmp_buf env, int val);

#endif /* CONTEXT_H */