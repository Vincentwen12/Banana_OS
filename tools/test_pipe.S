/* test_pipe.S — Ring 3 test program exercising W7 Phase 2.1 syscalls:
 *   pipe(22) / write(1) / read(0) / readv(19) / poll(7) / getdents64(217)
 * Expected VGA output:
 *   pipe test start
 *   pipe-ok
 *   readv-ok
 *   poll-ok
 *   getdents-ok
 *   pipe test PASS
 * On failure: "pipe fail <stage>" then exit(1).
 */

.macro STAGE n
    movb $\n, stage(%rip)
.endm

.section .text
.global _start
_start:
    /* write(1, msg1, 17) */
    movq $1, %rax
    movq $1, %rdi
    leaq msg1(%rip), %rsi
    movq $17, %rdx
    syscall

    /* ---- 1. pipe() + write/read roundtrip ---- */
    STAGE 1
    movq $22, %rax            /* sys_pipe */
    leaq fds(%rip), %rdi
    syscall
    testq %rax, %rax
    jnz  fail

    /* 提取 fds：pipe() 写入两个连续 int32 → quad 低 32 位 = fds[0]（读端），
     * 高 32 位 = fds[1]（写端）。r14/r15 为 callee-saved，后续段 r12/r13
     * 会被 getdents64 的 dirfd/nread 复用，故管道 fd 保存在 r14/r15。 */
    STAGE 2
    movq fds(%rip), %rax
    movl %eax, %r14d          /* r14 = 读端 fd */
    shrq $32, %rax
    movl %eax, %r15d          /* r15 = 写端 fd */

    /* write(fds[1], "hello", 5) */
    STAGE 3
    movq $1, %rax
    movq %r15, %rdi
    leaq payload(%rip), %rsi
    movq $5, %rdx
    syscall
    cmpq $5, %rax
    jne  fail

    /* read(fds[0], buf, 5) */
    STAGE 4
    movq $0, %rax
    movq %r14, %rdi
    leaq buf(%rip), %rsi
    movq $5, %rdx
    syscall
    cmpq $5, %rax
    jne  fail

    /* verify "hello" — 内联逐字节检查（不用 call/ret，避免栈相关问题） */
    STAGE 5
    leaq buf(%rip), %rsi
    movb 0(%rsi), %al
    cmpb $'h', %al
    jne  fail
    movb 1(%rsi), %al
    cmpb $'e', %al
    jne  fail
    movb 2(%rsi), %al
    cmpb $'l', %al
    jne  fail
    movb 3(%rsi), %al
    cmpb $'l', %al
    jne  fail
    movb 4(%rsi), %al
    cmpb $'o', %al
    jne  fail

    /* write(1, pmsg, 9)  "pipe-ok\n" */
    movq $1, %rax
    movq $1, %rdi
    leaq pmsg(%rip), %rsi
    movq $9, %rdx
    syscall

    /* 再写一份 "hello" 供 readv 消费 */
    STAGE 6
    movq $1, %rax
    movq %r15, %rdi
    leaq payload(%rip), %rsi
    movq $5, %rdx
    syscall
    cmpq $5, %rax
    jne  fail

    /* ---- 2. readv(19): read 5 bytes into 2 iovecs (3 + 2) ---- */
    STAGE 7
    leaq iov(%rip), %rsi      /* iov[] */
    movq %r14, %rdi           /* read fd */
    movq $2, %rdx             /* iovcnt */
    movq $19, %rax            /* sys_readv */
    syscall
    cmpq $5, %rax
    jne  fail
    /* verify iov0 buf "hel" + iov1 buf "lo" */
    STAGE 8
    leaq rvbuf0(%rip), %rsi
    leaq payload(%rip), %rdi
    movq $3, %rcx
    call memcmp
    testq %rax, %rax
    jnz  fail
    leaq rvbuf1(%rip), %rsi
    leaq payload+3(%rip), %rdi
    movq $2, %rcx
    call memcmp
    testq %rax, %rax
    jnz  fail

    /* write(1, rmsg, 11)  "readv-ok\n" */
    movq $1, %rax
    movq $1, %rdi
    leaq rmsg(%rip), %rsi
    movq $11, %rdx
    syscall

    /* ---- 3. poll(7): write end always writable ---- */
    /* pfd.fd = fds[1] (write end, saved in r15) */
    STAGE 9
    movl %r15d, %eax
    movl %eax, pfd(%rip)
    leaq pfd(%rip), %rdi      /* struct pollfd { int fd; short events; short revents; } */
    movq $1, %rsi             /* nfds */
    movq $0, %rdx             /* timeout = 0 (non-blocking) */
    movq $7, %rax             /* sys_poll */
    syscall
    testq %rax, %rax
    jle  fail
    /* revents & POLLOUT (0x4) */
    movzwq pfd+6(%rip), %rcx
    andq $4, %rcx
    jz   fail

    /* write(1, lmsg, 9)  "poll-ok\n" */
    movq $1, %rax
    movq $1, %rdi
    leaq lmsg(%rip), %rsi
    movq $9, %rdx
    syscall

    /* ---- 4. getdents64(217) on "/" ---- */
    /* open("/", O_RDONLY) */
    STAGE 10
    movq $2, %rax
    leaq rootpath(%rip), %rdi
    movq $0, %rsi
    movq $0, %rdx
    syscall
    testq %rax, %rax
    js   fail
    movq %rax, %r12           /* dirfd */

    /* getdents64(dirfd, dbuf, 4096) */
    STAGE 11
    movq $217, %rax
    movq %r12, %rdi
    leaq dbuf(%rip), %rsi
    movq $4096, %rdx
    syscall
    testq %rax, %rax
    jle  fail
    movq %rax, %r13           /* nread */
    /* 首个条目 d_name 至少 1 字节，d_reclen 非 0 */
    movzwq dbuf+16(%rip), %rcx
    testq %rcx, %rcx
    jz   fail

    /* close(dirfd) */
    movq $3, %rax
    movq %r12, %rdi
    syscall

    /* write(1, gmsg, 13)  "getdents-ok\n" */
    movq $1, %rax
    movq $1, %rdi
    leaq gmsg(%rip), %rsi
    movq $13, %rdx
    syscall

    /* ---- 5. close pipe ends ---- */
    movq $3, %rax
    movq %r14, %rdi
    syscall
    movq $3, %rax
    movq %r15, %rdi
    syscall

    /* write(1, done, 16)  "pipe test PASS\n" */
    movq $1, %rax
    movq $1, %rdi
    leaq done(%rip), %rsi
    movq $16, %rdx
    syscall

    /* exit(0) */
    movq $60, %rax
    xorq %rdi, %rdi
    syscall

