/* test_fork.S — Ring 3 test program exercising getpid/fork/wait4/exit.
 *
 * Runs under the Axion-Ban kernel user mode (Ring 3) using the real `syscall`
 * instruction.  Expected VGA output:
 *   fork test start
 *   parent
 *   child
 *   reaped
 * The parent forks a child, prints a message, blocks in wait4() (which runs
 * the child synchronously inside the kernel), and reaps it.
 */

.section .text
.global _start
_start:
    /* write(1, msg1, 16) */
    movq $1, %rax
    movq $1, %rdi
    leaq msg1(%rip), %rsi
    movq $16, %rdx
    syscall

    /* fork() */
    movq $57, %rax
    syscall
    cmpq $0, %rax
    je   child

    /* ---- parent ---- */
    movq %rax, %r13         /* child pid */

    /* write(1, pmsg, 7) */
    movq $1, %rax
    movq $1, %rdi
    leaq pmsg(%rip), %rsi
    movq $7, %rdx
    syscall

    /* wait4(child_pid, &status, 0, 0) */
    movq $61, %rax
    movq %r13, %rdi
    leaq status(%rip), %rsi
    movq $0, %rdx
    movq $0, %r10
    syscall

    /* write(1, rmsg, 7) */
    movq $1, %rax
    movq $1, %rdi
    leaq rmsg(%rip), %rsi
    movq $7, %rdx
    syscall

    /* exit(0) */
    movq $60, %rax
    xorq %rdi, %rdi
    syscall

child:
    /* write(1, cmsg, 6) */
    movq $1, %rax
    movq $1, %rdi
    leaq cmsg(%rip), %rsi
    movq $6, %rdx
    syscall

    /* exit(0) */
    movq $60, %rax
    xorq %rdi, %rdi
    syscall

.section .data
msg1:   .asciz "fork test start\n"
pmsg:   .asciz "parent\n"
cmsg:   .asciz "child\n"
rmsg:   .asciz "reaped\n"
status: .quad 0
