#ifndef SCHED_H
#define SCHED_H

#include "axion.h"

/* Forward declaration (mm/vm.h defines the full struct). */
struct vm_context;
typedef struct vm_context vm_context_t;

/* Full user register snapshot taken at syscall entry (filled by
 * syscall_entry.S, layout fixed and shared with the asm). Used by fork() to
 * build the child's resume context. */
typedef struct user_regs {
    uint64_t rax, rdi, rsi, rdx, r10, r8, r9, rflags, rip;
    uint64_t rbx, rbp, r12, r13, r14, r15, rsp;
} user_regs_t;

/* W7: 协作式调度的内核栈上下文。switch_context 汇编保存/恢复，布局与汇编
 * 硬编码偏移一致：rsp=0 rbx=8 rbp=16 r12=24 r13=32 r14=40 r15=48 valid=56。 */
typedef struct kctx {
    uint64_t rsp;      /* 阻塞点内核栈指针（[rsp] = 返回地址） */
    uint64_t rbx;
    uint64_t rbp;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t valid;    /* 1 = 有保存的上下文可恢复 */
} kctx_t;

/* 阻塞条件类型（task_t.wait_kind）。sched_wake_cond / 主循环轮询据此唤醒。 */
#define WAIT_NONE    0
#define WAIT_CHILD   1   /* wait4：等待子进程退出，arg = 目标 pid(-1=任意) */
#define WAIT_KBD     2   /* tty 读：等待键盘输入 */
#define WAIT_PIPE_RD 3   /* pipe 读：等待数据，arg = pipe id */
#define WAIT_PIPE_WR 4   /* pipe 写：等待空间，arg = pipe id */
#define WAIT_FUTEX   5   /* futex WAIT，arg = 用户地址 */
#define WAIT_TIME    6   /* nanosleep，arg = 唤醒时刻 ms */
#define WAIT_SELECT  7   /* poll/select，arg = 0（主循环重新扫描 fd） */
#define WAIT_SIGTIMED 8  /* rt_sigtimedwait，arg = 唤醒时刻 ms */

/* Task structure */
typedef struct task {
    uint32_t id;
    const char* name;
    void (*func)(void);
    int state;              /* TASK_STATE_* */
    int priority;           /* 0=highest, 4=lowest */
    int time_slice;         /* allocated ticks per round */
    int ticks_left;         /* remaining ticks */
    int core_id;            /* which core this task is pinned to */
    int doorbell_wait;      /* channel waiting on (-1 = none) */
    int saved_priority;     /* original priority before boost */

    /* W5: Process management */
    uint64_t pid;            /* Process ID */
    uint64_t user_rsp;       /* User-space stack pointer */
    uint64_t entry;          /* Program entry point */
    uint64_t is_user;        /* 1 = user-space task, 0 = kernel task */

    /* W6: per-task address space (NULL for kernel tasks). */
    vm_context_t* mm_context;

    /* W6: process tree linkage. */
    uint64_t ppid;           /* parent pid (0 = none) */
    int      exit_code;      /* exit status reported to wait4 */
    user_regs_t fork_ctx;    /* child resume context (set at fork) */

    /* W6: dedicated kernel stack top used by syscall_entry while this task
     * runs (0 = use the shared syscall stack, e.g. the initial task). */
    uint64_t kstack_top;

    /* W7 (Task 2.4): 多用户。默认 0（root），setuid/setgid 可改。 */
    uint32_t uid;    /* 真实 uid */
    uint32_t gid;    /* 真实 gid */
    uint32_t euid;   /* 有效 uid（权限检查用） */
    uint32_t egid;   /* 有效 gid */
    uint32_t sid;    /* 会话 id（setsid 建立） */
    uint16_t umask;  /* 文件创建掩码 */

    /* W7: 协作式调度字段。kctx = 阻塞时的内核栈上下文；
     * exit_ctx = 任务退出时返回调度器所需的 user_ret_* 副本。 */
    kctx_t   kctx;
    kctx_t   exit_ctx;
    int      wait_kind;   /* WAIT_* */
    uint64_t wait_arg;
    int      is_fork;     /* 1 = 首次运行需从 fork_ctx 恢复（fork 子进程） */

    /* W7: 当前工作目录（chdir/getcwd）。 */
    char     cwd[96];

    /* W7: 资源限制（16 项，对应 Linux RLIMIT_* 前 16 个）。 */
    uint64_t rlimit_cur[16];
    uint64_t rlimit_max[16];

    /* W7: 信号备用栈（sigaltstack）。 */
    uint64_t altstack_base;
    uint64_t altstack_size;
    int      altstack_onstack;

    /* Task 3.1: 进程组（job control）。创建任务时初始化为 pid，
     * setpgid 可改；bash 的 getpgrp() = getpgid(0) 读取它。 */
    uint32_t pgrp;

    /* W7 (Task 2.5): 信号。sig_handler[sig] = 用户处理函数地址
     * (0 = 默认动作, 1 = SIG_IGN)；sig_flags/sig_restorer 保存 sigaction
     * 的 SA_RESTORER 信息；sig_pending/sig_blocked 为信号位图。 */
    uint64_t sig_handler[64];
    uint64_t sig_flags[64];
    uint64_t sig_restorer[64];
    uint64_t sig_pending;
    uint64_t sig_blocked;

    /* W7 (Task 4.6/4.7): 定时器与 futex 超时。
     * itimer_deadline = ITIMER_REAL 到期时刻 ms（0 = 未激活）；
     * itimer_interval  = 周期 ms（0 = 一次性）。 */
    uint64_t itimer_deadline;
    uint64_t itimer_interval;

    /* W7 (Task 4.7): futex WAIT 的超时时刻 ms（0 = 无限）。
     * WAIT_FUTEX 的 wait_arg 存 uaddr 用于匹配，超时用本字段。 */
    uint64_t wait_deadline;

    /* W7 (Task 4.7): 可写进程名（prctl PR_SET_NAME）。name 指针指向此缓冲 */
    char     task_name[32];

    /* W7 Phase 3.2: 用户 TLS 段基址（arch_prctl 设置）。MSR_FS_BASE/GS_BASE
     * 是 per-CPU 全局寄存器，必须按任务保存并在切换/返回用户态时恢复，
     * 否则子进程覆盖后父进程读 %fs 金丝雀 #PF。 */
    uint64_t fs_base;
    uint64_t gs_base;
} task_t;

