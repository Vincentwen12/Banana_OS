/* sleep.S — Task 3.1 job control 演示工具。
 *
 * busy-wait 约 2 秒后 write(1, "sleep done\n") 再 exit(0)。
 * 注意：QEMU TCG 下 rdtsc 时间不可靠，这里用大计数循环，
 * 循环次数取大一点（20 亿次 dec/jnz，常见宿主上约 1-4 秒）。
 *
 * 构建：as --64 -o tools\sleep.o tools\sleep.S
 *       ld -e _start --oformat elf64-x86-64 -Ttext 0x400000 \
 *          -o tools\sleep tools\sleep.o
 */

.section .text
.global _start
_start:
    /* 计数器循环（r12 为被调用者保存寄存器，syscall 不会破坏） */
    movq $2000000000, %r12
1:  decq %r12
    jnz 1b

    /* write(1, msg, 11) "sleep done\n" */
    movq $1, %rax
    movq $1, %rdi
    leaq msg(%rip), %rsi
    movq $11, %rdx
    syscall

    /* exit(0) */
    movq $60, %rax
    xorq %rdi, %rdi
    syscall

.section .rodata
msg: .asciz "sleep done\n"
