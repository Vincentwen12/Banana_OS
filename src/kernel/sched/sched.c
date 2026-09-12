#include "sched.h"
#include "vga.h"
#include "printk.h"
#include "timer.h"
#include "power/cstate.h"
#include "fs/vfs.h"
#include "fs/devfs.h"
#include "port.h"
#include "mm.h"
#include "mm/vm.h"
#include "syscall/syscall.h"

/* W7 (Task 4.6): ITIMER_REAL 到期时投递 SIGALRM（signal.c 导出）。 */
extern void signal_deliver_to_task(task_t* t, int sig);
/* signal.c：清零跨任务信号投递/恢复全局（退出/杀/复用槽时调用）。 */
extern void signal_globals_reset(void);

static task_t task_table[MAX_TASKS];
static mlfq_t mlfq[MLFQ_LEVELS];
static int task_count = 0;
task_t* current_task = NULL;

/* W7: 调度器上下文。主循环在启动/恢复任务前建立，任务阻塞时
 * switch_context 返回主循环。valid 由 SAVE_SCHED_RET 置 1。 */
kctx_t g_sched_kctx = { 0, 0, 0, 0, 0, 0, 0, 0 };

/* W7: 默认用户凭据。users_init() 解析 /etc/passwd 的 banana 行后设置为
 * 1000；在用户程序 spawn 前生效——root 仅用于内核初始化阶段。 */
uint32_t g_default_uid = 0;
uint32_t g_default_gid = 0;

/* Load sampling for power policy */
static uint64_t sched_active_ticks = 0;
static uint64_t sched_total_ticks = 0;
static uint64_t sched_sample_start_ms = 0;
static task_t* idle_task = NULL;

/* Time slice per priority level: higher priority = shorter slice */
static int time_slice_for_level(int level) {
    return (MLFQ_LEVELS - level) * TIME_SLICE_BASE;
}

/* Enqueue a task into a specific MLFQ level */
static void mlfq_enqueue(mlfq_t* q, task_t* t) {
    if (q->count >= MAX_TASKS) return;
    q->tasks[q->tail] = t;
    q->tail = (q->tail + 1) % MAX_TASKS;
    q->count++;
}

/* Dequeue a task from an MLFQ level */
static task_t* mlfq_dequeue(mlfq_t* q) {
    if (q->count == 0) return NULL;
    task_t* t = q->tasks[q->head];
    q->head = (q->head + 1) % MAX_TASKS;
    q->count--;
    return t;
}

/* Remove a specific task from an MLFQ level */
static int mlfq_remove(mlfq_t* q, task_t* t) {
    for (int i = 0; i < q->count; i++) {
        int idx = (q->head + i) % MAX_TASKS;
        if (q->tasks[idx] == t) {
            /* Shift remaining elements */
            for (int j = i; j < q->count - 1; j++) {
                int from = (q->head + j + 1) % MAX_TASKS;
                int to = (q->head + j) % MAX_TASKS;
                q->tasks[to] = q->tasks[from];
            }
            q->tail = (q->tail - 1 + MAX_TASKS) % MAX_TASKS;
            q->count--;
            return 1;
        }
    }
    return 0;
}

