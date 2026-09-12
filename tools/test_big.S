/* test_big.S — Ring 3 测试程序：连续写 1MB 到 /tmp/big.bin（w 模式），
 * 重启后重读校验（v 模式）。验证 ext2_write_file 大文件路径（>12 直块 →
 * 单间接块）在分块连续写入下无数据丢失。
 *
 * 运行环境：Axion 内核用户态 (Ring 3)，使用真实 `syscall` 指令（Linux ABI：
 * rax=nr, rdi=a1, rsi=a2, rdx=a3）。_start 时栈布局：rsp->argc,
 * rsp+8->argv[0], rsp+16->argv[1]。
 *
 * 用法：
 *   run /bin/test_big w   -> open("/tmp/big.bin", O_WRONLY|O_CREAT|O_TRUNC)
 *                            按 1KB 分块循环 write 共 1MB；数据模式：
 *                            第 i 字节 = (i*31 + i/8) % 251；内存累计
 *                            32 位校验和；打印 "big-test WROTE checksum=<v>"
 *   run /bin/test_big v   -> open(O_RDONLY) 循环 read 1KB 共 1MB；重算校验和
 *                            与预期比对（并逐字节核对数据模式）；
 *                            一致打印 "big-test PASS"，否则 "big-test FAIL"
 *
 * 分块循环状态保存在被调用者保存寄存器（r12-r15/rbx），syscall 不会破坏。
 */

.section .text
.global _start
_start:
    cmpq $2, (%rsp)             /* argc >= 2？ */
    jb   usage_exit

    movq 8(%rsp), %r13          /* argv[0]（本程序路径） */
    movq 16(%rsp), %r14         /* argv[1] */
    movb (%r14), %al
    cmpb $'w', %al
    je   mode_w
    cmpb $'v', %al
    je   mode_v
    jmp  usage_exit

/* ================= 写模式 ================= */
mode_w:
    /* open("/tmp/big.bin", O_WRONLY|O_CREAT|O_TRUNC, 0644) */
    movq $2, %rax
    leaq path(%rip), %rdi
    movl $0x241, %esi           /* O_WRONLY(1)|O_CREAT(0x40)|O_TRUNC(0x200) */
    movl $0644, %edx
    syscall
    testq %rax, %rax
    js   fail_open
    movq %rax, %r12             /* fd */
    xorl %r13d, %r13d           /* 分块序号 0..1023 */
    xorl %r15d, %r15d           /* 32 位校验和 */
    movq $251, %r10             /* 取模常数 */

w_loop:
    /* 生成 1KB 数据块：buf[j] = ((base+j)*31 + (base+j)/8) % 251 */
    leaq buf(%rip), %rdi
    movq %r13, %r11
    shlq $10, %r11              /* base = 分块序号 * 1024 */
    xorl %ecx, %ecx             /* j = 0..1023 */
w_fill:
    movq %r11, %r8
    addq %rcx, %r8              /* i = base + j */
    movq %r8, %r9
    imulq $31, %r9, %r9
    shrq $3, %r8
    addq %r8, %r9               /* i*31 + i/8 */
    movq %r9, %rax
    xorl %edx, %edx
    divq %r10                   /* rdx = (i*31+i/8) % 251 */
    movb %dl, (%rdi,%rcx)       /* buf[j] = byte */
    addl %edx, %r15d            /* 校验和累计 */
    incq %rcx
    cmpq $1024, %rcx
    jb   w_fill

    /* write(fd, buf, 1024) */
    movq $1, %rax
    movq %r12, %rdi
    leaq buf(%rip), %rsi
    movq $1024, %rdx
    syscall
    testq %rax, %rax
    js   fail_write

    incq %r13
    cmpq $1024, %r13
    jb   w_loop

    /* write(1, "big-test WROTE checksum=", 24) */
    movq $1, %rax
    movq $1, %rdi
    leaq msg_wrote(%rip), %rsi
    movq $24, %rdx
    syscall

    /* 打印校验和（十进制） */
    movl %r15d, %eax
    call dec_out
    movq $1, %rax
    movq $1, %rdi
    syscall

    /* write(1, "\n", 1) */
    movq $1, %rax
    movq $1, %rdi
    leaq msg_nl(%rip), %rsi
    movq $1, %rdx
    syscall
    jmp  exit0

