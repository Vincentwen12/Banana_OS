/* test_shm.S — Ring 3 SysV 共享内存测试（Task 3.2）。
 *
 * 流程（与 test_fork.S 相同的同步 fork/wait4 模型）：
 *   shmget(0x1234, 4096, 0) -> shmid
 *   shmat(shmid, 0, 0)      -> vaddr（内核默认基址 0x6000000000 + id*0x1000）
 *   父：写 "hello-shm" 到 offset 0
 *   fork()
 *   子：shmget 同 key -> shmat -> 校验 offset 0 == "hello-shm"
 *       -> 写 "child-shm" 到 offset 64 -> exit(0)（wait4 内同步运行）
 *   父：wait4(子) 后校验自己映射 offset 64 == "child-shm"
 *       -> 打印 "shm-test PASS\n" -> exit(0)
 *
 * 关键点：fork() 深拷贝页表后，子进程再次 shmat 会把共享物理页覆盖到
 * 同一 vaddr 上，从而父子真正共享同一物理页——子写、父读可见。
 */

.section .text
.global _start
_start:
    /* shmget(0x1234, 4096, 0) -> rax = shmid */
    movq $29, %rax
    movq $0x1234, %rdi
    movq $4096, %rsi
    xorq %rdx, %rdx
    xorq %r10, %r10
    syscall
    cmpq $-1, %rax
    je   fail
    movq %rax, %r12            /* shmid */

    /* shmat(shmid, 0, 0) -> rax = vaddr */
    movq $30, %rax
    movq %r12, %rdi
    xorq %rsi, %rsi
    xorq %rdx, %rdx
    xorq %r10, %r10
    syscall
    cmpq $-1, %rax
    je   fail
    movq %rax, %r13            /* shm vaddr */

    /* 父：写 "hello-shm\0"（10 字节）到 offset 0 */
    leaq hello(%rip), %rsi
    movq %r13, %rdi
    movq $10, %rcx
1:  movb (%rsi), %al
    movb %al, (%rdi)
    incq %rsi
    incq %rdi
    decq %rcx
    jnz  1b

    /* fork() */
    movq $57, %rax
    syscall
    cmpq $0, %rax
    je   child

    /* ---- parent ---- */
    movq %rax, %r14            /* child pid */

    /* wait4(child, &status, 0, 0) — 内核同步运行子进程至退出 */
    movq $61, %rax
    movq %r14, %rdi
    leaq status(%rip), %rsi
    xorq %rdx, %rdx
    xorq %r10, %r10
    syscall

    /* 父：校验自己映射 offset 64 == "child-shm\0"（10 字节） */
    leaq cexp(%rip), %rsi
    movq %r13, %rdi
    addq $64, %rdi
    movq $10, %rcx
2:  movb (%rsi), %al
    cmpb %al, (%rdi)
    jne  fail
    incq %rsi
    incq %rdi
    decq %rcx
    jnz  2b

    /* write(1, "shm-test PASS\n", 14) */
    movq $1, %rax
    movq $1, %rdi
    leaq pass(%rip), %rsi
    movq $14, %rdx
    syscall

    /* exit(0) */
    movq $60, %rax
    xorq %rdi, %rdi
    syscall

child:
    /* 子：shmget(0x1234, 4096, 0) -> 复用父创建的段 */
    movq $29, %rax
    movq $0x1234, %rdi
    movq $4096, %rsi
    xorq %rdx, %rdx
    xorq %r10, %r10
    syscall
    cmpq $-1, %rax
    je   fail
    movq %rax, %r12

    /* 子：shmat(shmid, 0, 0) -> 共享物理页覆盖 fork 拷贝 */
    movq $30, %rax
    movq %r12, %rdi
    xorq %rsi, %rsi
    xorq %rdx, %rdx
    xorq %r10, %r10
    syscall
    cmpq $-1, %rax
    je   fail
    movq %rax, %r13

    /* 子：校验 offset 0 == "hello-shm\0"（父 fork 前写入） */
    leaq hello(%rip), %rsi
    movq %r13, %rdi
    movq $10, %rcx
3:  movb (%rsi), %al
    cmpb %al, (%rdi)
    jne  fail
    incq %rsi
    incq %rdi
    decq %rcx
    jnz  3b

    /* 子：写 "child-shm\0"（10 字节）到 offset 64（父映射可见） */
    leaq cwrite(%rip), %rsi
    movq %r13, %rdi
    addq $64, %rdi
    movq $10, %rcx
4:  movb (%rsi), %al
    movb %al, (%rdi)
    incq %rsi
    incq %rdi
    decq %rcx
    jnz  4b

    /* exit(0) */
    movq $60, %rax
    xorq %rdi, %rdi
    syscall

fail:
    /* write(1, "shm-test FAIL\n", 14) */
    movq $1, %rax
    movq $1, %rdi
    leaq fmsg(%rip), %rsi
    movq $14, %rdx
    syscall

    /* exit(1) */
    movq $60, %rax
    movq $1, %rdi
    syscall

.section .data
hello:  .asciz "hello-shm"     /* 10 字节：9 字符 + NUL */
cwrite: .asciz "child-shm"     /* 10 字节 */
cexp:   .asciz "child-shm"     /* 10 字节 */
pass:   .ascii "shm-test PASS\n"   /* 14 字节 */
fmsg:   .ascii "shm-test FAIL\n"   /* 14 字节 */
status: .quad 0