void sched_init(void) {
    task_count = 0;
    current_task = NULL;
    for (int i = 0; i < MLFQ_LEVELS; i++) {
        mlfq[i].head = 0;
        mlfq[i].tail = 0;
        mlfq[i].count = 0;
    }
    for (int i = 0; i < MAX_TASKS; i++) {
        task_table[i].id = 0;
        task_table[i].name = NULL;
        task_table[i].func = NULL;
        task_table[i].state = TASK_STATE_ZOMBIE;
        task_table[i].priority = 0;
        task_table[i].time_slice = 0;
        task_table[i].ticks_left = 0;
        task_table[i].core_id = -1;
        task_table[i].doorbell_wait = -1;
        task_table[i].saved_priority = 0;
        task_table[i].pid = 0;
        task_table[i].user_rsp = 0;
        task_table[i].entry = 0;
        task_table[i].is_user = 0;
        task_table[i].ppid = 0;
        task_table[i].exit_code = 0;
        task_table[i].uid = 0;
        task_table[i].gid = 0;
        task_table[i].pgrp = 0;
        for (int s = 0; s < 64; s++) {
            task_table[i].sig_handler[s] = 0;
            task_table[i].sig_flags[s] = 0;
            task_table[i].sig_restorer[s] = 0;
        }
        task_table[i].sig_pending = 0;
        task_table[i].sig_blocked = 0;
    }

    /* Create idle task (lowest priority, triggers C-state) */
    idle_task = &task_table[task_count];
    idle_task->id = task_count;
    idle_task->name = "Idle";
    idle_task->func = NULL;
    idle_task->state = TASK_STATE_READY;
    idle_task->priority = MLFQ_LEVELS - 1;  /* Lowest priority */
    idle_task->time_slice = 100;
    idle_task->ticks_left = 100;
    idle_task->core_id = -1;
    idle_task->doorbell_wait = -1;
    idle_task->saved_priority = MLFQ_LEVELS - 1;
    idle_task->pid = 0;
    idle_task->user_rsp = 0;
    idle_task->entry = 0;
    idle_task->is_user = 0;
    idle_task->mm_context = NULL;
    idle_task->pgrp = 0;
    task_count++;
}

int sched_next(void) {
    /* Scan from highest priority (0) to lowest (4) */
    for (int i = 0; i < MLFQ_LEVELS; i++) {
        if (mlfq[i].count > 0) {
            /* Get next task from this level, rotate round-robin */
            task_t* t = mlfq_dequeue(&mlfq[i]);
            if (t && t->state == TASK_STATE_READY) {
                current_task = t;
                t->state = TASK_STATE_RUNNING;
                if (t->ticks_left <= 0) {
                    t->ticks_left = time_slice_for_level(t->priority);
                }
                return t->id;
            }
            /* Task is no longer ready, skip it */
            if (t) {
                /* Debug disabled to reduce AP core spam */
            }
        }
    }
    /* No ready tasks, return idle task */
    if (idle_task && idle_task->state == TASK_STATE_READY) {
        idle_task->state = TASK_STATE_RUNNING;
        current_task = idle_task;
        return idle_task->id;
    }
    return -1;
}

void sched_tick(void) {
    sched_total_ticks++;
    if (current_task && current_task->state == TASK_STATE_RUNNING) {
        sched_active_ticks++;
    }
    if (!current_task || current_task->state != TASK_STATE_RUNNING) return;
    current_task->ticks_left--;
    if (current_task->ticks_left <= 0) {
        /* Time slice exhausted: demote priority */
        if (current_task->priority < MLFQ_LEVELS - 1) {
            current_task->priority++;
        }
        current_task->state = TASK_STATE_READY;
        mlfq_enqueue(&mlfq[current_task->priority], current_task);
        current_task = NULL;
    }
}

void sched_add(task_t* task) {
    if (task_count >= MAX_TASKS) return;
    /* Copy into task table */
    int id = task_count;
    task_table[id] = *task;
    task_table[id].id = id;
    task_table[id].state = TASK_STATE_READY;
    task_table[id].priority = 2; /* Default: middle priority */
    task_table[id].time_slice = time_slice_for_level(2);
    task_table[id].ticks_left = task_table[id].time_slice;
    task_table[id].core_id = 0;
    task_table[id].doorbell_wait = -1;
    task_table[id].saved_priority = 2;
    task_table[id].mm_context = NULL;
    task_table[id].uid = 0;
    task_table[id].gid = 0;
    task_table[id].pgrp = task_table[id].pid;
    for (int s = 0; s < 64; s++) {
        task_table[id].sig_handler[s] = 0;
        task_table[id].sig_flags[s] = 0;
        task_table[id].sig_restorer[s] = 0;
    }
    task_table[id].sig_pending = 0;
    task_table[id].sig_blocked = 0;
    task_count++;
    /* Enqueue */
    mlfq_enqueue(&mlfq[task_table[id].priority], &task_table[id]);
}

