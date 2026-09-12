/*
 * syscall_entry.S — x86-64 syscall/sysret entry and Ring 3 transition.
 *
 * Ring 3 user programs issue `syscall`, which:
 *   1. Saves RIP -> RCX, RFLAGS -> R11
 *   2. Loads CS/SS from IA32_STAR (kernel selectors)
 *   3. Loads RIP from IA32_LSTAR
 *   4. Clears RFLAGS according to IA32_FMASK
 *
 * The CPU does NOT switch RSP, so syscall_entry switches to a dedicated
 * kernel stack itself and saves the user RSP in syscall_user_rsp.
 */

.section .text

/* Kernel stack top (set by syscall_init in syscall.c). */
.extern syscall_kernel_top
/* Saved user RSP across a syscall (written here, read on sysret). */
.extern syscall_user_rsp

/* W6: exit/exec request flags and saved kernel-context globals. */
.extern syscall_exit_request
.extern syscall_exec_request
.extern syscall_exec_entry
.extern syscall_exec_rsp
.extern user_ret_rsp
.extern user_ret_rbx
.extern user_ret_rbp
.extern user_ret_r12
.extern user_ret_r13
.extern user_ret_r14
.extern user_ret_r15
/* W6: full user register snapshot (fork) + saved outer kernel context. */
.extern syscall_user_ctx
/* W7: 调度器上下文（sched.c 定义），任务阻塞时 switch_context 返回主循环
 * 的返回点。布局 = kctx_t：rsp0 rbx8 rbp16 r12 24 r13 32 r14 40 r15 48 valid56。 */
.extern g_sched_kctx

/* W7 (Task 2.5): signal delivery / restore requests (defined in signal.c). */
.extern syscall_sig_request
.extern syscall_sig_handler
.extern syscall_sig_frame_rsp
.extern syscall_sig_frame_ptr
.extern syscall_sig_restore_request
.extern syscall_sig_restore_rip
.extern syscall_sig_restore_rflags
.extern signal_check_deliver

.global syscall_entry
.type syscall_entry, @function
syscall_entry:
    /* RCX = user RIP, R11 = user RFLAGS, RSP = user RSP (unchanged). */
    movq %rsp, syscall_user_rsp(%rip)
    movq %rsp, syscall_user_ctx+120(%rip)  /* user rsp */
    movq syscall_kernel_top(%rip), %rsp

    /* W6: snapshot the full user context for fork().
       Layout matches user_regs_t (sched.h):
       rax0 rdi8 rsi16 rdx24 r10 32 r8 40 r9 48 rflags56 rip64
       rbx72 rbp80 r12 88 r13 96 r14 104 r15 112 rsp120 */
    movq %rax, syscall_user_ctx+0(%rip)
    movq %rdi, syscall_user_ctx+8(%rip)
    movq %rsi, syscall_user_ctx+16(%rip)
    movq %rdx, syscall_user_ctx+24(%rip)
    movq %r10, syscall_user_ctx+32(%rip)
    movq %r8,  syscall_user_ctx+40(%rip)
    movq %r9,  syscall_user_ctx+48(%rip)
    movq %r11, syscall_user_ctx+56(%rip)
    movq %rcx, syscall_user_ctx+64(%rip)
    movq %rbx, syscall_user_ctx+72(%rip)
    movq %rbp, syscall_user_ctx+80(%rip)
    movq %r12, syscall_user_ctx+88(%rip)
    movq %r13, syscall_user_ctx+96(%rip)
    movq %r14, syscall_user_ctx+104(%rip)
    movq %r15, syscall_user_ctx+112(%rip)

    /* Save user context:
       [rsp+0x00] rax (nr)       [rsp+0x08] rdi (a1)
       [rsp+0x10] rsi (a2)       [rsp+0x18] rdx (a3)
       [rsp+0x20] r10 (a4)       [rsp+0x28] r8  (a5)
       [rsp+0x30] r9  (a6)       [rsp+0x38] r11 (rflags)
       [rsp+0x40] rcx (rip) */
    pushq %rcx
    pushq %r11
    pushq %r9
    pushq %r8
    pushq %r10
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rax

    /* Map Linux ABI -> dispatch ABI:
       dispatch(nr=rdi, a1=rsi, a2=rdx, a3=rcx, a4=r8, a5=r9, a6=[stack]) */
    movq 0x00(%rsp), %rdi   /* nr = user rax */
    movq 0x08(%rsp), %rsi   /* a1 = user rdi */
    movq 0x10(%rsp), %rdx   /* a2 = user rsi */
    movq 0x18(%rsp), %rcx   /* a3 = user rdx */
    movq 0x20(%rsp), %r8    /* a4 = user r10 */
    movq 0x28(%rsp), %r9    /* a5 = user r8 */
    pushq 0x30(%rsp)        /* a6 = user r9 (7th arg on stack) */

    call syscall_dispatch

    /* W7: 把返回值写回 syscall_user_ctx.rax（信号投递时 sigframe 需要
       保存"返回点"的用户 rax = 返回值，而非入口的系统调用号）。
       注意：不能用 rbx 暂存返回值——那会污染用户 rbx（glibc 把函数指针
       放在 rbx 跨 syscall，被覆盖后 call *%rbx 跳到堆地址触发 NX#PF）。
       用栈保存。 */
    movq %rax, syscall_user_ctx+0(%rip)
    pushq %rax
    call signal_check_deliver      /* 有可投递信号时置 sig_request 全局 */
    popq %rax

    /* W7 3.2: sysretq 前按 current_task 恢复 FS/GS base MSR。阻塞型 syscall
       （wait4）期间子进程可能用 arch_prctl 覆盖全局 MSR，不恢复则父进程
       %fs 金丝雀读错地址 #PF。此点覆盖普通返回/信号投递/信号恢复三条路径。 */
    pushq %rax
    call syscall_restore_msrs
    popq %rax

    cmpl $0, syscall_exit_request(%rip)
    jne  .Lreturn_to_kernel
    cmpl $0, syscall_exec_request(%rip)
    jne  .Ljump_to_user
    cmpl $0, syscall_sig_restore_request(%rip)
    jne  .Lsignal_restore
    cmpl $0, syscall_sig_request(%rip)
    jne  .Lsignal_deliver

    addq $8, %rsp           /* drop a6 */
    addq $8, %rsp           /* skip saved rax (keep return value in %rax) */

    popq %rdi
    popq %rsi
    popq %rdx
    popq %r10
    popq %r8
    popq %r9
    popq %r11               /* user RFLAGS */
    popq %rcx               /* user RIP */

    movq syscall_user_rsp(%rip), %rsp
    sysretq