/* MLFQ queue structure */
typedef struct {
    task_t* tasks[MAX_TASKS];
    int head;
    int tail;
    int count;
} mlfq_t;

extern task_t* current_task;

void     sched_init(void);
int      sched_next(void);
void     sched_tick(void);
void     sched_add(task_t* task);
void     sched_block(task_t* task, int doorbell_ch);
void     sched_wake(task_t* task);
void     sched_boost(task_t* task);
void     sched_kill(int task_id);
int      sched_list(char* buf, int max);
task_t*  sched_get_task(int id);
int      sched_task_count(void);
void     sched_loop(void);
void     sched_requeue(task_t* task);
float    sched_load_sample(void);

/* W5: User-space task support */
int      sched_create_user_task(uint64_t entry, uint64_t stack_top);
void     sched_user_task_exit(int task_id);
void     sched_set_user_task_rsp(uint64_t rsp);

/* W6: return the id of the first zombie task, or -1 if none. */
int      sched_reap_zombie(void);

/* W6: find a live (not reaped) child of `parent_pid`; pid > 0 = specific
 * child, pid == -1 = any child. Returns NULL if none. */
task_t*  sched_find_child(uint64_t parent_pid, int64_t pid);

/* W6: record a task's exit code and mark it zombie (called by exit syscall). */
void     sched_mark_exited(task_t* t, int code);

/* W6: allocate a fresh user process (not enqueued in the MLFQ — it is run
 * synchronously via user_run()). Returns NULL when the task table is full. */
task_t*  sched_spawn_process(vm_context_t* mm, uint64_t entry, uint64_t user_rsp);

/* W7: 强制回收无人认领的 ZOMBIE（如退出的 bash），置 REAPED 以释放槽位。 */
void     sched_force_reap(uint64_t pid);

/* W7: 协作式调度。block_and_switch 由阻塞 syscall 调用：置 BLOCKED 并切换
 * 回调度器；唤醒后从调用点继续执行（switch_context 恢复内核栈）。 */
void     sched_block_and_switch(int kind, uint64_t arg);
void     sched_wake_cond(int kind, uint64_t arg);
void     sched_wake_kbd_all(void);
void     sched_wake_select_all(void);
void     sched_poll_timeouts(void);
void     sched_poll_signals(void);
void     sched_wake_waiting_parent(task_t* child);
int      sched_task_alive(uint64_t pid);

/* W7: 调度器上下文（switch_context 的 to/from 目标，sched.c 定义）。 */
extern kctx_t g_sched_kctx;

/* W7: 默认用户凭据（sched.c）。users_init 解析 /etc/passwd 的 banana 行
 * 后设置为 1000；所有用户程序 spawn 时使用。 */
extern uint32_t g_default_uid;
extern uint32_t g_default_gid;

/* W7: 汇编原语（syscall_entry.S）。 */
void     switch_context(kctx_t* from, kctx_t* to);
void     sched_enter_user(uint64_t entry, uint64_t user_rsp);
void     fork_resume(user_regs_t* ctx);

#endif