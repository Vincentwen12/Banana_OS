/* test_fault.S — Ring 3 test program triggering a user-mode page fault.
 *
 * Runs under the Axion-Ban kernel user mode (Ring 3) using the real `syscall`
 * instruction.  Prints a marker, then writes to address 0 (NULL dereference)
 * which raises #PF (vector 14).  The kernel must kill this task with
 * SIGSEGV (11) instead of halting the whole system, and the shell/bash must
 * get back control.  No exit() is needed — the task dies on the fault.
 */

.section .text
.global _start
_start:
    /* write(1, msg, 17) */
    movq $1, %rax
    movq $1, %rdi
    leaq msg(%rip), %rsi
    movq $17, %rdx
    syscall

    /* NULL dereference: store to 0x0 -> #PF -> SIGSEGV */
    xorq %rax, %rax
    movl $0, (%rax)

    /* Should never be reached (task killed by the fault). */
1:  jmp 1b

.section .data
msg:    .asciz "fault test start\n"