/* W5: Create a user-space task */
int sched_create_user_task(uint64_t entry, uint64_t stack_top)
{
    if (task_count >= MAX_TASKS) return -1;

    task_t* t = &task_table[task_count];
    t->id = task_count;
    t->name = "user";
    t->func = (void*)0;  /* No kernel function — runs user code */
    t->state = TASK_STATE_READY;
    t->priority = 2;  /* Medium priority */
    t->time_slice = TIME_SLICE_BASE * 3;
    t->ticks_left = TIME_SLICE_BASE * 3;
    t->core_id = 0;
    t->doorbell_wait = -1;
    t->saved_priority = 2;
    t->pid = task_count;
    t->user_rsp = stack_top;
    t->entry = entry;
    t->is_user = 1;
    t->mm_context = NULL;
    t->uid = 0;
    t->gid = 0;
    t->pgrp = t->pid;
    for (int s = 0; s < 64; s++) {
        t->sig_handler[s] = 0;
        t->sig_flags[s] = 0;
        t->sig_restorer[s] = 0;
    }
    t->sig_pending = 0;
    t->sig_blocked = 0;

    mlfq_enqueue(&mlfq[t->priority], t);
    task_count++;

    /* Allocate stdin/stdout/stderr via devfs /dev/tty */
    {
        file_t* f = devfs_open("/dev/tty", 0);
        if (f) vfs_fd_alloc(f);  /* fd 0 = stdin */
        f = devfs_open("/dev/tty", 0);
        if (f) vfs_fd_alloc(f);  /* fd 1 = stdout */
        f = devfs_open("/dev/tty", 0);
        if (f) vfs_fd_alloc(f);  /* fd 2 = stderr */
    }

    return (int)t->id;
}

/* W5: User task exit — mark as zombie, return to shell */
void sched_user_task_exit(int task_id)
{
    if (task_id < 0 || task_id >= task_count) return;
    task_t* t = &task_table[task_id];
    t->state = TASK_STATE_ZOMBIE;
    t->is_user = 0;
    /* Remove from MLFQ */
    for (int i = 0; i < MLFQ_LEVELS; i++) {
        mlfq_remove(&mlfq[i], t);
    }
}

/* W5: Set the current user task's RSP (for context switching) */
void sched_set_user_task_rsp(uint64_t rsp)
{
    if (current_task && current_task->is_user) {
        current_task->user_rsp = rsp;
    }
}

void sched_block(task_t* task, int doorbell_ch) {
    if (!task || task->id >= task_count) return;
    task->state = TASK_STATE_BLOCKED;
    task->doorbell_wait = doorbell_ch;
    /* Remove from MLFQ */
    mlfq_remove(&mlfq[task->priority], task);
    if (current_task == task) current_task = NULL;
}

void sched_wake(task_t* task) {
    if (!task || task->id >= task_count) return;
    if (task->state != TASK_STATE_BLOCKED) return;
    task->state = TASK_STATE_READY;
    task->doorbell_wait = -1;
    /* Restore priority */
    task->priority = task->saved_priority;
    task->ticks_left = time_slice_for_level(task->priority);
    mlfq_enqueue(&mlfq[task->priority], task);
}

/* W7: 阻塞当前任务并切换到调度器。唤醒（sched_wake_*）后从调用点继续。 */
void sched_block_and_switch(int kind, uint64_t arg)
{
    if (!current_task) return;
    task_t* t = current_task;
    t->wait_kind = kind;
    t->wait_arg = arg;
    t->state = TASK_STATE_BLOCKED;
    mlfq_remove(&mlfq[t->priority], t);
    /* 退出返回上下文（user_ret_* = 主循环调用点）在阻塞期间会被其它任务
     * 的启动覆盖，必须保存到本任务，唤醒后由主循环恢复。 */
    t->exit_ctx.rsp = user_ret_rsp;
    t->exit_ctx.rbx = user_ret_rbx;
    t->exit_ctx.rbp = user_ret_rbp;
    t->exit_ctx.r12 = user_ret_r12;
    t->exit_ctx.r13 = user_ret_r13;
    t->exit_ctx.r14 = user_ret_r14;
    t->exit_ctx.r15 = user_ret_r15;
    t->exit_ctx.valid = 1;
    switch_context(&t->kctx, &g_sched_kctx);
    /* 唤醒后继续（主循环 switch_context 恢复本栈）。 */
    t->state = TASK_STATE_RUNNING;
}