/* W7: 投递信号 — sysretq 到 handler。从 sigframe 恢复用户寄存器
 * （frame[16..143] = user_regs_t 全量），RDI = signum（handler 第一参数，
 * Linux 约定），RIP = handler 入口，RSP 指向 frame 起始（frame[0]=restorer
 * 返回地址，handler 的 ret 会跳到 restorer 执行 rt_sigreturn）。 */
.Lsignal_deliver:
    movl $0, syscall_sig_request(%rip)
    movq syscall_sig_frame_rsp(%rip), %rsp   /* rsp = frame 起始 */
    movq 16+0(%rsp), %rax
    movq 8(%rsp), %rdi            /* rdi = signum */
    movq 16+16(%rsp), %rsi
    movq 16+24(%rsp), %rdx
    movq 16+32(%rsp), %r10
    movq 16+40(%rsp), %r8
    movq 16+48(%rsp), %r9
    movq 16+72(%rsp), %rbx
    movq 16+80(%rsp), %rbp
    movq 16+88(%rsp), %r12
    movq 16+96(%rsp), %r13
    movq 16+104(%rsp), %r14
    movq 16+112(%rsp), %r15
    movq 16+56(%rsp), %r11        /* user RFLAGS -> sysretq RFLAGS */
    movq syscall_sig_handler(%rip), %rcx   /* handler 入口 -> sysretq RIP */
    sysretq

/* W7: 从 sigframe 恢复被信号中断的完整上下文（rt_sigreturn 请求）。
 * 恢复全部 16 个用户寄存器；RSP 最后装载为用户栈。 */
.Lsignal_restore:
    movl $0, syscall_sig_restore_request(%rip)
    movq syscall_sig_frame_ptr(%rip), %rsp   /* rsp = frame 起始 */
    movq 16+64(%rsp), %rcx        /* user RIP */
    movq 16+56(%rsp), %r11        /* user RFLAGS */
    movq 16+0(%rsp), %rax
    movq 16+8(%rsp), %rdi
    movq 16+16(%rsp), %rsi
    movq 16+24(%rsp), %rdx
    movq 16+32(%rsp), %r10
    movq 16+40(%rsp), %r8
    movq 16+48(%rsp), %r9
    movq 16+72(%rsp), %rbx
    movq 16+80(%rsp), %rbp
    movq 16+88(%rsp), %r12
    movq 16+96(%rsp), %r13
    movq 16+104(%rsp), %r14
    movq 16+112(%rsp), %r15
    movq 16+120(%rsp), %rsp       /* user RSP（最后装载） */
    sysretq

