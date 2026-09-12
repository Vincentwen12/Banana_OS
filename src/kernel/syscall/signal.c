/* signal.c — W7 信号完整性（Task 2.5）。
 *
 * 实现 rt_sigaction(13) / rt_sigprocmask(14) / rt_sigreturn(15) / kill(62)
 * 扩展，以及信号投递/恢复机制：
 *   - 正常 syscall 返回路径：syscall_entry.S 在 call syscall_dispatch 后
 *     立即 call signal_check_deliver()（无参无返回），检查 current_task 的
 *     sig_pending & ~sig_blocked，取最低位信号，把 sigframe 写到用户栈并
 *     置 syscall_sig_request/handler/frame_rsp 三个全局；asm 随后 sysretq
 *     到 handler。
 *   - 异常路径：idt.c 的 exc_kill_user() 调用 signal_deliver_now()，按
 *     上述方式构建 sigframe（regs 来自 fault 帧），idt.S 检查
 *     syscall_sig_request 后用 handler/sigframe rsp 覆写 iretq 帧并 iretq。
 *   - 恢复路径：handler 通过 restorer（内核桩或用户 SA_RESTORER）执行
 *     rt_sigreturn，从用户栈 sigframe 恢复 rip/rflags/rsp，置
 *     syscall_sig_restore_request/rip/rflags，asm 检查后 sysretq 恢复。
 */
#include "axion.h"
#include "sched.h"
#include "syscall.h"
#include "mm/vm.h"
#include "printk.h"

/* ---- 信号常量 ---- */
#define SIG_KILL   9
#define SIG_STOP  19
#define SIG_CHLD  17
#define SA_RESTORER   0x04000000
#define SIG_IGN_VAL   1             /* SIG_IGN (Linux) */
#define SIG_RESTORER_ADDR 0x600000000000ULL   /* 内核 restorer 桩固定地址（用户区高位，避开 512GB 栈/ld-linux 区） */

/* ---- 信号投递/恢复全局（syscall_entry.S / idt.S 通过 extern 引用）---- */
volatile int syscall_sig_request = 0;
uint64_t     syscall_sig_handler = 0;
uint64_t     syscall_sig_frame_rsp = 0;
uint64_t     syscall_sig_frame_ptr = 0;   /* sigframe 起始（恢复时用） */
volatile int syscall_sig_restore_request = 0;
uint64_t     syscall_sig_restore_rip = 0;
uint64_t     syscall_sig_restore_rflags = 0;
/* 1 = 一个 handler 正在执行（投递后未 sigreturn）。防止 handler 自身再次
 * 触发异常时被无限重投递同信号（bash termsig 入口崩 → SIGSEGV 死循环）。 */
volatile int syscall_sig_active = 0;

/* 任务退出/槽复用时清零全部投递/恢复全局（防止跨任务残留：子进程 arm 的
 * 请求在父进程 sysretq 时被误消费）。sched.c 在 mark_exited/kill/spawn 调用。 */
void signal_globals_reset(void)
{
    syscall_sig_request = 0;
    syscall_sig_handler = 0;
    syscall_sig_frame_rsp = 0;
    syscall_sig_frame_ptr = 0;
    syscall_sig_restore_request = 0;
    syscall_sig_restore_rip = 0;
    syscall_sig_restore_rflags = 0;
    syscall_sig_active = 0;
}

/* 内核 restorer 桩页（signal_init 填充：mov $15,%eax; syscall）。
 * vm_create() 将其映射到 0x7FFFFFF00000（VM_USER|READ|WRITE|EXEC）。 */
uint8_t* sig_stub_page = 0;

/* sigframe 布局（用户栈，共 144B）：
 *   [0]  restorer 返回地址
 *   [8]  signum
 *   [16] user_regs_t 全量 128B
 */
#define SIGFRAME_SIZE 144

/* 取最低位置位的信号号（1..63），无则返回 0 */
static int lowest_sig(uint64_t mask)
{
    for (int i = 1; i < 64; i++)
        if (mask & (1ULL << i)) return i;
    return 0;
}