/* W7: 唤醒所有满足 (kind, arg) 条件的阻塞任务。 */
void sched_wake_cond(int kind, uint64_t arg)
{
    for (int i = 0; i < task_count; i++) {
        task_t* t = &task_table[i];
        if (t->state == TASK_STATE_BLOCKED && t->wait_kind == kind &&
            t->wait_arg == arg)
            sched_wake(t);
    }
}

/* W7: 键盘输入到达，唤醒所有 WAIT_KBD 任务。 */
void sched_wake_kbd_all(void)
{
    for (int i = 0; i < task_count; i++) {
        task_t* t = &task_table[i];
        if (t->state == TASK_STATE_BLOCKED && t->wait_kind == WAIT_KBD)
            sched_wake(t);
    }
}

/* W7: 唤醒所有 WAIT_SELECT（poll/select）任务——fd 状态可能已变化。 */
void sched_wake_select_all(void)
{
    for (int i = 0; i < task_count; i++) {
        task_t* t = &task_table[i];
        if (t->state == TASK_STATE_BLOCKED && t->wait_kind == WAIT_SELECT)
            sched_wake(t);
    }
}

/* W7: 唤醒到期的 WAIT_TIME（nanosleep）与超时的 WAIT_SELECT（poll）任务。
 * 同时轮询 ITIMER_REAL：到期投递 SIGALRM（周期型重排下次到期）。 */
void sched_poll_timeouts(void)
{
    uint64_t now = timer_ms();
    for (int i = 0; i < task_count; i++) {
        task_t* t = &task_table[i];
        /* 已死任务不保留定时器：不触发也不重排（死亡进程的周期 SIGALRM
         * 会形成幽灵信号风暴，打到待复用槽/错误当前任务）。 */
        if (t->state == TASK_STATE_ZOMBIE || t->state == TASK_STATE_REAPED) {
            t->itimer_deadline = 0;
            continue;
        }
        /* ITIMER_REAL 到期 → SIGALRM（周期型：重排下一周期） */
        if (t->itimer_deadline && now >= t->itimer_deadline) {
            t->itimer_deadline = t->itimer_interval
                ? t->itimer_deadline + t->itimer_interval : 0;
            signal_deliver_to_task(t, 14);   /* SIGALRM */
        }
        if (t->state != TASK_STATE_BLOCKED) continue;
        if (t->wait_kind == WAIT_TIME || t->wait_kind == WAIT_SIGTIMED) {
            if (now >= t->wait_arg) sched_wake(t);
        } else if (t->wait_kind == WAIT_SELECT) {
            if (now >= t->wait_arg) sched_wake(t);
        } else if (t->wait_kind == WAIT_FUTEX) {
            if (t->wait_deadline && now >= t->wait_deadline) sched_wake(t);
        }
    }
}

/* W7: 唤醒有未决且未阻塞信号的阻塞任务（使其在 syscall 返回路径处理）。 */
void sched_poll_signals(void)
{
    for (int i = 0; i < task_count; i++) {
        task_t* t = &task_table[i];
        if (t->state == TASK_STATE_BLOCKED && t->wait_kind != WAIT_NONE) {
            if (t->sig_pending & ~t->sig_blocked)
                sched_wake(t);
        }
    }
}

