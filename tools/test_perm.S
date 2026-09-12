/* test_perm.S — W7 (Task 2.4) 多用户权限测试。
 *
 * 流程：
 *   open("/tmp/secret.txt", O_WRONLY|O_CREAT|O_TRUNC, 0)  -> fd (root 创建)
 *   write(fd, "SECR", 4)
 *   close(fd)
 *   setuid(1000)
 *   open("/tmp/secret.txt", O_RDONLY)   -> 预期 -13 (EACCES)
 *   打印 "perm-test PASS" 或 "perm-test FAIL"
 *
 * 构建：as --64 -o tools\test_perm.o tools\test_perm.S
 *       ld -e _start --oformat elf64-x86-64 -Ttext 0x400000 \
 *          -o tools\test_perm tools\test_perm.o
 */

.section .text
.global _start
_start:
    /* open("/tmp/secret.txt", O_WRONLY|O_CREAT|O_TRUNC, 0)
       flags = 1 | 0x40 | 0x200 = 0x241 ; mode = 0 */
    movq $2, %rax
    leaq path(%rip), %rdi
    movq $0x241, %rsi
    movq $0, %rdx
    syscall
    movq %rax, %r15          /* fd */

    /* write(fd, data, 4) */
    movq $1, %rax
    movq %r15, %rdi
    leaq data(%rip), %rsi
    movq $4, %rdx
    syscall

    /* close(fd) */
    movq $3, %rax
    movq %r15, %rdi
    syscall

    /* setuid(1000) */
    movq $105, %rax
    movq $1000, %rdi
    syscall

    /* open("/tmp/secret.txt", O_RDONLY) -> 预期 -13 */
    movq $2, %rax
    leaq path(%rip), %rdi
    movq $0, %rsi
    syscall
    cmpq $-13, %rax
    je  pass

fail:
    /* write(1, fmsg, 15) "perm-test FAIL\n" */
    movq $1, %rax
    movq $1, %rdi
    leaq fmsg(%rip), %rsi
    movq $15, %rdx
    syscall
    /* exit(1) */
    movq $60, %rax
    movq $1, %rdi
    syscall

pass:
    /* write(1, pmsg, 15) "perm-test PASS\n" */
    movq $1, %rax
    movq $1, %rdi
    leaq pmsg(%rip), %rsi
    movq $15, %rdx
    syscall
    /* exit(0) */
    movq $60, %rax
    xorq %rdi, %rdi
    syscall

.section .data
path:  .asciz "/tmp/secret.txt"
data:  .asciz "SECR"
pmsg:  .asciz "perm-test PASS\n"
fmsg:  .asciz "perm-test FAIL\n"
