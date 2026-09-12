#ifndef SYSCALL_H
#define SYSCALL_H

#include "axion.h"

/* Full user register snapshot (defined in sched.h). */
typedef struct user_regs user_regs_t;

typedef uint64_t (*syscall_handler_t)(uint64_t a1, uint64_t a2, uint64_t a3,
                                       uint64_t a4, uint64_t a5, uint64_t a6);

void     syscall_init(void);
uint64_t syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6);
void     syscall_register(uint64_t nr, syscall_handler_t handler);
void     syscall_restore_msrs(void);   /* W7 3.2: 按 current_task 恢复 FS/GS base MSR */

/* MSR constants for syscall/sysret */
#define IA32_STAR   0xC0000081
#define IA32_LSTAR  0xC0000082
#define IA32_CSTAR  0xC0000083
#define IA32_FMASK  0xC0000084
#define IA32_EFER   0xC0000080

/* Segment selectors live in core/gdt.h (single source of truth for the GDT).
 * syscall/sysret derive CS/SS from IA32_STAR, not from these constants. */

/* syscall entry point (defined in syscall_entry.S). */
void syscall_entry(void);

/* Kernel RSP used on syscall entry, and the saved user RSP (set by assembly). */
extern uint64_t syscall_kernel_top;
extern uint64_t syscall_user_rsp;

/* W6: full user register snapshot at syscall entry (layout = user_regs_t). */
extern uint64_t syscall_user_ctx[16];

/* W7: 首次运行 fork 子进程（syscall_entry.S）。保存调度器返回上下文后
 * 用 fork_ctx 重建 syscall 返回帧 sysretq 进用户态。 */
void fork_resume(user_regs_t* resume_ctx);

/*
 * W6: out-of-band requests raised by a syscall handler and honoured by
 * syscall_entry.S after syscall_dispatch returns.
 *   exit_request: the current user task called exit(); unwind back to the
 *                 kernel dispatch loop (never sysret to the exiting program).
 *   exec_request: the current task called execve(); iretq into the replacement
 *                 image at exec_entry/exec_rsp (never sysret to the old image).
 */
extern volatile int syscall_exit_request;
extern volatile int syscall_exec_request;
extern uint64_t      syscall_exec_entry;
extern uint64_t      syscall_exec_rsp;

/* W6: kernel-return context saved by switch_to_user() (syscall_entry.S
 * restores these when a user task exits). */
extern uint64_t user_ret_rsp;
extern uint64_t user_ret_rbx;
extern uint64_t user_ret_rbp;
extern uint64_t user_ret_r12;
extern uint64_t user_ret_r13;
extern uint64_t user_ret_r14;
extern uint64_t user_ret_r15;

/* Max syscall number — must cover openat(257)/newfstatat(262)/getrandom(318).
 * 512 entries * 8B = 4KB static table. */
#define MAX_SYSCALLS 512

#endif