/* W7: 子进程退出后唤醒等待它的父进程（wait4 阻塞）。 */
void sched_wake_waiting_parent(task_t* child)
{
    if (!child) return;
    for (int i = 0; i < task_count; i++) {
        task_t* p = &task_table[i];
        if (p->state != TASK_STATE_BLOCKED || p->wait_kind != WAIT_CHILD)
            continue;
        if (p->pid != child->ppid) continue;
        int64_t want = (int64_t)p->wait_arg;
        if (want != -1 && (int64_t)child->pid != want) continue;
        sched_wake(p);
    }
}

/* W7: 任务是否存活（非 ZOMBIE/REAPED）。 */
int sched_task_alive(uint64_t pid)
{
    for (int i = 0; i < task_count; i++) {
        task_t* t = &task_table[i];
        if (t->pid != pid) continue;
        if (t->state == TASK_STATE_ZOMBIE || t->state == TASK_STATE_REAPED)
            return 0;
        return 1;
    }
    return 0;
}

void sched_boost(task_t* task) {
    if (!task || task->id >= task_count) return;
    task->saved_priority = task->priority;
    task->priority = 0; /* Highest priority */
    task->ticks_left = time_slice_for_level(0);
    /* Move to top-level queue */
    mlfq_remove(&mlfq[task->saved_priority], task);
    task->state = TASK_STATE_READY;  /* Ensure task is ready after boost */
    mlfq_enqueue(&mlfq[0], task);
}

void sched_kill(int task_id) {
    if (task_id < 0 || task_id >= task_count) return;
    task_t* t = &task_table[task_id];
    if (t->state == TASK_STATE_ZOMBIE) return;
    t->itimer_deadline = 0;
    t->itimer_interval = 0;
    signal_globals_reset();
    /* Remove from MLFQ */
    mlfq_remove(&mlfq[t->priority], t);
    t->state = TASK_STATE_ZOMBIE;
    if (current_task == t) current_task = NULL;
}

/* W6: find a live (not reaped) child of `parent_pid`; pid > 0 = specific
 * child, pid == -1 = any child. Returns NULL if none. */
task_t* sched_find_child(uint64_t parent_pid, int64_t pid)
{
    for (int i = 0; i < task_count; i++) {
        task_t* t = &task_table[i];
        if (t->ppid != parent_pid) continue;
        if (t->state == TASK_STATE_REAPED) continue;
        if (t->state == TASK_STATE_ZOMBIE && t->name == NULL) continue;
        if (pid > 0 && (int64_t)t->pid != pid) continue;
        return t;
    }
    return NULL;
}

/* W6: record a task's exit code and mark it zombie (called by exit syscall).
 * The task stays zombie until wait4() reaps it (state -> REAPED). */
void sched_mark_exited(task_t* t, int code)
{
    if (!t) return;
    t->exit_code = code;
    /* 死亡进程不得保留定时器/信号投递残留 */
    t->itimer_deadline = 0;
    t->itimer_interval = 0;
    signal_globals_reset();
    /* Remove from MLFQ so the scheduler never picks it again. */
    for (int i = 0; i < MLFQ_LEVELS; i++) {
        mlfq_remove(&mlfq[i], t);
    }
    t->state = TASK_STATE_ZOMBIE;
    /* 关闭该进程自己打开的文件描述符（全局共享 fd 表按 owner 清理）。 */
    extern void vfs_fd_close_all(int pid);
    vfs_fd_close_all(t->pid);
}

/* bash 等无父进程的用户任务退出后停留在 ZOMBIE（没有 wait4 回收）。内核
 * 检测到其前台任务死亡后调用本函数把它置为 REAPED，释放任务槽供
 * sched_spawn_process 复用（bash 登录循环不耗尽 64 槽任务表）。 */
void sched_force_reap(uint64_t pid)
{
    if (pid <= 0) return;
    for (int i = 0; i < task_count; i++) {
        task_t* t = &task_table[i];
        if (t->pid == pid && t->state == TASK_STATE_ZOMBIE) {
            t->state = TASK_STATE_REAPED;
            return;
        }
    }
}

/* W6: scan for the first zombie task (for wait4 reaping). */
int sched_reap_zombie(void) {
    for (int i = 0; i < task_count; i++) {
        if (task_table[i].state == TASK_STATE_ZOMBIE)
            return i;
    }
    return -1;
}

