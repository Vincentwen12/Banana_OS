/* test_tcp.S — Task 3.3 loopback TCP 测试程序（Ring 3, 裸 syscall）。
 *
 * 单进程顺序跑（无并发依赖）：
 *   lfd = socket(2, 1, 0)
 *   bind(lfd, 127.0.0.1:7777, 16)
 *   listen(lfd, 4)
 *   cfd = socket(2, 1, 0)
 *   connect(cfd, 127.0.0.1:7777, 16)   → 0（同步立即完成）
 *   afd = accept(lfd, &caddr, &caddrlen)
 *   write(cfd, "ping", 4) → read(afd, 4) 校验 == "ping"
 *   write(afd, "pong", 4) → read(cfd, 4) 校验 == "pong"
 *   打印 "tcp-test PASS\n" → exit(0)
 *
 * syscall 号：read=0 write=1 socket=41 connect=42 accept=43
 *             shutdown=48 bind=49 listen=50
 * 测试读写直接用 read/write syscall 走 file_ops（loop_recv/loop_send）。
 * sockaddr_in 端口大端打包：7777 = 0x1E61 → 字节 0x1E 0x61。
 */

.section .text
.global _start
_start:
    /* lfd = socket(2, 1, 0) */
    movq $41, %rax
    movq $2, %rdi
    movq $1, %rsi
    xorq %rdx, %rdx
    syscall
    testq %rax, %rax
    js   fail
    movq %rax, %r12          /* lfd */

    /* bind(lfd, &saddr, 16) */
    movq $49, %rax
    movq %r12, %rdi
    leaq saddr(%rip), %rsi
    movq $16, %rdx
    syscall
    testq %rax, %rax
    js   fail

    /* listen(lfd, 4) */
    movq $50, %rax
    movq %r12, %rdi
    movq $4, %rsi
    syscall
    testq %rax, %rax
    js   fail

    /* cfd = socket(2, 1, 0) */
    movq $41, %rax
    movq $2, %rdi
    movq $1, %rsi
    xorq %rdx, %rdx
    syscall
    testq %rax, %rax
    js   fail
    movq %rax, %r13          /* cfd */

    /* connect(cfd, &saddr, 16) */
    movq $42, %rax
    movq %r13, %rdi
    leaq saddr(%rip), %rsi
    movq $16, %rdx
    syscall
    testq %rax, %rax
    js   fail

    /* afd = accept(lfd, &caddr, &caddrlen) */
    movq $43, %rax
    movq %r12, %rdi
    leaq caddr(%rip), %rsi
    leaq caddrlen(%rip), %rdx
    syscall
    testq %rax, %rax
    js   fail
    movq %rax, %r14          /* afd */

    /* write(cfd, "ping", 4) */
    movq $1, %rax
    movq %r13, %rdi
    leaq ping(%rip), %rsi
    movq $4, %rdx
    syscall
    cmpq $4, %rax
    jne  fail

    /* read(afd, rbuf, 4) 并校验 == "ping" */
    movq $0, %rax
    movq %r14, %rdi
    leaq rbuf(%rip), %rsi
    movq $4, %rdx
    syscall
    cmpq $4, %rax
    jne  fail
    leaq rbuf(%rip), %rdi
    leaq ping(%rip), %rsi
    call chk4
    testq %rax, %rax
    jnz  fail

    /* write(afd, "pong", 4) */
    movq $1, %rax
    movq %r14, %rdi
    leaq pong(%rip), %rsi
    movq $4, %rdx
    syscall
    cmpq $4, %rax
    jne  fail

    /* read(cfd, rbuf, 4) 并校验 == "pong" */
    movq $0, %rax
    movq %r13, %rdi
    leaq rbuf(%rip), %rsi
    movq $4, %rdx
    syscall
    cmpq $4, %rax
    jne  fail
    leaq rbuf(%rip), %rdi
    leaq pong(%rip), %rsi
    call chk4
    testq %rax, %rax
    jnz  fail

    /* write(1, "tcp-test PASS\n", 14) */
    movq $1, %rax
    movq $1, %rdi
    leaq pass(%rip), %rsi
    movq $14, %rdx
    syscall

    /* exit(0) */
    movq $60, %rax
    xorq %rdi, %rdi
    syscall

fail:
    /* write(1, "tcp-test FAIL\n", 14) */
    movq $1, %rax
    movq $1, %rdi
    leaq failmsg(%rip), %rsi
    movq $14, %rdx
    syscall
    /* exit(1) */
    movq $60, %rax
    movq $1, %rdi
    syscall

/* chk4: 比较 (%rdi) 与 (%rsi) 的 4 字节；rax=0 相等，非 0 不等 */
chk4:
    xorq %rcx, %rcx
1:
    cmpq $4, %rcx
    jge  2f
    movb (%rdi,%rcx), %al
    cmpb (%rsi,%rcx), %al
    jne  3f
    incq %rcx
    jmp  1b
2:
    xorq %rax, %rax
    ret
3:
    movq $1, %rax
    ret

.section .data
saddr:
    .short 2             /* sin_family = AF_INET */
    .byte  0x1E, 0x61    /* sin_port = 7777 大端 (0x1E61) */
    .byte  127, 0, 0, 1  /* sin_addr = 127.0.0.1 */
    .zero  8             /* sin_zero */
caddr:
    .zero 16             /* accept 回写地址 */
caddrlen:
    .quad 16
rbuf:
    .zero 8
ping:
    .asciz "ping"
pong:
    .asciz "pong"
pass:
    .asciz "tcp-test PASS\n"
failmsg:
    .asciz "tcp-test FAIL\n"