/* 构建 sigframe 并置投递全局。regs = 信号发生时的用户上下文快照，
 * user_sp = 信号发生时的用户栈（用于计算 sigframe 位置）。 */
static void signal_setup_delivery(task_t* t, int sig, const user_regs_t* regs,
                                  uint64_t user_sp)
{
    uint64_t restorer = SIG_RESTORER_ADDR;
    if ((t->sig_flags[sig] & SA_RESTORER) && t->sig_restorer[sig])
        restorer = t->sig_restorer[sig];

    uint64_t frame[18];                       /* 144B / 8 */
    frame[0] = restorer;
    frame[1] = (uint64_t)sig;
    for (int i = 0; i < 16; i++)
        frame[2 + i] = ((uint64_t*)regs)[i];

    uint64_t frsp = (user_sp - SIGFRAME_SIZE) & ~0xFULL;
    /* W7 (Task 4.6): SA_ONSTACK 且已配置备用栈 → 在 altstack 上建 sigframe */
    if ((t->sig_flags[sig] & 0x08000000) && t->altstack_size > 0 &&
        !t->altstack_onstack) {
        frsp = (t->altstack_base + t->altstack_size - SIGFRAME_SIZE) & ~0xFULL;
        t->altstack_onstack = 1;
    }
    if (!t->mm_context) return;
    if (vm_copy_to_user(t->mm_context, frsp, frame, SIGFRAME_SIZE) < 0)
        return;                               /* 用户栈不可写：放弃投递 */

    syscall_sig_handler = t->sig_handler[sig];
    syscall_sig_frame_rsp = frsp;
    syscall_sig_frame_ptr = frsp;
    syscall_sig_request = 1;
    syscall_sig_active = 1;   /* handler 即将运行：不再嵌套投递 */
    t->sig_pending &= ~(1ULL << sig);
}

/* 异常路径信号投递（idt.c exc_kill_user 调用）。有 handler 且未阻塞才投递；
 * 返回 1 表示已投递（syscall_sig_request=1），0 表示未投递（调用者维持
 * 原杀进程逻辑）。 */
int signal_deliver_now(int sig, uint64_t fault_rip, uint64_t fault_rsp)
{
    task_t* t = current_task;
    if (!t || sig < 1 || sig >= 64) return 0;
    if (syscall_sig_active) return 0;   /* handler 执行中：不嵌套投递 */
    if (t->sig_handler[sig] <= SIG_IGN_VAL) return 0;   /* 默认/忽略 */
    if (t->sig_blocked & (1ULL << sig)) return 0;       /* 阻塞：不投递 */

    user_regs_t regs;
    for (int i = 0; i < 16; i++) ((uint64_t*)&regs)[i] = syscall_user_ctx[i];
    regs.rip = fault_rip;
    regs.rsp = fault_rsp;
    t->sig_pending |= (1ULL << sig);
    signal_setup_delivery(t, sig, &regs, fault_rsp);
    return 1;
}

/* syscall 返回路径投递检查（syscall_entry.S 在 call syscall_dispatch 后
 * 立即 call，无参无返回）。对 current_task 检查 sig_pending & ~sig_blocked，
 * 取最低位信号，从 syscall_user_ctx/syscall_user_rsp 快照构建 sigframe。 */
void signal_check_deliver(void)
{
    task_t* t = current_task;
    if (!t) return;
    if (syscall_sig_active) return;   /* handler 执行中：信号保持 pending */
    uint64_t mask = t->sig_pending & ~t->sig_blocked;
    if (!mask) return;
    int sig = lowest_sig(mask);
    if (!sig) return;
    if (t->sig_handler[sig] <= SIG_IGN_VAL) return;

    user_regs_t regs;
    for (int i = 0; i < 16; i++) ((uint64_t*)&regs)[i] = syscall_user_ctx[i];
    signal_setup_delivery(t, sig, &regs, syscall_user_rsp);
}