/* W6: allocate a fresh user process and enqueue it into the MLFQ. The task
 * runs via the cooperative scheduler (main loop). Returns NULL when the task
 * table is full (no reusable REAPED slot available). */
task_t* sched_spawn_process(vm_context_t* mm, uint64_t entry, uint64_t user_rsp)
{
    /* 优先复用 REAPED 槽（bash 登录循环/频繁 fork 后 task_table 不膨胀） */
    int reuse = -1;
    for (int i = 0; i < task_count; i++) {
        if (task_table[i].state == TASK_STATE_REAPED) { reuse = i; break; }
    }
    task_t* t;
    if (reuse >= 0) {
        t = &task_table[reuse];
    } else {
        if (task_count >= MAX_TASKS) return NULL;
        t = &task_table[task_count];
    }

    t->id = reuse >= 0 ? reuse : task_count;
    t->name = "user";
    t->func = (void*)0;
    /* 复用 REAPED 槽：先清残留（含跨任务信号全局——新任务不得被上一个
     * 任务残留的投递/恢复请求误伤）。 */
    signal_globals_reset();
    t->fs_base = 0;
    t->gs_base = 0;
    t->state = TASK_STATE_READY;
    t->priority = 2;
    t->time_slice = TIME_SLICE_BASE * 3;
    t->ticks_left = t->time_slice;
    t->core_id = 0;
    t->doorbell_wait = -1;
    t->saved_priority = 2;
    t->pid = t->id;
    t->user_rsp = user_rsp;
    t->entry = entry;
    t->is_user = 1;
    t->mm_context = mm;
    t->ppid = 0;
    t->exit_code = 0;
    t->name = t->task_name;
    t->task_name[0] = 'u'; t->task_name[1] = 's';
    t->task_name[2] = 'e'; t->task_name[3] = 'r';
    t->task_name[4] = '\0';
    t->uid = g_default_uid;
    t->gid = g_default_gid;
    t->euid = g_default_uid;
    t->egid = g_default_gid;
    t->sid = 0;
    t->umask = 0;
    t->pgrp = t->pid;
    /* W7: 协作调度字段。 */
    t->wait_kind = WAIT_NONE;
    t->wait_arg = 0;
    t->is_fork = 0;
    t->kctx.valid = 0;
    t->exit_ctx.valid = 0;
    t->cwd[0] = '/'; t->cwd[1] = '\0';
    for (int r = 0; r < 16; r++) {
        t->rlimit_cur[r] = ~0ULL;   /* 默认无限 */
        t->rlimit_max[r] = ~0ULL;
    }
    t->rlimit_cur[7] = 64;   /* RLIMIT_NOFILE */
    t->rlimit_max[7] = 64;
    t->altstack_base = 0;
    t->altstack_size = 0;
    t->altstack_onstack = 0;
    t->itimer_deadline = 0;
    t->itimer_interval = 0;
    t->wait_deadline = 0;
    for (int s = 0; s < 64; s++) {
        t->sig_handler[s] = 0;
        t->sig_flags[s] = 0;
        t->sig_restorer[s] = 0;
    }
    t->sig_pending = 0;
    t->sig_blocked = 0;

    /* W7: 每任务独立内核栈（syscall 与阻塞切换用），16KB 连续页。 */
    void* kstack = pmalloc_contig(4);
    if (!kstack) {
        printk(KERN_WARNING, "[oom] spawn kstack pid=%u\n", (unsigned)t->pid);
        return NULL;  /* 槽未占用，下次 spawn 会覆盖 */
    }
    t->kstack_top = (uint64_t)kstack + 4 * PAGE_SIZE;

    mlfq_enqueue(&mlfq[t->priority], t);
    if (reuse < 0) task_count++;   /* 复用 REAPED 槽时不增长任务表 */

    /* Wire up stdin/stdout/stderr to /dev/tty. 逐个补齐空闲 std 槽位：
     * 若某槽位已占用（如 bash 已注册）则不动；若空闲（std fd 缺失/被释放）
     * 则补注册，保证每个新进程的 0/1/2 都是可用 tty——文件 open 永不占
     * 用 0/1/2（vfs_fd_alloc 从 3 起），std fd 不会被目录/文件 fd 覆盖。 */
    for (int stdfd = 0; stdfd < 3; stdfd++) {
        if (vfs_fd_get(stdfd) == (file_t*)0) {
            file_t* f = devfs_open("/dev/tty", 0);
            if (f) vfs_fd_set(f, stdfd);
        }
    }
    return t;
}