/* ================= 校验模式 ================= */
mode_v:
    /* open("/tmp/big.bin", O_RDONLY) */
    movq $2, %rax
    leaq path(%rip), %rdi
    xorl %esi, %esi
    xorl %edx, %edx
    syscall
    testq %rax, %rax
    js   fail_open
    movq %rax, %r12             /* fd */
    xorl %r13d, %r13d           /* 分块序号 */
    xorl %r15d, %r15d           /* 预期校验和（按数据模式累计） */
    xorl %ebx, %ebx             /* 实际校验和（按读回数据累计） */
    xorl %r14d, %r14d           /* 数据不符标志 */
    movq $251, %r10

v_loop:
    /* read(fd, buf, 1024) */
    movq $0, %rax
    movq %r12, %rdi
    leaq buf(%rip), %rsi
    movq $1024, %rdx
    syscall
    testq %rax, %rax
    jle  v_done                 /* EOF / 错误：由分块数检查判定 FAIL */
    cmpq $1024, %rax
    jne  v_fail                 /* 短读：FAIL */

    /* 逐字节核对数据模式并累计校验和 */
    movq %r13, %r11
    shlq $10, %r11              /* base */
    xorl %ecx, %ecx
v_check:
    movq %r11, %r8
    addq %rcx, %r8              /* i */
    movq %r8, %r9
    imulq $31, %r9, %r9
    shrq $3, %r8
    addq %r8, %r9
    movq %r9, %rax
    xorl %edx, %edx
    divq %r10                   /* rdx = 预期字节 */
    addl %edx, %r15d            /* 预期校验和累计 */
    movzbl (%rsi,%rcx), %r8d    /* 实际字节（读回数据） */
    addl %r8d, %ebx             /* 实际校验和累计 */
    cmpb %dl, %r8b
    je   v_next
    movl $1, %r14d              /* 数据不一致 */
v_next:
    incq %rcx
    cmpq $1024, %rcx
    jb   v_check
    incq %r13
    cmpq $1024, %r13
    jb   v_loop

v_done:
    cmpq $1024, %r13
    jne  v_fail                 /* 没读满 1024 块 */
    testq %r14, %r14
    jnz  v_fail                 /* 存在数据不一致 */
    cmpl %ebx, %r15d
    jne  v_fail                 /* 校验和不一致 */

    /* write(1, "big-test PASS\n", 15) */
    movq $1, %rax
    movq $1, %rdi
    leaq msg_pass(%rip), %rsi
    movq $15, %rdx
    syscall
    jmp  exit0

v_fail:
    /* write(1, "big-test FAIL\n", 15) */
    movq $1, %rax
    movq $1, %rdi
    leaq msg_fail(%rip), %rsi
    movq $15, %rdx
    syscall
    jmp  exit0

/* ================= 辅助 ================= */
usage_exit:
    movq $1, %rax
    movq $1, %rdi
    leaq msg_usage(%rip), %rsi
    movq $20, %rdx
    syscall
    jmp  exit1

fail_open:
    movq $1, %rax
    movq $1, %rdi
    leaq msg_ofail(%rip), %rsi
    movq $21, %rdx
    syscall
    jmp  exit1

fail_write:
    movq $1, %rax
    movq $1, %rdi
    leaq msg_wfail(%rip), %rsi
    movq $22, %rdx
    syscall
    jmp  exit1

exit0:
    movq $60, %rax
    xorl %edi, %edi
    syscall

exit1:
    movq $60, %rax
    movq $1, %rdi
    syscall

/* dec_out：把 eax（uint32）转十进制串到 decbuf（倒序生成），
 * 返回 rsi=串首地址、rdx=长度。只破坏 rax/rcx/rdx/rdi/rsi。 */
dec_out:
    leaq decbuf+32(%rip), %rdi
    movl $10, %ecx
1:  xorl %edx, %edx
    divl %ecx
    addl $'0', %edx
    decq %rdi
    movb %dl, (%rdi)
    testl %eax, %eax
    jnz  1b
    movq %rdi, %rsi
    leaq decbuf+32(%rip), %rdx
    subq %rdi, %rdx
    ret

.section .rodata
path:      .asciz "/tmp/big.bin"
msg_wrote: .asciz "big-test WROTE checksum="
msg_pass:  .asciz "big-test PASS\n"
msg_fail:  .asciz "big-test FAIL\n"
msg_ofail: .asciz "big-test open failed\n"
msg_wfail: .asciz "big-test write failed\n"
msg_usage: .asciz "usage: big-test w|v\n"
msg_nl:    .asciz "\n"

.section .bss
.balign 16
buf:
    .zero 1024
decbuf:
    .zero 32
