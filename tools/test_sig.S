/* test_sig.S — W7 (Task 2.5) 信号投递测试。
 *
 * 流程：
 *   write(1, "sig test start\n")
 *   rt_sigaction(15(SIGTERM), &act, 0, 8)  注册 handler
 *   kill(getpid(), 15)     -> syscall 返回路径投递信号
 *   handler 打印 "sig-handler-ok\n" 后 ret 到内核 restorer
 *     （act.sa_flags=0 -> 用内核桩 0x7FFFFFF00000，执行 rt_sigreturn）
 *   rt_sigreturn 恢复被中断上下文 -> 主流程 exit(0)
 *
 * 构建：as --64 -o tools\test_sig.o tools\test_sig.S
 *       ld -e _start --oformat elf64-x86-64 -Ttext 0x400000 \
 *          -o tools\test_sig tools\test_sig.o
 */

.section .text
.global _start
_start:
    /* write(1, start_msg, 15) "sig test start\n" */
    movq $1, %rax
    movq $1, %rdi
    leaq start_msg(%rip), %rsi
    movq $15, %rdx
    syscall

    /* rt_sigaction(15, &act, 0, 8) */
    movq $13, %rax
    movq $15, %rdi
    leaq act(%rip), %rsi
    movq $0, %rdx
    movq $8, %r10
    syscall

    /* getpid() */
    movq $39, %rax
    syscall
    movq %rax, %r15

    /* kill(pid, SIGTERM=15) — 信号在 syscall 返回路径投递，
       handler 运行完后 rt_sigreturn 恢复到这里继续 */
    movq $62, %rax
    movq %r15, %rdi
    movq $15, %rsi
    syscall

    /* exit(0) */
    movq $60, %rax
    xorq %rdi, %rdi
    syscall

/* SIGTERM handler */
handler:
    /* write(1, h_msg, 15) "sig-handler-ok\n" */
    movq $1, %rax
    movq $1, %rdi
    leaq h_msg(%rip), %rsi
    movq $15, %rdx
    syscall
    ret   /* 弹 sigframe[0] = restorer（内核桩 -> rt_sigreturn） */

.section .data
start_msg: .asciz "sig test start\n"
h_msg:     .asciz "sig-handler-ok\n"
act:       .quad handler    /* sa_handler */
           .quad 0          /* sa_flags (0 -> 内核 restorer 桩) */
           .quad 0          /* sa_restorer */