/* ---- 13: rt_sigaction ---- */
uint64_t sys_rt_sigaction(uint64_t sig, uint64_t act, uint64_t oldact,
                          uint64_t sigsetsize, uint64_t a5, uint64_t a6)
{
    (void)sigsetsize; (void)a5; (void)a6;
    task_t* t = current_task;
    if (!t || !t->mm_context) return (uint64_t)(-14);   /* EFAULT */
    if (sig == 0 || sig >= 64) return (uint64_t)(-22);  /* EINVAL */
    if (sig == SIG_KILL || sig == SIG_STOP)
        return (uint64_t)(-22);                         /* 不可捕获 */

    /* Linux x86-64 struct sigaction 布局:
     *   +0  handler (8)   +8  mask (8)   +16 flags (4)  +24 restorer (8)  */
    if (oldact) {
        uint64_t old[4];
        old[0] = t->sig_handler[sig];
        old[1] = 0;                 /* mask（本内核未按信号保存 mask） */
        old[2] = t->sig_flags[sig];
        old[3] = t->sig_restorer[sig];
        if (vm_copy_to_user(t->mm_context, oldact, old, 32) < 0)
            return (uint64_t)(-14);
    }
    if (act) {
        uint64_t newact[4];
        if (vm_copy_from_user(t->mm_context, newact, act, 32) < 0)
            return (uint64_t)(-14);
        t->sig_handler[sig]  = newact[0];
        t->sig_flags[sig]    = newact[2];
        t->sig_restorer[sig] = newact[3];
    }
    return 0;
}

/* ---- 14: rt_sigprocmask ---- */
uint64_t sys_rt_sigprocmask(uint64_t how, uint64_t set, uint64_t oldset,
                            uint64_t sigsetsize, uint64_t a5, uint64_t a6)
{
    (void)sigsetsize; (void)a5; (void)a6;
    task_t* t = current_task;
    if (!t || !t->mm_context) return (uint64_t)(-14);   /* EFAULT */
    if (oldset) {
        uint64_t m = t->sig_blocked;
        if (vm_copy_to_user(t->mm_context, oldset, &m, 8) < 0)
            return (uint64_t)(-14);
    }
    if (set) {
        uint64_t m = 0;
        if (vm_copy_from_user(t->mm_context, &m, set, 8) < 0)
            return (uint64_t)(-14);
        if (how == 0)      t->sig_blocked |= m;         /* SIG_BLOCK */
        else if (how == 1) t->sig_blocked &= ~m;        /* SIG_UNBLOCK */
        else               t->sig_blocked = m;          /* SIG_SETMASK */
        t->sig_blocked &= ~((1ULL << SIG_KILL) | (1ULL << SIG_STOP));
    }
    return 0;
}

/* ---- 15: rt_sigreturn ---- */
uint64_t sys_rt_sigreturn(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    task_t* t = current_task;
    if (!t || !t->mm_context) return (uint64_t)(-14);   /* EFAULT */

    /* handler 通过 restorer 进入 rt_sigreturn 时，handler 的 ret 已弹出
     * sigframe[0]，故 rsp == frame+8，frame 起始 = syscall_user_rsp - 8。 */
    uint64_t fr = syscall_user_rsp - 8;
    uint64_t signum = 0;
    user_regs_t regs;
    if (vm_copy_from_user(t->mm_context, &signum, fr + 8, 8) < 0)
        return (uint64_t)(-14);
    if (vm_copy_from_user(t->mm_context, &regs, fr + 16,
                          sizeof(user_regs_t)) < 0)
        return (uint64_t)(-14);

    syscall_user_rsp = regs.rsp;
    syscall_sig_frame_ptr = fr;              /* sigframe 起始 */
    syscall_sig_restore_rip = regs.rip;
    syscall_sig_restore_rflags = regs.rflags;
    syscall_sig_restore_request = 1;
    syscall_sig_active = 0;                  /* handler 结束 */
    /* 从 altstack 返回：清除 onstack 标记（Linux 语义） */
    if (t->altstack_size > 0 && regs.rsp >= t->altstack_base &&
        regs.rsp < t->altstack_base + t->altstack_size)
        t->altstack_onstack = 0;
    if (signum > 0 && signum < 64)
        t->sig_pending &= ~(1ULL << signum);
    return 0;
}

/* ---- 62: sys_kill ---- */

