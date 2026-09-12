/* test_time.S — W7 Phase 2.2 时间/同步 syscall 冒烟测试：
 *   clock_gettime(228) / clock_getres(114) / nanosleep(35) / alarm(37)
 * Expected output:
 *   time test start
 *   clock-ok
 *   sleep-ok
 *   alarm-ok
 *   time test PASS
 */

.macro STAGE n
    movb $\n, stage(%rip)
.endm

.section .text
.global _start
_start:
    /* write(1, msg1, 16) */
    movq $1, %rax
    movq $1, %rdi
    leaq msg1(%rip), %rsi
    movq $16, %rdx
    syscall

    /* ---- 1. clock_gettime(CLOCK_MONOTONIC, &t0) ---- */
    STAGE 1
    movq $228, %rax
    movq $1, %rdi              /* CLOCK_MONOTONIC */
    leaq t0(%rip), %rsi
    syscall
    testq %rax, %rax
    jnz  fail
    /* t0.sec 必须非零（已运行 > 1s）或 nsec 在合理范围 */
    movq t0(%rip), %rcx        /* sec */
    movq t0+8(%rip), %rdx      /* nsec */
    cmpq $0, %rdx
    jl   fail                  /* nsec 无符号，仅校验 < 1e9 */
    movq $1000000000, %rax
    cmpq %rax, %rdx
    jge  fail

    /* ---- 2. clock_getres(0, &res) — 1ms ---- */
    STAGE 2
    movq $229, %rax
    movq $0, %rdi
    leaq res(%rip), %rsi
    syscall
    testq %rax, %rax
    jnz  fail
    cmpq $1000000, res+8(%rip) /* nsec == 1ms */
    jne  fail

    /* ---- 3. nanosleep(200ms, 0) — 挂起约 200ms ---- */
    STAGE 3
    movq $35, %rax
    leaq req(%rip), %rdi       /* {0 sec, 200000000 nsec} */
    movq $0, %rsi              /* no rem */
    syscall
    testq %rax, %rax
    jnz  fail

    /* ---- 4. clock_gettime 再次采样，elapsed >= 150ms ---- */
    STAGE 4
    movq $228, %rax
    movq $1, %rdi
    leaq t1(%rip), %rsi
    syscall
    testq %rax, %rax
    jnz  fail
    /* elapsed_ms = (t1.sec - t0.sec)*1000 + nsec_diff/1e6
     * nsec_diff = t1.nsec - t0.nsec，为负时向 sec 借 1s。 */
    movq t1(%rip), %rax
    subq t0(%rip), %rax        /* sec diff */
    imulq $1000, %rax, %rax    /* sec diff -> ms */
    movq t1+8(%rip), %rcx
    subq t0+8(%rip), %rcx      /* nsec diff（可能为负） */
    movq %rcx, %rsi
    shrq $63, %rsi             /* 符号位：为负则借 1s */
    imulq $1000000000, %rsi, %rsi
    addq %rsi, %rcx            /* 修正后的 nsec diff */
    shrq $20, %rcx             /* /1e6 近似（>>20 = /1048576） */
    addq %rcx, %rax            /* total ms */
    cmpq $150, %rax
    jl   fail

    /* write(1, cmsg, 10)  "clock-ok\n"  (sec diff + nsec） */
    movq $1, %rax
    movq $1, %rdi
    leaq cmsg(%rip), %rsi
    movq $10, %rdx
    syscall

    /* ---- 5. alarm(1) 返回 0（无上一定时）；alarm(0) 取消 ---- */
    STAGE 5
    movq $37, %rax
    movq $1, %rdi
    syscall
    testq %rax, %rax
    jnz  fail
    movq $37, %rax
    movq $0, %rdi
    syscall

    /* write(1, smsg, 10)  "sleep-ok\n" */
    movq $1, %rax
    movq $1, %rdi
    leaq smsg(%rip), %rsi
    movq $10, %rdx
    syscall

    /* write(1, amsg, 10)  "alarm-ok\n" */
    movq $1, %rax
    movq $1, %rdi
    leaq amsg(%rip), %rsi
    movq $10, %rdx
    syscall

    /* write(1, done, 16)  "time test PASS\n" */
    movq $1, %rax
    movq $1, %rdi
    leaq done(%rip), %rsi
    movq $16, %rdx
    syscall

    movq $60, %rax
    xorq %rdi, %rdi
    syscall

fail:
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

.section .data
msg1:   .asciz "time test start\n"
cmsg:   .asciz "clock-ok\n"
smsg:   .asciz "sleep-ok\n"
amsg:   .asciz "alarm-ok\n"
done:   .asciz "time test PASS\n"
fstg:   .asciz "time fail "
stage:  .byte 0
stbuf:  .byte 0, 0x0a
t0:     .quad 0, 0
t1:     .quad 0, 0
res:    .quad 0, 0
req:    .quad 0, 200000000      /* 0.2s */