/* The current task called exit(): abandon the syscall frame and return to the
 * kernel caller of user_run(), restoring the callee-saved kernel context. */
.Lreturn_to_kernel:
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

/* The current task called execve(): iretq into the replacement image without
 * disturbing the kernel-return context saved by the original switch_to_user. */
.Ljump_to_user:
    movl $0, syscall_exec_request(%rip)
    movq syscall_exec_rsp(%rip), %rsi
    movq syscall_exec_entry(%rip), %rdi
    call enter_user
.size syscall_entry, . - syscall_entry

/*
 * switch_to_user(rdi=entry, rsi=user_rsp) — iretq into Ring 3.  Never returns;
 * the only way back is a `syscall` from user space.
 */
.global switch_to_user
.type switch_to_user, @function
switch_to_user:
    movq %rsp, user_ret_rsp(%rip)
    movq %rbx, user_ret_rbx(%rip)
    movq %rbp, user_ret_rbp(%rip)
    movq %r12, user_ret_r12(%rip)
    movq %r13, user_ret_r13(%rip)
    movq %r14, user_ret_r14(%rip)
    movq %r15, user_ret_r15(%rip)

/* enter_user(rdi=entry, rsi=user_rsp) — build the iretq frame and drop into
 * Ring 3.  Never returns; the only way back is a `syscall` from user space. */
.global enter_user
.type enter_user, @function
enter_user:
    pushq $0x1B             /* SS: user data (0x18 | RPL3) */
    pushq %rsi              /* RSP */
    pushq $0x202            /* RFLAGS: IF=1, bit1=1 */
    pushq $0x23             /* CS: user code (0x20 | RPL3) */
    pushq %rdi              /* RIP: entry */
    iretq
.size switch_to_user, . - switch_to_user

/* W7: 协作式调度 — switch_context(from, to)：保存调用者内核上下文到
 * from（rsp = 调用点栈指针，[rsp] = 返回地址），加载 to 并 ret 继续。
 * 仅供 C 调用，调用者可见寄存器（rax/rcx/rdx/rsi/rdi/r8-r11）可被改写。 */
.global switch_context
.type switch_context, @function
switch_context:
    movq %rsp, 0x00(%rdi)       /* from->rsp */
    movq %rbx, 0x08(%rdi)
    movq %rbp, 0x10(%rdi)
    movq %r12, 0x18(%rdi)
    movq %r13, 0x20(%rdi)
    movq %r14, 0x28(%rdi)
    movq %r15, 0x30(%rdi)
    movq $1,   0x38(%rdi)       /* from->valid */
    movq 0x00(%rsi), %rsp
    movq 0x08(%rsi), %rbx
    movq 0x10(%rsi), %rbp
    movq 0x18(%rsi), %r12
    movq 0x20(%rsi), %r13
    movq 0x28(%rsi), %r14
    movq 0x30(%rsi), %r15
    ret
.size switch_context, . - switch_context

/* 保存"当前内核上下文"到 user_ret_*（退出返回点）与 g_sched_kctx（阻塞
 * 返回点）。二者都指向调用点之后的返回地址，故任务阻塞或退出都回到主循环
 * 同一位置，由 C 侧根据 task->state 区分。 */
.macro SAVE_SCHED_RET
    movq %rsp, user_ret_rsp(%rip)
    movq %rbx, user_ret_rbx(%rip)
    movq %rbp, user_ret_rbp(%rip)
    movq %r12, user_ret_r12(%rip)
    movq %r13, user_ret_r13(%rip)
    movq %r14, user_ret_r14(%rip)
    movq %r15, user_ret_r15(%rip)
    movq %rsp, g_sched_kctx(%rip)
    movq %rbx, g_sched_kctx+8(%rip)
    movq %rbp, g_sched_kctx+16(%rip)
    movq %r12, g_sched_kctx+24(%rip)
    movq %r13, g_sched_kctx+32(%rip)
    movq %r14, g_sched_kctx+40(%rip)
    movq %r15, g_sched_kctx+48(%rip)
    movq $1,    g_sched_kctx+56(%rip)
.endm