/* 向单个任务投递信号（最小语义）。
 * 有 handler 且未阻塞 -> 置 sig_pending（必要时 sched_wake 唤醒阻塞的
 * 内核任务）；无 handler -> SIGKILL 及终止类走 sched_kill/退出，
 * SIGCHLD 默认忽略。 */
static void kill_one_task(task_t* t, int sig)
{
    if (!t || sig <= 0 || sig >= 64) return;

    if (sig == SIG_KILL) {
        if (t == current_task) {
            t->exit_code = sig;
            sched_mark_exited(t, sig);
            syscall_exit_request = 1;
        } else {
            sched_kill((int)t->id);
        }
        return;
    }
    if (sig == SIG_STOP) return;   /* 无 stop 机制：忽略 */

    uint64_t h = t->sig_handler[sig];
    if (h > SIG_IGN_VAL && !(t->sig_blocked & (1ULL << sig))) {
        t->sig_pending |= (1ULL << sig);
        if (t->state == TASK_STATE_BLOCKED && !t->is_user)
            sched_wake(t);         /* 唤醒阻塞的内核任务 */
        return;
    }
    if (h == SIG_IGN_VAL) return;  /* SIG_IGN：忽略 */

    if (sig == SIG_CHLD) return;   /* SIGCHLD 默认忽略 */

    /* 本内核无 stop/continue 机制：SIGSTOP/SIGCONT/SIGTSTP/SIGTTIN/SIGTTOU
     * 默认动作按忽略处理。bash 启动时会向自己发 SIGTTIN 自测（后台 shell
     * 语义），若不忽略会误杀 bash。 */
    if (sig == 18 || sig == 19 || sig == 20 || sig == 21 || sig == 22)
        return;

    /* 终止类默认动作 */
    if (t == current_task) {
        t->exit_code = sig;
        sched_mark_exited(t, sig);
        syscall_exit_request = 1;
    } else {
        sched_kill((int)t->id);
    }
}

uint64_t sys_kill(uint64_t pid, uint64_t sig, uint64_t a3, uint64_t a4,
                  uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (sig >= 64) return (uint64_t)(-22);   /* EINVAL */
    int64_t spid = (int64_t)pid;
    int sent = 0;

    if (spid > 0) {
        task_t* t = (current_task && spid == (int64_t)current_task->pid)
                    ? current_task : sched_get_task((int)pid);
        if (!t) return (uint64_t)(-3);       /* ESRCH */
        kill_one_task(t, (int)sig);
        return 0;
    }
    if (spid == 0) {                         /* 同进程组：最小语义 = 自己 */
        if (current_task) kill_one_task(current_task, (int)sig);
        return 0;
    }

    /* pid < 0：进程组投递（最小语义）。pid==-1 投递全部用户进程；
     * pid<-1 投递给 pid==-pid（组长）及其直接子进程。 */
    int64_t pgid = -spid;
    for (int i = 0; i < sched_task_count(); i++) {
        task_t* t = sched_get_task(i);
        if (!t) continue;
        if (pgid == 1) {
            kill_one_task(t, (int)sig);
            sent++;
        } else if ((int64_t)t->pid == pgid || (int64_t)t->ppid == pgid) {
            kill_one_task(t, (int)sig);
            sent++;
        }
    }
    return sent ? 0 : (uint64_t)(-3);        /* ESRCH */
}

/* W7 (Task 4.6): 给指定任务投递信号（sched.c ITIMER_REAL 轮询用）。 */
void signal_deliver_to_task(task_t* t, int sig)
{
    kill_one_task(t, sig);
}

/* ---- 内核 restorer 桩 ---- */
void signal_init(void)
{
    sig_stub_page = (uint8_t*)vm_alloc_page();
    if (!sig_stub_page) return;
    /* mov $15, %eax ; syscall  （7 字节） */
    sig_stub_page[0] = 0xB8;
    sig_stub_page[1] = 0x0F; sig_stub_page[2] = 0x00;
    sig_stub_page[3] = 0x00; sig_stub_page[4] = 0x00;
    sig_stub_page[5] = 0x0F;
    sig_stub_page[6] = 0x05;
}