fail:
    /* 打印 "pipe fail <stage>\n"：先写 "pipe fail "，再写 stbuf（数字+\n） */
    movzbl stage(%rip), %eax
    addq $48, %rax
    movb %al, stbuf(%rip)
    movq $1, %rax
    movq $1, %rdi
    leaq fstg(%rip), %rsi
    movq $10, %rdx
    syscall
    movq $1, %rax
    movq $1, %rdi
    leaq stbuf(%rip), %rsi
    movq $2, %rdx
    syscall
    movq $60, %rax
    movq $1, %rdi
    syscall

/* memcmp(rsi, rdi, rcx) — compare rcx bytes, returns 0 if equal in rax.
 * 用 %dl 暂存字节，避免污染 rax（%al 会残留最后一个字节值）。 */
memcmp:
    xorq %rax, %rax
1:
    testq %rcx, %rcx
    jz   2f
    movb (%rsi), %dl
    cmpb (%rdi), %dl
    jne  3f
    incq %rsi
    incq %rdi
    decq %rcx
    jmp  1b
3:
    movq $1, %rax
2:
    ret

.section .data
msg1:   .asciz "pipe test start\n"
pmsg:   .asciz "pipe-ok\n"
rmsg:   .asciz "readv-ok\n"
lmsg:   .asciz "poll-ok\n"
gmsg:   .asciz "getdents-ok\n"
done:   .asciz "pipe test PASS\n"
fstg:   .asciz "pipe fail "
stage:  .byte 0
stbuf:  .byte 0, 0x0a
rootpath: .asciz "/"
payload: .asciz "hello"
fds:    .quad 0
pfd:    .long 0          /* fd（运行时填 fds[1]） */
        .short 4         /* events = POLLOUT */
        .short 0         /* revents */
iov:    .quad rvbuf0, 3
        .quad rvbuf1, 2
rvbuf0: .byte 0,0,0,0
rvbuf1: .byte 0,0,0,0
buf:    .byte 0,0,0,0,0
dbuf:   .byte 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
