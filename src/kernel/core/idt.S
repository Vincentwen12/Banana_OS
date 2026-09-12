/*
 * idt.S — minimal exception stubs + common handler (W6 hardening).
 *
 * The kernel previously had NO IDT: any fault (user or kernel) became a #DF
 * then a triple fault -> silent reboot. This installs one gate per vector so
 * that any exception prints vector/err/RIP/RSP/CR2 and halts instead.
 *
 * Layout trick: 32 exception thunks packed at 8-byte stride, each just `call
 * exc_common`. The CPU jumps to the gate for vector N, i.e. thunk_base + N*8;
 * the `call` pushes thunk_base + N*8 + 5 as the return address, so the common
 * handler recovers N = (ret_addr - thunk_base - 5) / 8. Vectors 32-255 (masked
 * interrupts) all share the gate at thunk_base + 32*8.
 */
.section .text
.p2align 3
.globl thunk_base

/* Symbols defined elsewhere (syscall_entry.S / syscall.c / idt.c). */
.extern syscall_exit_request
.extern user_ret_rsp
.extern user_ret_rbx
.extern user_ret_rbp
.extern user_ret_r12
.extern user_ret_r13
.extern user_ret_r14
.extern user_ret_r15
.extern exc_kill_user
/* W7 (Task 2.5): signal delivery globals (defined in signal.c). */
.extern syscall_sig_request
.extern syscall_sig_handler
.extern syscall_sig_frame_rsp

thunk_base:
.set i, 0
.rept 32
    call exc_common
    nop
    nop
    nop
    .set i, i + 1
.endr
    call exc_common          /* generic gate for vectors 32-255 */
    nop
    nop
    nop

/*
 * exc_common — stack on entry (top -> bottom):
 *   [ret_addr] [err?] [RIP] [CS] [RFLAGS] [RSP] [SS]
 * The error code is pushed by the CPU only for vectors 8,10,11,12,13,14,17.
 * Build a uniform frame [vec][err][RIP][CS][RFLAGS][RSP][SS] for the C dump.
 */
exc_common:
    cli
    movq (%rsp), %rax           /* return address = thunk_base + N*8 + 5 */
    leaq thunk_base(%rip), %rcx
    subq %rcx, %rax
    subq $5, %rax
    shrq $3, %rax               /* rax = vector N */
    addq $8, %rsp               /* drop the call return address */

    cmpq $8,  %rax;  je  .Lerr
    cmpq $10, %rax;  je  .Lerr
    cmpq $11, %rax;  je  .Lerr
    cmpq $12, %rax;  je  .Lerr
    cmpq $13, %rax;  je  .Lerr
    cmpq $14, %rax;  je  .Lerr
    cmpq $17, %rax;  je  .Lerr
    pushq $0                    /* no error code: push a dummy */
    jmp  .Lframe
.Lerr:
    /* CPU-pushed error code is already at (%rsp). */
.Lframe:
    pushq %rax                  /* [vec][err][RIP][CS][RFLAGS][RSP][SS] */

    /* User-mode fault (CS.RPL == 3): kill the current task and unwind back
     * to the kernel instead of halting the whole system.  The frame sits on
     * the per-CPU TSS.RSP0 stack; it is discarded below when RSP is reloaded
     * from user_ret_rsp (same unwind as the exit syscall path). */
    movq 24(%rsp), %rdx         /* CS */
    andq $3, %rdx
    jnz  .Luser

    /* Kernel-mode fault: print and halt (unchanged behaviour).
     * Push all GPRs first so exc_print_c can dump the faulting register
     * values (frame: [rax..r15][vec][err][RIP][CS][RFLAGS][RSP][SS]). */
    pushq %r15
    pushq %r14
    pushq %r13
    pushq %r12
    pushq %r11
    pushq %r10
    pushq %r9
    pushq %r8
    pushq %rdi
    pushq %rsi
    pushq %rbp
    pushq %rbx
    pushq %rdx
    pushq %rcx
    pushq %rax
    movq %rsp, %rdi
    call exc_print_c
1:  hlt
    jmp 1b

.Luser:
    movq %rax, %rdi             /* rdi = vector */
    movq 16(%rsp), %rsi         /* rsi = faulting user RIP */
    movq 40(%rsp), %rdx         /* rdx = faulting user RSP */
    call exc_kill_user          /* handler 存在时置 syscall_sig_request=1 */

    /* W7: 有信号 handler -> 用 handler/sigframe rsp 覆写 iretq 帧的
       RIP/RSP，iretq 进 handler（原 faulting RIP/RSP 已存进 sigframe）。 */
    cmpl $0, syscall_sig_request(%rip)
    jne  .Luser_signal

    /* 无 handler：复用 exit unwind（.Lreturn_to_kernel）返回内核。 */
    movl $0, syscall_exit_request(%rip)
    movq user_ret_rsp(%rip), %rsp
    movq user_ret_rbx(%rip), %rbx
    movq user_ret_rbp(%rip), %rbp
    movq user_ret_r12(%rip), %r12
    movq user_ret_r13(%rip), %r13
    movq user_ret_r14(%rip), %r14
    movq user_ret_r15(%rip), %r15
    xorl %eax, %eax
    ret

.Luser_signal:
    movl $0, syscall_sig_request(%rip)
    movq syscall_sig_handler(%rip), %rax
    movq %rax, 16(%rsp)          /* iretq RIP = handler */
    movq syscall_sig_frame_rsp(%rip), %rax
    movq %rax, 40(%rsp)          /* iretq RSP = sigframe 起始 */
    movq 32(%rsp), %rax          /* RFLAGS: 确保 IF=1 */
    orq  $0x200, %rax
    movq %rax, 32(%rsp)
    addq $16, %rsp               /* 丢弃 [vec][err] */
    iretq