int sched_list(char* buf, int max) {
    int pos = 0;
    const char* state_names[] = {"RUN", "RDY", "BLK", "SLP", "ZMB", "RPD"};
    const char* header = "ID  Name        State Pri Core\n";
    
    while (header[pos] && pos < max) { buf[pos] = header[pos]; pos++; }
    
    for (int i = 0; i < task_count; i++) {
        task_t* t = &task_table[i];
        if (t->state == TASK_STATE_ZOMBIE && t->name == NULL) continue;
        
        /* ID */
        if (pos + 4 > max) break;
        if (t->id < 10) { buf[pos++] = ' '; buf[pos++] = '0' + t->id; }
        else { buf[pos++] = '0' + t->id / 10; buf[pos++] = '0' + t->id % 10; }
        buf[pos++] = ' '; buf[pos++] = ' ';
        
        /* Name */
        const char* name = t->name ? t->name : "?";
        int name_len = 0;
        while (*name && name_len < 12 && pos < max) {
            buf[pos++] = *name++;
            name_len++;
        }
        while (name_len < 12 && pos < max) { buf[pos++] = ' '; name_len++; }
        
        /* State */
        const char* sn = (t->state >= 0 && t->state <= 5) ? state_names[t->state] : "???";
        for (int j = 0; j < 3 && pos < max; j++) buf[pos++] = sn[j];
        buf[pos++] = ' '; buf[pos++] = ' ';
        
        /* Priority */
        if (pos < max) buf[pos++] = '0' + t->priority;
        buf[pos++] = ' '; buf[pos++] = ' ';
        
        /* Core */
        if (pos < max) buf[pos++] = '0' + t->core_id;
        buf[pos++] = '\n';
    }
    if (pos < max) buf[pos] = '\0';
    else if (pos > 0) buf[pos - 1] = '\0';
    return pos;
}

task_t* sched_get_task(int id) {
    if (id < 0 || id >= task_count) return NULL;
    if (task_table[id].state == TASK_STATE_ZOMBIE) return NULL;
    if (task_table[id].state == TASK_STATE_REAPED) return NULL;
    return &task_table[id];
}

int sched_task_count(void) {
    return task_count;
}

void sched_loop(void) {
    while (1) {
        int tid = sched_next();
        if (tid >= 0) {
            task_t* t = &task_table[tid];
            if (t->func) t->func();
            /* After task function returns, rotate */
            if (t->state == TASK_STATE_RUNNING) {
                t->state = TASK_STATE_READY;
                mlfq_enqueue(&mlfq[t->priority], t);
            }
            current_task = NULL;
        } else {
            /* No tasks ready, idle */
            __asm__ volatile("pause");
        }
    }
}

void sched_requeue(task_t* task) {
    if (!task || task->state != TASK_STATE_RUNNING) return;
    task->state = TASK_STATE_READY;
    mlfq_enqueue(&mlfq[task->priority], task);
    if (current_task == task) current_task = NULL;
}

float sched_load_sample(void) {
    uint64_t now_ms = timer_ms();
    if (sched_total_ticks == 0) return 0.0f;
    
    float load = (float)sched_active_ticks / (float)sched_total_ticks;
    
    /* Reset every POLICY_SAMPLE_MS (100ms) */
    if (now_ms - sched_sample_start_ms >= 100) {
        sched_active_ticks = 0;
        sched_total_ticks = 0;
        sched_sample_start_ms = now_ms;
    }
    
    return load;
}