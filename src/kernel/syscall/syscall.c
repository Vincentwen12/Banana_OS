#include "syscall.h"
#include "vga.h"
#include "port.h"
#include "printk.h"
#include "sched.h"

#define SERIAL_PORT 0x3F8

static syscall_handler_t syscall_table[MAX_SYSCALLS];

/* Dedicated kernel stack used while servicing a user `syscall`. */
static uint8_t ksyscall_stack[16 * 1024] __attribute__((aligned(16)));

uint64_t syscall_kernel_top = 0;   /* top of ksyscall_stack (set in syscall_init) */
uint64_t syscall_user_rsp   = 0;   /* written by syscall_entry.S on entry */

/* W6: full user register snapshot taken at syscall entry. Layout matches
 * user_regs_t in sched.h (rax rdi rsi rdx r10 r8 r9 rflags rip rbx rbp
 * r12 r13 r14 r15 rsp). Filled by syscall_entry.S; consumed by fork(). */
uint64_t syscall_user_ctx[16];

/* W6: exit/exec out-of-band requests (honoured by syscall_entry.S). */
volatile int syscall_exit_request = 0;
volatile int syscall_exec_request = 0;
uint64_t      syscall_exec_entry   = 0;
uint64_t      syscall_exec_rsp     = 0;

/* W6: kernel context saved by switch_to_user() before entering Ring 3. */
uint64_t user_ret_rsp = 0;
uint64_t user_ret_rbx = 0;
uint64_t user_ret_rbp = 0;
uint64_t user_ret_r12 = 0;
uint64_t user_ret_r13 = 0;
uint64_t user_ret_r14 = 0;
uint64_t user_ret_r15 = 0;

void syscall_init(void)
{
    /* Zero out the syscall table */
    for (int i = 0; i < MAX_SYSCALLS; i++) {
        syscall_table[i] = (syscall_handler_t)0;
    }

    /* Point the syscall entry at our dedicated kernel stack. */
    syscall_kernel_top = (uint64_t)(uintptr_t)(ksyscall_stack + sizeof(ksyscall_stack));

    /*
     * Enable syscall/sysret (Ring 3 <-> Ring 0):
     *
     *   IA32_EFER.SCE (bit 0) — enables the syscall/sysret instructions.
     *   IA32_STAR — [47:32] kernel CS (0x08), [63:48] sysret base (0x10).
     *       syscall loads CS=STAR[47:32]=0x08, SS=STAR[47:32]+8=0x10.
     *       sysretq loads CS=(STAR[63:48]+16)|3=0x23, SS=(STAR[63:48]+8)|3=0x1B.
     *   IA32_LSTAR — RIP loaded on syscall.
     *   IA32_FMASK — RFLAGS bits cleared on syscall entry (TF|IF|DF).
     */
    uint64_t efer = rdmsr(IA32_EFER);
    efer |= 0x1;    /* SCE: enable syscall/sysret */
    efer |= 0x800;  /* NXE: enable NX so non-exec user pages (stack, data)
                       can carry bit 63; without it bit 63 is reserved and
                       any such PTE triggers a reserved-bit #PF */
    wrmsr(IA32_EFER, efer);

    wrmsr(IA32_STAR,  (0x10ULL << 48) | (0x08ULL << 32));
    wrmsr(IA32_LSTAR, (uint64_t)(uintptr_t)syscall_entry);
    wrmsr(IA32_FMASK, (1ULL << 8) | (1ULL << 9) | (1ULL << 10));
}

void syscall_register(uint64_t nr, syscall_handler_t handler)
{
    if (nr < MAX_SYSCALLS) {
        syscall_table[nr] = handler;
    }
}

uint64_t syscall_dispatch(uint64_t nr, uint64_t a1, uint64_t a2,
                          uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    if (nr >= MAX_SYSCALLS || !syscall_table[nr])
        return (uint64_t)(-38);  /* ENOSYS */
    return syscall_table[nr](a1, a2, a3, a4, a5, a6);
}

/* W7 Phase 3.2: MSR_FS_BASE/GS_BASE 是 per-CPU 全局寄存器。任务在阻塞期间
 * （wait4 等）其他任务可能用 arch_prctl 覆盖它们，返回用户态前必须按
 * current_task 恢复，否则 bash 主进程读 %fs:0x28（栈金丝雀）会落到子进程
 * TLS 地址触发 #PF。syscall_entry.S 在 sysretq 前调用本函数。 */
#define MSR_FS_BASE 0xC0000100
#define MSR_GS_BASE 0xC0000101
void syscall_restore_msrs(void)
{
    if (!current_task) return;
    wrmsr(MSR_FS_BASE, current_task->fs_base);
    wrmsr(MSR_GS_BASE, current_task->gs_base);
}