/* sched_enter_user(rdi=entry, rsi=user_rsp) — 调度器首次启动用户任务：
 * 保存调度器返回上下文后 iretq 进 Ring 3。任务退出经 .Lreturn_to_kernel
 * 返回调用点；阻塞经 switch_context 返回调用点。 */
.global sched_enter_user
.type sched_enter_user, @function
sched_enter_user:
    SAVE_SCHED_RET
    jmp enter_user
.size sched_enter_user, . - sched_enter_user

/* fork_resume(rdi = user_regs_t*) — 首次运行 fork 子进程：保存调度器返回
 * 上下文后，用 fork_ctx 重建 syscall 返回帧（RAX=0）sysretq 进用户态。 */
.global fork_resume
.type fork_resume, @function
fork_resume:
    SAVE_SCHED_RET

    /* 构建子进程的 syscall-return 帧（同 syscall_entry 布局）：
       [rsp+0x00] rax (nr)  ... [rsp+0x40] rcx(rip) [rsp+0x38] r11(rflags) */
    movq %rdi, %r12              /* r12 = ctx 指针 */
    movq 64(%r12), %rcx          /* user rip */
    movq 56(%r12), %r11          /* user rflags */
    pushq %rcx
    pushq %r11
    pushq 48(%r12)               /* r9  */
    pushq 40(%r12)               /* r8  */
    pushq 32(%r12)               /* r10 */
    pushq 24(%r12)               /* rdx */
    pushq 16(%r12)               /* rsi */
    pushq 8(%r12)                /* rdi */
    pushq $0                     /* rax = 0 (fork 子进程返回 0) */

    /* 恢复子进程的用户 callee-saved 寄存器。 */
    movq 120(%r12), %rax
    movq %rax, syscall_user_rsp(%rip)
    movq 72(%r12), %rbx
    movq 80(%r12), %rbp
    movq 96(%r12), %r13
    movq 104(%r12), %r14
    movq 112(%r12), %r15
    movq 88(%r12), %r12          /* child 的 r12（ctx 最后一次使用） */

    xorl %eax, %eax              /* fork() 子进程返回 0 */
    addq $8, %rsp                /* 跳过 saved rax */
    popq %rdi
    popq %rsi
    popq %rdx
    popq %r10
    popq %r8
    popq %r9
    popq %r11
    popq %rcx
    movq syscall_user_rsp(%rip), %rsp
    sysretq
.size fork_resume, . - fork_resume

/*
 * syscall_trampoline — Ring 0 syscall bridge for W5 user tasks.
 *
 * The syscall instruction is illegal in Ring 0 (#UD). During W5, user
 * programs run in Ring 0 and call this trampoline instead of syscall.
 * Fixed at address 0x10a000 by linker.ld for easy ELF code generation.
 *
 * Calling convention (same as Linux syscall ABI):
 *   rax = syscall number
 *   rdi = arg1, rsi = arg2, rdx = arg3
 *   r10 = arg4, r8 = arg5, r9 = arg6
 * Returns: rax = result
 */
.section .trampoline, "ax"
.global syscall_trampoline
.type syscall_trampoline, @function
syscall_trampoline:
    /*
     * Remap from Linux syscall ABI to syscall_dispatch ABI:
     *   Linux: rax=nr, rdi=a1, rsi=a2, rdx=a3, r10=a4, r8=a5, r9=a6
     *   dispatch: rdi=nr, rsi=a1, rdx=a2, rcx=a3, r8=a4, r9=a5, [rsp]=a6
     *
     * Strategy: use register-to-register shuffle (no stack offsets).
     * Only rcx and r11 are caller-saved and need preservation.
     */
    pushq %rcx
    pushq %r11

    /* Push a6 as 7th argument on stack */
    pushq %r9

    /* Shuffle registers to match dispatch ABI */
    movq %rax, %r11        /* r11 = nr */
    movq %rdi, %rax        /* rax = a1 (temp) */
    movq %rsi, %rdi        /* rdi = a2 (temp) */
    movq %rdx, %rsi        /* rsi = a3 (temp) */
    movq %r8,  %r9         /* r9  = a5 */
    movq %r10, %r8         /* r8  = a4 */
    movq %rsi, %rcx        /* rcx = a3 */
    movq %rdi, %rdx        /* rdx = a2 */
    movq %rax, %rsi        /* rsi = a1 */
    movq %r11, %rdi        /* rdi = nr */

    call syscall_dispatch

    addq $8, %rsp          /* pop a6 */

    popq %r11
    popq %rcx
    ret
.size syscall_trampoline, . - syscall_trampoline