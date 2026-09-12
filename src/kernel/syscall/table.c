#include "table.h"
#include "syscall.h"
#include "vga.h"
#include "printk.h"
#include "port.h"
#include "timer.h"
#include "mm.h"
#include "mm/vm.h"
#include "sched.h"
#include "keyboard.h"
#include "fs/vfs.h"
#include "fs/devfs.h"
#include "fs/tmpfs.h"
#include "fs/procfs.h"
#include "fs/sysfs.h"
#include "fs/ext2.h"
#include "fs/pipe.h"
#include "elf/loader.h"

/* ---- Individual syscall implementations ---- */

/* 0: sys_read */
static uint64_t sys_read(uint64_t fd, uint64_t buf, uint64_t count,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);  /* EBADF */
    return vfs_read(f, (void*)buf, count);
}

/* 1: sys_write */
static uint64_t sys_write(uint64_t fd, uint64_t buf, uint64_t count,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    file_t* f = vfs_fd_get((int)fd);
    if (!f) {
        return (uint64_t)(-9);  /* EBADF */
    }
    return vfs_write(f, (const void*)buf, count);
}

/* W7 (Task 2.4): vfs_open_mode 在 vfs.c 定义（带 mode 的打开路径 +
 * ext2 权限检查）。 */
extern file_t* vfs_open_mode(const char* path, uint64_t flags, uint64_t mode);
extern int vfs_open_errno;   /* 最近一次 open 失败 errno（-13 = EACCES） */

/* 前向声明（定义在本文件后面，sys_open/sys_openat 需要）。 */
static int exec_strncpy_from_user(vm_context_t* mm, char* dst, uint64_t src, int max);
static int cwd_resolve(const char* cwd, const char* path, char* out, int outsz);

/* 2: sys_open — unified lookup: devfs -> tmpfs -> ext2.
 * W7: mode 参数（Linux rdx=a3）透传 ext2_create_file；权限拒绝返回 -13。 */
static uint64_t sys_open(uint64_t path, uint64_t flags, uint64_t mode,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if (!current_task || !current_task->mm_context)
        return (uint64_t)(-14);  /* EFAULT */

    char pathbuf[96];
    if (exec_strncpy_from_user(current_task->mm_context, pathbuf, path,
                               sizeof(pathbuf) - 1) < 0)
        return (uint64_t)(-14);  /* EFAULT */

    /* 相对路径（含 "." / ".."）相对当前 cwd 解析为绝对路径，否则
     * vfs_open_mode 会把 "." 当字面路径查找而失败（ls 无参数即此路径）。 */
    char resolved[96];
    const char* p = pathbuf;
    if (pathbuf[0] != '/') {
        cwd_resolve(current_task->cwd[0] ? current_task->cwd : "/",
                    pathbuf, resolved, sizeof(resolved));
        p = resolved;
    }

    file_t* f = vfs_open_mode(p, flags, mode);
    if (!f) {
        if (vfs_open_errno == -13) return (uint64_t)(-13);  /* EACCES */
        return (uint64_t)(-2);  /* ENOENT */
    }

    int fd = vfs_fd_alloc(f);
    if (fd < 0) return (uint64_t)(-24);  /* EMFILE */
    return (uint64_t)fd;
}

/* 3: sys_close */
static uint64_t sys_close(uint64_t fd, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);  /* EBADF */
    vfs_close(f);
    vfs_fd_free((int)fd);
    return 0;
}

/* 9: sys_mmap — real mmap for the dynamic linker. Maps zeroed pages (anon) or
 * file-backed pages into the current task's address space. Libraries get a
 * high hint area (0x7f0000100000+) so they never collide with the main
 * program (loaded high, above all kernel physical zones) or with ld-linux at
 * 0x7f0000000000. */
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MMAP_HINT_BASE 0x7f0000100000ULL
static uint64_t mmap_cursor = MMAP_HINT_BASE;

/* 从 start 起向上找 npages 页全空闲的区间（逐页探测当前页表）。
 * 返回 4KB 对齐的基址；扫描上限内无空闲区间返回 0（MAP_FAILED）。 */
static uint64_t mmap_place_free(vm_context_t* mm, uint64_t start, uint64_t npages)
{
    uint64_t a = (start + 0xFFF) & ~0xFFFULL;
    for (uint64_t tries = 0; tries < 0x40000; tries++) {   /* 上限 1GB */
        if (!vm_range_present(mm, a, npages)) return a;
        a += PAGE_SIZE;
    }
    return 0;
}

static uint64_t sys_mmap(uint64_t addr, uint64_t length, uint64_t prot,
                         uint64_t flags, uint64_t fd, uint64_t offset)
{
    (void)offset;
    if (length == 0) return (uint64_t)(-22);  /* EINVAL */

    uint64_t npages = (length + 0xFFF) / 0x1000;
    vm_context_t* mm = current_task ? current_task->mm_context : 0;
    if (!mm) return (uint64_t)(-22);

    /* Linux 语义：自动（addr==0）与 hint（addr!=0 非 MAP_FIXED）分配都不得
     * 落在已映射的活页上，否则新内容会静默覆盖旧映射（malloc arena、已加载
     * .so 的 data/bss）→ glibc 堆损坏。MAP_FIXED 由调用方精确指定区间
     * （ld.so 只在自己的整段 span 内做段重定位），保持原样覆盖。 */
    uint64_t base;
    if (!(flags & MAP_FIXED)) {
        uint64_t hint = (addr != 0) ? addr : mmap_cursor;
        base = mmap_place_free(mm, hint, npages);
        if (!base) return (uint64_t)(-1);           /* MAP_FAILED */
        if (addr == 0) {
            uint64_t next = (base + npages * PAGE_SIZE + 0xFFF) & ~0xFFFULL;
            if (next > mmap_cursor) mmap_cursor = next;
        }
    } else {
        base = addr;   /* MAP_FIXED：按精确地址映射 */
    }

    uint64_t vm_flags = VM_USER;
    if (prot & 1) vm_flags |= VM_READ;
    if (prot & 2) vm_flags |= VM_WRITE;
    if (prot & 4) vm_flags |= VM_EXEC;

    file_t* f = (flags & MAP_ANONYMOUS) ? (file_t*)0 : vfs_fd_get((int)fd);

    for (uint64_t i = 0; i < npages; i++) {
        void* page = vm_alloc_page();
        if (!page) return (uint64_t)(-1);  /* MAP_FAILED */
        if (f) {
            f->offset = offset + i * PAGE_SIZE;
            vfs_read(f, page, PAGE_SIZE);  /* short read = zero-filled tail */
        }
        if (vm_map_page(mm, base + i * PAGE_SIZE, (uint64_t)page, vm_flags) < 0)
            return (uint64_t)(-1);  /* MAP_FAILED */
    }
    return base;
}

/* 11: sys_munmap — real munmap: unmap the user pages and return their backing
 * physical pages to the Ω allocator, so freed ranges are genuinely reusable
 * (ld.so / malloc rely on munmap to release memory). */
static uint64_t sys_munmap(uint64_t addr, uint64_t length,
                           uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    vm_context_t* mm = current_task ? current_task->mm_context : 0;
    if (!mm) return (uint64_t)(-14);   /* EFAULT */
    if ((addr & 0xFFF) || length == 0) return (uint64_t)(-22);   /* EINVAL */
    uint64_t npages = (length + 0xFFF) / 0x1000;
    vm_unmap(mm, addr, npages * PAGE_SIZE);
    return 0;
}

/* 12: sys_brk — per-task program break backed by real user pages.
 * The break starts at the end of the loaded image (.bss, set by elf_load into
 * mm_context->brk). Each extension maps fresh zeroed user pages into the
 * current task's page table, so fork() can deep-copy the heap correctly. */
static uint64_t sys_brk(uint64_t brk, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!current_task || !current_task->mm_context)
        return (uint64_t)(-22);  /* EINVAL */

    vm_context_t* mm = current_task->mm_context;

    if (brk == 0)
        return mm->brk;  /* query: return the current break */

    /* Extend: map fresh zeroed user pages from the current break upward. */
    if (brk > mm->brk) {
        uint64_t start = (mm->brk + 0xFFF) & ~0xFFFULL;
        uint64_t end   = (brk + 0xFFF) & ~0xFFFULL;
        for (uint64_t a = start; a < end; a += PAGE_SIZE) {
            /* [BISECT] 不跳过已存在页 */
            void* page = vm_alloc_page();
            if (!page) return mm->brk;  /* ENOMEM: keep the old break */
            if (vm_map_page(mm, a, (uint64_t)page,
                            VM_USER | VM_READ | VM_WRITE) < 0)
                return mm->brk;
        }
    }
    mm->brk = brk;
    return mm->brk;
}

/* 24: sys_sched_yield */
static uint64_t sys_sched_yield(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    /* Yield CPU — just return, scheduler will handle on next tick */
    return 0;
}

/* 39: sys_getpid */
static uint64_t sys_getpid(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (current_task)
        return current_task->pid;
    return 0;
}

/* 56: clone — glibc's fork() maps to clone(CLONE_CHILD_SETTID|CLEARTID|SIGCHLD,
 * 0, ...) with a NULL stack. Route that to the synchronous fork path. Real
 * threads (non-NULL stack) are not supported yet. */
static uint64_t sys_fork(uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6);
static uint64_t sys_clone(uint64_t flags, uint64_t stack, uint64_t ptid,
                          uint64_t ctid, uint64_t tls, uint64_t a6)
{
    (void)flags;(void)ptid;(void)ctid;(void)tls;(void)a6;
    if (stack != 0) return (uint64_t)(-38);  /* ENOSYS: no threads */
    return sys_fork(0, 0, 0, 0, 0, 0);
}

/* 57: sys_fork — duplicate the current task and its address space (no COW).
 * The child is enqueued into the MLFQ and run by the cooperative scheduler;
 * the parent blocks in wait4 until the child exits. It resumes in user mode
 * as if fork() returned 0. */
static uint64_t sys_fork(uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!current_task || !current_task->mm_context)
        return (uint64_t)(-22);  /* EINVAL */

    vm_context_t* child_mm = vm_fork(current_task->mm_context);
    if (!child_mm) {
        printk(KERN_WARNING, "[oom] fork vm_fork failed pid=%u\n",
               (unsigned)current_task->pid);
        return (uint64_t)(-12);  /* ENOMEM */
    }

    task_t* child = sched_spawn_process(child_mm, 0, 0);
    if (!child) {
        printk(KERN_WARNING, "[oom] fork spawn failed pid=%u\n",
               (unsigned)current_task->pid);
        return (uint64_t)(-11);  /* EAGAIN */
    }

    /* Child resumes at the parent's fork()-return point with RAX = 0. */
    child->fork_ctx = *(user_regs_t*)syscall_user_ctx;
    child->fork_ctx.rax = 0;
    child->ppid = current_task->pid;
    child->is_fork = 1;  /* 首次运行走 fork_resume（从 fork_ctx 恢复） */
    child->state = TASK_STATE_READY;

    /* W7: fork 继承 uid/gid/euid/egid/sid/umask/cwd/资源限制/信号处置。 */
    child->uid  = current_task->uid;
    child->gid  = current_task->gid;
    child->euid = current_task->euid;
    child->egid = current_task->egid;
    child->sid  = current_task->sid;
    child->umask = current_task->umask;
    for (int r = 0; r < 16; r++) {
        child->rlimit_cur[r] = current_task->rlimit_cur[r];
        child->rlimit_max[r] = current_task->rlimit_max[r];
    }
    child->altstack_base = current_task->altstack_base;
    child->altstack_size = current_task->altstack_size;
    child->altstack_onstack = 0;
    /* Task 3.1: fork 子进程继承父进程组（Linux 语义）。 */
    child->pgrp = current_task->pgrp;
    /* W7 3.2: 继承父进程 TLS 段基址（fs/gs base 按任务保存）。 */
    child->fs_base = current_task->fs_base;
    child->gs_base = current_task->gs_base;
    /* cwd 拷贝 */
    int ci = 0;
    while (current_task->cwd[ci] && ci < 95) {
        child->cwd[ci] = current_task->cwd[ci];
        ci++;
    }
    child->cwd[ci] = '\0';
    for (int i = 0; i < 64; i++) {
        child->sig_handler[i]  = current_task->sig_handler[i];
        child->sig_flags[i]    = current_task->sig_flags[i];
        child->sig_restorer[i] = current_task->sig_restorer[i];
    }
    child->sig_pending = 0;
    child->sig_blocked = current_task->sig_blocked;

    return child->pid;
}

/* Write a 32-bit wait status into the parent's user space. */
static int wait4_write_status(vm_context_t* mm, uint64_t status_ptr,
                              uint32_t status)
{
    if (!mm || !status_ptr) return 0;
    return vm_copy_to_user(mm, status_ptr, &status, 4);
}

/* 61: sys_wait4 — wait for a child. The parent blocks (WAIT_CHILD) and the
 * cooperative scheduler runs the child until it exits, then reaps it. */
static uint64_t sys_wait4(uint64_t pid, uint64_t status_ptr, uint64_t options,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if (!current_task) return (uint64_t)(-10);  /* ECHILD */

    task_t* parent = current_task;
    /* glibc 把 waitpid(-1) 作为 int 传参，寄存器里是 0x00000000ffffffff
     * （零扩展），必须按 int32 符号扩展成 -1 才能匹配"任意子进程"。 */
    int64_t want = (int64_t)(int32_t)pid;
    task_t* child = sched_find_child(parent->pid, want);
    if (!child)
        return (uint64_t)(-10);  /* ECHILD */

    if (child->state == TASK_STATE_ZOMBIE) {
        /* Already exited: reap immediately. */
        uint32_t status = ((uint32_t)child->exit_code & 0xFF) << 8;
        if (wait4_write_status(parent->mm_context, status_ptr, status) < 0)
            return (uint64_t)(-14);  /* EFAULT */
        child->state = TASK_STATE_REAPED;
        return child->pid;
    }

    if (options & 1)  /* WNOHANG: don't block */
        return 0;

    /* Block until a matching child is zombie. Woken by
     * sched_wake_waiting_parent() when the child exits.
     * 注意：syscall_user_rsp/syscall_user_ctx 是全局，wait4 阻塞期间子进程
     * 的每个 syscall 都会覆盖它们。父进程唤醒返回时 syscall_entry.S 用
     * syscall_user_rsp 恢复用户 RSP，必须在此保存/恢复父进程的副本。 */
    uint64_t saved_user_rsp = syscall_user_rsp;
    uint64_t saved_user_ctx[16];
    for (int i = 0; i < 16; i++) saved_user_ctx[i] = syscall_user_ctx[i];

    for (;;) {
        sched_block_and_switch(WAIT_CHILD, (uint64_t)want);
        child = sched_find_child(parent->pid, want);
        if (!child) {
            syscall_user_rsp = saved_user_rsp;
            for (int i = 0; i < 16; i++) syscall_user_ctx[i] = saved_user_ctx[i];
            return (uint64_t)(-10);  /* ECHILD（子进程被并发回收） */
        }
        if (child->state == TASK_STATE_ZOMBIE) break;
        /* 唤醒条件已满足但被其它 wait4 抢走：继续等待。 */
    }

    syscall_user_rsp = saved_user_rsp;
    for (int i = 0; i < 16; i++) syscall_user_ctx[i] = saved_user_ctx[i];

    uint32_t status = ((uint32_t)child->exit_code & 0xFF) << 8;
    if (wait4_write_status(parent->mm_context, status_ptr, status) < 0)
        return (uint64_t)(-14);  /* EFAULT */
    child->state = TASK_STATE_REAPED;
    return child->pid;
}

/* 62: sys_kill — W7 (Task 2.5) 完整信号投递实现位于 signal.c：
 * 有 handler 且未阻塞 -> 置 sig_pending（syscall 返回路径投递）；
 * 无 handler -> SIGKILL 及终止类 sched_kill/退出，SIGCHLD 默认忽略。 */
extern uint64_t sys_kill(uint64_t pid, uint64_t sig, uint64_t a3, uint64_t a4,
                         uint64_t a5, uint64_t a6);

/* 13/14/15: rt_sigaction / rt_sigprocmask / rt_sigreturn — W7 (Task 2.5)
 * 实现位于 signal.c。 */
extern uint64_t sys_rt_sigaction(uint64_t sig, uint64_t act, uint64_t oldact,
                                 uint64_t sigsetsize, uint64_t a5, uint64_t a6);
extern uint64_t sys_rt_sigprocmask(uint64_t how, uint64_t set, uint64_t oldset,
                                   uint64_t sigsetsize, uint64_t a5, uint64_t a6);
extern uint64_t sys_rt_sigreturn(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6);

/* 72: sys_fcntl — F_DUPFD (0), F_GETFD (1), F_SETFD (2). */
static uint64_t sys_fcntl(uint64_t fd, uint64_t cmd, uint64_t arg,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    switch (cmd) {
    case 0: {  /* F_DUPFD */
        file_t* f = vfs_fd_get((int)fd);
        if (!f) return (uint64_t)(-9);  /* EBADF */
        int nfd = vfs_fd_alloc_min(f, (int)arg);
        if (nfd < 0) return (uint64_t)(-24);  /* EMFILE */
        return (uint64_t)nfd;
    }
    case 1:  /* F_GETFD */
        return vfs_fd_get((int)fd) ? 0 : (uint64_t)(-9);
    case 2:  /* F_SETFD */
        return vfs_fd_get((int)fd) ? 0 : (uint64_t)(-9);
    case 3: {  /* F_GETFL — 返回实际打开标志（O_NONBLOCK 等） */
        file_t* f = vfs_fd_get((int)fd);
        if (!f) return (uint64_t)(-9);
        return (uint64_t)(int64_t)f->flags;
    }
    case 4: {  /* F_SETFL — 仅 O_NONBLOCK 等可改位生效 */
        file_t* f = vfs_fd_get((int)fd);
        if (!f) return (uint64_t)(-9);
        f->flags = (f->flags & ~(uint64_t)O_NONBLOCK) | (arg & O_NONBLOCK);
        return 0;
    }
    default:
        return (uint64_t)(-22);  /* EINVAL */
    }
}

/* W7: 路径规范化：把 (cwd, path) 解析为规范化绝对路径写入 out。
 * 处理 "." / ".." / 连续斜杠。返回 0 成功，-1 出错。 */
static int exec_strncpy_from_user(vm_context_t* mm, char* dst, uint64_t src,
                                  int max);

static int cwd_resolve(const char* cwd, const char* path, char* out, int outsz)
{
    char stack[8][64];
    int top = 0;

    /* 相对路径：先把 cwd 的分量压栈作为解析基础 */
    if (path[0] != '/') {
        const char* cp = cwd;
        while (*cp) {
            char seg[64];
            int i = 0;
            while (*cp && *cp != '/' && i < 63) seg[i++] = *cp++;
            while (*cp == '/') cp++;
            if (i == 0) continue;
            if (seg[0] == '.' && seg[1] == '\0') continue;
            if (seg[0] == '.' && seg[1] == '.' && seg[2] == '\0') {
                if (top > 0) top--;
                continue;
            }
            if (top < 8) {
                for (int j = 0; j < i && j < 63; j++) stack[top][j] = seg[j];
                stack[top][i < 64 ? i : 63] = '\0';
                top++;
            }
        }
    }

    /* 处理 path 的分量（".." 弹栈，"." 与空分量跳过） */
    const char* p = path;
    while (*p) {
        char seg[64];
        int i = 0;
        while (*p && *p != '/' && i < 63) seg[i++] = *p++;
        while (*p == '/') p++;
        if (i == 0) continue;
        if (seg[0] == '.' && seg[1] == '\0') continue;
        if (seg[0] == '.' && seg[1] == '.' && seg[2] == '\0') {
            if (top > 0) top--;
            continue;
        }
        if (top < 8) {
            for (int j = 0; j < i && j < 63; j++) stack[top][j] = seg[j];
            stack[top][i < 64 ? i : 63] = '\0';
            top++;
        }
    }

    int pos = 0;
    out[pos++] = '/';
    for (int s = 0; s < top; s++) {
        for (int j = 0; stack[s][j] && pos < outsz - 1; j++)
            out[pos++] = stack[s][j];
        if (s != top - 1 && pos < outsz - 1) out[pos++] = '/';
    }
    if (pos == 1) out[pos++] = '\0';   /* 根目录 */
    else out[pos] = '\0';
    return 0;
}

/* 79: sys_getcwd — 返回任务 cwd。 */
static uint64_t sys_getcwd(uint64_t buf, uint64_t size,
                           uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    if (!buf) return (uint64_t)(-14);  /* EFAULT */
    if (!current_task || !current_task->mm_context) return (uint64_t)(-14);
    const char* cwd = current_task->cwd[0] ? current_task->cwd : "/";
    int len = 0;
    while (cwd[len]) len++;
    if ((uint64_t)len + 1 > size) return (uint64_t)(-34);  /* ERANGE */
    if (vm_copy_to_user(current_task->mm_context, buf, cwd, (uint64_t)len + 1) < 0)
        return (uint64_t)(-14);  /* EFAULT */
    return (uint64_t)len;
}

/* 80: sys_chdir — 解析路径（相对当前 cwd），校验为目录后更新任务 cwd。 */
static uint64_t sys_chdir(uint64_t path, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!current_task) return (uint64_t)(-14);
    vm_context_t* mm = current_task->mm_context;
    if (!mm) return (uint64_t)(-14);
    char pathbuf[96];
    if (exec_strncpy_from_user(mm, pathbuf, path, sizeof(pathbuf) - 1) < 0)
        return (uint64_t)(-14);
    char resolved[96];
    cwd_resolve(current_task->cwd[0] ? current_task->cwd : "/", pathbuf,
                resolved, sizeof(resolved));
    uint32_t ino;
    if (ext2_lookup_path(resolved, &ino) < 0) return (uint64_t)(-2);  /* ENOENT */
    uint32_t type;
    if (ext2_inode_type(ino, &type) < 0) return (uint64_t)(-2);
    if (type != EXT2_S_IFDIR) return (uint64_t)(-20);  /* ENOTDIR */
    int i = 0;
    while (resolved[i] && i < 95) { current_task->cwd[i] = resolved[i]; i++; }
    current_task->cwd[i] = '\0';
    return 0;
}

/* 81: fchdir — 按 fd（记录的文件路径）切换 cwd。 */
static uint64_t sys_fchdir(uint64_t fd, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (!current_task) return (uint64_t)(-14);
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);   /* EBADF */
    if (!f->name[0]) return (uint64_t)(-20);  /* ENOTDIR */
    /* 复用 chdir 的路径解析逻辑 */
    char resolved[96];
    cwd_resolve(current_task->cwd[0] ? current_task->cwd : "/", f->name,
                resolved, sizeof(resolved));
    uint32_t ino;
    if (ext2_lookup_path(resolved, &ino) < 0) return (uint64_t)(-2);
    uint32_t type;
    if (ext2_inode_type(ino, &type) < 0) return (uint64_t)(-2);
    if (type != EXT2_S_IFDIR) return (uint64_t)(-20);
    int i = 0;
    while (resolved[i] && i < 95) { current_task->cwd[i] = resolved[i]; i++; }
    current_task->cwd[i] = '\0';
    return 0;
}

/* 59: sys_execve */
#define EXEC_MAX_ARGS 16
#define EXEC_MAX_LEN  128

/* Copy a NUL-terminated string from user space into `dst` (max bytes). */
static int exec_strncpy_from_user(vm_context_t* mm, char* dst, uint64_t src,
                                  int max)
{
    for (int i = 0; i < max; i++) {
        char c;
        if (vm_copy_from_user(mm, &c, src + i, 1) < 0) return -1;
        dst[i] = c;
        if (c == 0) return 0;
    }
    dst[max - 1] = 0;   /* truncated */
    return 0;
}

/* Read a user-space argv/envp pointer vector into `out` (kernel pointers to
 * freshly allocated zeroed pages). Returns the count, or -1 on error. The
 * caller must pfree() each non-NULL entry. */
static int exec_read_strvec(vm_context_t* mm, uint64_t vec,
                            char* out[EXEC_MAX_ARGS])
{
    int n = 0;
    while (n < EXEC_MAX_ARGS - 1) {
        uint64_t p = 0;
        if (vm_copy_from_user(mm, &p, vec + (uint64_t)n * 8, 8) < 0) return -1;
        if (p == 0) break;
        void* pg = vm_alloc_page();
        if (!pg) return -1;
        if (exec_strncpy_from_user(mm, (char*)pg, p, PAGE_SIZE) < 0) {
            pfree(pg);
            return -1;
        }
        out[n++] = (char*)pg;
    }
    out[n] = NULL;
    return n;
}

static void exec_free_vec(char* v[EXEC_MAX_ARGS], int n)
{
    for (int i = 0; i < n; i++) {
        if (v[i]) pfree(v[i]);
    }
}

/* ---- W7 (Task 4.8): execve 默认环境注入 + 用户 envp 合并 ---- */

/* dst = a + b + c（kernel 无 snprintf） */
static void exec_cat3(char* dst, const char* a, const char* b, const char* c)
{
    while (*a) *dst++ = *a++;
    while (*b) *dst++ = *b++;
    while (*c) *dst++ = *c++;
    *dst = '\0';
}

/* 取 "KEY=value" 的 key（不含 '='）到 out */
static void env_key_of(const char* e, char* out, int max)
{
    int i = 0;
    while (e[i] && e[i] != '=' && i < max - 1) { out[i] = e[i]; i++; }
    out[i] = '\0';
}

/* e 是否以 key= 开头 */
static int env_key_match(const char* e, const char* key)
{
    int k = 0;
    while (key[k]) {
        if (e[k] != key[k]) return 0;
        k++;
    }
    return e[k] == '=';
}

static uint64_t sys_execve(uint64_t path, uint64_t argv, uint64_t envp,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if (!current_task || !current_task->mm_context)
        return (uint64_t)(-22);  /* EINVAL */

    vm_context_t* mm = current_task->mm_context;

    /* 1. Read the program path from user space. */
    char pathbuf[64];
    if (exec_strncpy_from_user(mm, pathbuf, path, sizeof(pathbuf) - 1) < 0)
        return (uint64_t)(-14);  /* EFAULT */

    /* 2. Read argv/envp from user space. */
    char* kargv[EXEC_MAX_ARGS];
    char* kenvp[EXEC_MAX_ARGS];
    int argc = exec_read_strvec(mm, argv, kargv);
    if (argc < 0) return (uint64_t)(-14);  /* EFAULT */
    int envc = exec_read_strvec(mm, envp, kenvp);
    if (envc < 0) {
        exec_free_vec(kargv, argc);
        return (uint64_t)(-14);  /* EFAULT */
    }

    /* 3. Load the new image (fresh address space + user stack). */
    elf_context_t ctx;
    if (elf_load(pathbuf, &ctx) < 0) {
        exec_free_vec(kargv, argc);
        exec_free_vec(kenvp, envc);
        return (uint64_t)(-2);  /* ENOENT */
    }

    /* 4. 环境注入：默认 PATH/HOME/TERM/SHELL/USER/LOGNAME/PWD，用户同名键
     * 覆盖默认。kenvp 用户页在 elf_build_user_stack 拷贝后才释放。 */
    char pwd_buf[112];
    char user_buf[32];
    char logname_buf[32];
    const char* uname = (current_task->euid == 0) ? "root" : "banana";
    const char* cwdstr = current_task->cwd[0] ? current_task->cwd : "/";
    exec_cat3(pwd_buf, "PWD=", cwdstr, "");
    exec_cat3(user_buf, "USER=", uname, "");
    exec_cat3(logname_buf, "LOGNAME=", uname, "");
    const char* defaults[8] = {
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "HOME=/home/banana",
        "TERM=xterm",
        "SHELL=/bin/bash",
        user_buf,
        logname_buf,
        pwd_buf,
        NULL
    };
    #define EXEC_MAX_ENV 24
    char* menv[EXEC_MAX_ENV];
    int m = 0;
    for (int i = 0; defaults[i] && m < EXEC_MAX_ENV - 1; i++)
        menv[m++] = (char*)defaults[i];
    for (int i = 0; i < envc && m < EXEC_MAX_ENV - 1; i++) {
        char key[32];
        env_key_of(kenvp[i], key, sizeof(key));
        int replaced = 0;
        for (int j = 0; j < m; j++) {
            if (env_key_match(menv[j], key)) {
                menv[j] = kenvp[i];      /* 用户值覆盖默认 */
                replaced = 1;
                break;
            }
        }
        if (!replaced) menv[m++] = kenvp[i];
    }
    menv[m] = NULL;

    /* 5. Prepare the user stack (argc/argv/envp/auxv). */
    uint64_t rsp = 0;
    if (elf_build_user_stack(&ctx, argc, (const char**)kargv,
                             (const char**)menv, &rsp) < 0) {
        exec_free_vec(kargv, argc);
        exec_free_vec(kenvp, envc);
        return (uint64_t)(-12);  /* ENOMEM */
    }

    /* 6. Switch the task to the new address space (old one is abandoned). */
    exec_free_vec(kargv, argc);
    exec_free_vec(kenvp, envc);

    /* W7: exec 替换进程映像：信号处置/屏蔽/pending 重置为默认（Linux
     * 语义：exec 后捕获的信号恢复默认，SIG_IGN 保持忽略则不必处理）。 */
    for (int i = 0; i < 64; i++) {
        current_task->sig_handler[i]  = 0;
        current_task->sig_flags[i]    = 0;
        current_task->sig_restorer[i] = 0;
    }
    current_task->sig_pending = 0;
    current_task->sig_blocked = 0;

    /* exec 替换进程：清零跨任务信号投递/恢复全局（新镜像不得被旧任务
     * arm 的投递/恢复请求误伤；exit/exec 优先于 sig_request 返回）。 */
    extern void signal_globals_reset(void);
    signal_globals_reset();

    current_task->mm_context = ctx.vmctx;
    current_task->entry = ctx.entry;
    current_task->user_rsp = rsp;
    vm_switch(ctx.vmctx);

    /* 6. Request the assembly entry to iretq into the new image. */
    syscall_exec_entry = ctx.entry;
    syscall_exec_rsp = rsp;
    syscall_exec_request = 1;
    return 0;
}

/* 60: sys_exit */
static uint64_t sys_exit(uint64_t code, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    /* Record the exit status and mark the task zombie. The assembly entry
     * (syscall_entry.S) notices syscall_exit_request and unwinds back to the
     * kernel (never sysret to the exiting program). */
    if (current_task) {
        sched_mark_exited(current_task, (int)code);
    }
    syscall_exit_request = 1;
    return 0;
}

/* 231: sys_exit_group — same semantics as exit for a single-threaded task. */
static uint64_t sys_exit_group(uint64_t code, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    if (current_task) {
        sched_mark_exited(current_task, (int)code);
    }
    syscall_exit_request = 1;
    return 0;
}

/* 32: dup — duplicate an fd to the lowest free slot. */
static uint64_t sys_dup(uint64_t fd, uint64_t a2, uint64_t a3,
                        uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);  /* EBADF */
    int nfd = vfs_fd_alloc(f);
    if (nfd < 0) return (uint64_t)(-24);  /* EMFILE */
    if (f->ref_count > 0) f->ref_count++;   /* dup 语义：共享同一 file */
    return (uint64_t)nfd;
}

/* 109: setpgid — 设置进程组（job control）。pid 为 0/自身 → 当前任务；
 * pgid 为 0 → 使用目标 pid 自身作组号。bash 启动时 setpgid(0,0) 使
 * 自己的 pgrp == pid，与 TIOCGPGRP 返回的 tty_pgrp 一致后不再报警。 */
static uint64_t sys_setpgid(uint64_t pid, uint64_t pgid, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3;(void)a4;(void)a5;(void)a6;
    if (!current_task) return (uint64_t)(-3);   /* ESRCH */
    task_t* t;
    if (pid == 0 || pid == current_task->pid) {
        t = current_task;
    } else {
        t = sched_get_task((int)pid);
        if (!t) return (uint64_t)(-3);          /* ESRCH */
    }
    t->pgrp = (pgid == 0) ? (uint32_t)t->pid : (uint32_t)pgid;
    return 0;
}

/* 121: getpgid — 返回目标任务的进程组。getpgrp() = getpgid(0)（glibc
 * 用 syscall 121），bash 用它比较自己是否在前台进程组。 */
static uint64_t sys_getpgid(uint64_t pid, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    if (!current_task) return (uint64_t)(-3);   /* ESRCH */
    uint32_t r;
    if (pid == 0 || pid == current_task->pid)
        r = current_task->pgrp;
    else {
        task_t* t = sched_get_task((int)pid);
        if (!t) return (uint64_t)(-3);          /* ESRCH */
        r = t->pgrp;
    }
    return r;
}

/* 127: rt_sigpending — 返回"待决且被阻塞"的信号位图（Linux 语义）。 */
static uint64_t sys_rt_sigpending(uint64_t set, uint64_t sigsetsize,
                                  uint64_t a3, uint64_t a4, uint64_t a5,
                                  uint64_t a6)
{
    (void)sigsetsize;(void)a3;(void)a4;(void)a5;(void)a6;
    if (!current_task || !current_task->mm_context) return (uint64_t)(-14);
    uint64_t mask = current_task->sig_pending & current_task->sig_blocked;
    if (vm_copy_to_user(current_task->mm_context, set, &mask, 8) < 0)
        return (uint64_t)(-14);
    return 0;
}

/* 201: time — seconds since boot (glibc/bash use it for timestamps). */
static uint64_t sys_time(uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    uint64_t sec = timer_ms() / 1000;
    if (a1) {  /* optional tloc output */
        vm_context_t* mm = current_task ? current_task->mm_context : 0;
        if (mm) vm_copy_to_user(mm, a1, &sec, 8);
    }
    return sec;
}

/* 302: prlimit64 — 基于每任务 rlimit 表。
 * NOTE: oldlim/newlim 指向单个 struct rlimit64 (16 字节)，一次一个资源。 */
static uint64_t sys_prlimit64(uint64_t pid, uint64_t resource, uint64_t newlim,
                              uint64_t oldlim, uint64_t a5, uint64_t a6)
{
    (void)a5;(void)a6;
    if (!current_task) return 0;
    if (resource >= 16) return (uint64_t)(-22);   /* EINVAL */
    vm_context_t* mm = current_task->mm_context;
    uint64_t cur = current_task->rlimit_cur[resource];
    uint64_t max = current_task->rlimit_max[resource];

    if (oldlim && mm) {
        if (vm_copy_to_user(mm, oldlim, &cur, 8) < 0) return (uint64_t)(-14);
        if (vm_copy_to_user(mm, oldlim + 8, &max, 8) < 0) return (uint64_t)(-14);
    }
    if (newlim && mm) {
        uint64_t ncur = 0, nmax = 0;
        if (vm_copy_from_user(mm, &ncur, newlim, 8) < 0) return (uint64_t)(-14);
        if (vm_copy_from_user(mm, &nmax, newlim + 8, 8) < 0) return (uint64_t)(-14);
        if (ncur > nmax) return (uint64_t)(-22);   /* EINVAL */
        /* 非 root 不能提高硬上限 */
        if (current_task->euid != 0 && nmax > current_task->rlimit_max[resource])
            return (uint64_t)(-1);   /* EPERM */
        current_task->rlimit_cur[resource] = ncur;
        current_task->rlimit_max[resource] = nmax;
    }
    return 0;
}

/* ================= W7: 进程 / 资源 / 文件批 ================= */

/* 112: setsid — 新会话：sid=pgrp=pid。已是进程组长 → EPERM。 */
static uint64_t sys_setsid(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    if (!current_task) return (uint64_t)(-1);   /* EPERM */
    if (current_task->pgrp == current_task->pid)
        return (uint64_t)(-1);   /* EPERM：已是进程组长 */
    current_task->sid = (uint32_t)current_task->pid;
    current_task->pgrp = (uint32_t)current_task->pid;
    return current_task->pid;
}

/* 127: getpgrp — 返回当前任务进程组。 */
static uint64_t sys_getpgrp(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    uint32_t p = current_task ? current_task->pgrp : 0;
    return p;
}

/* 160/97: setrlimit / getrlimit — 每任务 rlimit 表（16 项）。 */
static uint64_t sys_getrlimit(uint64_t resource, uint64_t rlim,
                              uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3;(void)a4;(void)a5;(void)a6;
    if (!current_task || !current_task->mm_context) return (uint64_t)(-14);
    if (resource >= 16) return (uint64_t)(-22);
    uint64_t cur = current_task->rlimit_cur[resource];
    uint64_t max = current_task->rlimit_max[resource];
    if (vm_copy_to_user(current_task->mm_context, rlim, &cur, 8) < 0)
        return (uint64_t)(-14);
    if (vm_copy_to_user(current_task->mm_context, rlim + 8, &max, 8) < 0)
        return (uint64_t)(-14);
    return 0;
}

static uint64_t sys_setrlimit(uint64_t resource, uint64_t rlim,
                              uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3;(void)a4;(void)a5;(void)a6;
    if (!current_task || !current_task->mm_context) return (uint64_t)(-14);
    if (resource >= 16) return (uint64_t)(-22);
    uint64_t cur = 0, max = 0;
    if (vm_copy_from_user(current_task->mm_context, &cur, rlim, 8) < 0)
        return (uint64_t)(-14);
    if (vm_copy_from_user(current_task->mm_context, &max, rlim + 8, 8) < 0)
        return (uint64_t)(-14);
    if (cur > max) return (uint64_t)(-22);
    if (current_task->euid != 0 && max > current_task->rlimit_max[resource])
        return (uint64_t)(-1);   /* EPERM */
    current_task->rlimit_cur[resource] = cur;
    current_task->rlimit_max[resource] = max;
    return 0;
}

/* 98: getrusage — 返回全零 struct rusage（本内核不统计资源用量）。 */
static uint64_t sys_getrusage(uint64_t who, uint64_t usage,
                              uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)who;(void)a3;(void)a4;(void)a5;(void)a6;
    if (!current_task || !current_task->mm_context) return (uint64_t)(-14);
    static const uint8_t zeros[144] = {0};   /* struct rusage = 144 字节 */
    if (vm_copy_to_user(current_task->mm_context, usage, zeros, sizeof(zeros)) < 0)
        return (uint64_t)(-14);
    return 0;
}

/* 8: lseek — SEEK_SET/CUR/END（复用 vfs_lseek）。 */
static uint64_t sys_lseek(uint64_t fd, uint64_t offset, uint64_t whence,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4;(void)a5;(void)a6;
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);   /* EBADF */
    uint64_t r = vfs_lseek(f, offset, whence);
    if (r == (uint64_t)(-1)) return (uint64_t)(-22);   /* EINVAL */
    return r;
}

/* 83/84: mkdir / rmdir — 解析父目录 + 名字，调 ext2_mkdir/rmdir。 */
static int split_path_ino(const char* p, uint32_t* parent_ino, char* name,
                          int name_max)
{
    char parent[256];
    const char* slash = 0;
    for (const char* q = p; *q; q++) if (*q == '/') slash = q;
    if (!slash) return -2;   /* ENOENT */
    int plen = (int)(slash - p);
    if (plen == 0) {
        parent[0] = '/'; parent[1] = '\0';
    } else {
        if (plen >= (int)sizeof(parent)) return -36;
        for (int i = 0; i < plen; i++) parent[i] = p[i];
        parent[plen] = '\0';
    }
    if (ext2_lookup_path(parent, parent_ino) < 0) return -2;
    int nlen = 0;
    for (const char* q = slash + 1; *q && nlen < name_max - 1; q++) name[nlen++] = *q;
    name[nlen] = '\0';
    if (nlen == 0) return -2;
    return 0;
}

static uint64_t sys_mkdir(uint64_t path, uint64_t mode,
                          uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3;(void)a4;(void)a5;(void)a6;
    if (!current_task || !current_task->mm_context) return (uint64_t)(-14);
    vm_context_t* mm = current_task->mm_context;
    char pathbuf[96];
    if (exec_strncpy_from_user(mm, pathbuf, path, sizeof(pathbuf) - 1) < 0)
        return (uint64_t)(-14);
    char resolved[96];
    cwd_resolve(current_task->cwd[0] ? current_task->cwd : "/", pathbuf,
                resolved, sizeof(resolved));
    /* 父目录写权限检查 */
    char name[64];
    uint32_t parent_ino;
    int r = split_path_ino(resolved, &parent_ino, name, sizeof(name));
    if (r < 0) return (uint64_t)r;
    if (ext2_lookup_path(resolved, 0) == 0) return (uint64_t)(-17);  /* EEXIST */
    if (vfs_dir_write_perm(parent_ino) < 0) return (uint64_t)(-13);  /* EACCES */
    (void)mode;
    if (ext2_mkdir(parent_ino, name) < 0) return (uint64_t)(-17);
    return 0;
}

static uint64_t sys_rmdir(uint64_t path, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    if (!current_task || !current_task->mm_context) return (uint64_t)(-14);
    vm_context_t* mm = current_task->mm_context;
    char pathbuf[96];
    if (exec_strncpy_from_user(mm, pathbuf, path, sizeof(pathbuf) - 1) < 0)
        return (uint64_t)(-14);
    char resolved[96];
    cwd_resolve(current_task->cwd[0] ? current_task->cwd : "/", pathbuf,
                resolved, sizeof(resolved));
    char name[64];
    uint32_t parent_ino;
    int r = split_path_ino(resolved, &parent_ino, name, sizeof(name));
    if (r < 0) return (uint64_t)r;
    if (vfs_dir_write_perm(parent_ino) < 0) return (uint64_t)(-13);
    int e = ext2_rmdir(parent_ino, name);
    if (e == -2) return (uint64_t)(-39);   /* ENOTEMPTY */
    if (e < 0) return (uint64_t)(-2);      /* ENOENT */
    return 0;
}

/* 82: rename — 旧路径 → 新路径（跨目录）。 */
static uint64_t sys_rename(uint64_t oldp, uint64_t newp,
                           uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3;(void)a4;(void)a5;(void)a6;
    if (!current_task || !current_task->mm_context) return (uint64_t)(-14);
    vm_context_t* mm = current_task->mm_context;
    char ob[96], nb[96];
    if (exec_strncpy_from_user(mm, ob, oldp, sizeof(ob) - 1) < 0) return (uint64_t)(-14);
    if (exec_strncpy_from_user(mm, nb, newp, sizeof(nb) - 1) < 0) return (uint64_t)(-14);
    char ors[96], nrs[96];
    cwd_resolve(current_task->cwd[0] ? current_task->cwd : "/", ob, ors, sizeof(ors));
    cwd_resolve(current_task->cwd[0] ? current_task->cwd : "/", nb, nrs, sizeof(nrs));
    char oname[64], nname[64];
    uint32_t oparent, nparent;
    if (split_path_ino(ors, &oparent, oname, sizeof(oname)) < 0) return (uint64_t)(-2);
    if (split_path_ino(nrs, &nparent, nname, sizeof(nname)) < 0) return (uint64_t)(-2);
    if (vfs_dir_write_perm(oparent) < 0 || vfs_dir_write_perm(nparent) < 0)
        return (uint64_t)(-13);
    if (ext2_rename(oparent, oname, nparent, nname) < 0) return (uint64_t)(-2);
    return 0;
}

/* 89: readlink — 读符号链接目标。 */
static uint64_t sys_readlink(uint64_t path, uint64_t buf, uint64_t bufsiz,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4;(void)a5;(void)a6;
    if (!current_task || !current_task->mm_context) return (uint64_t)(-14);
    vm_context_t* mm = current_task->mm_context;
    char pathbuf[96];
    if (exec_strncpy_from_user(mm, pathbuf, path, sizeof(pathbuf) - 1) < 0)
        return (uint64_t)(-14);
    uint32_t ino;
    if (ext2_lookup_path(pathbuf, &ino) < 0) return (uint64_t)(-2);   /* ENOENT */
    uint32_t type;
    if (ext2_inode_type(ino, &type) < 0) return (uint64_t)(-2);
    if (type != EXT2_S_IFLNK) return (uint64_t)(-22);   /* EINVAL（非符号链接） */
    char target[256];
    int n = ext2_symlink_target(ino, target, sizeof(target));
    if (n < 0) return (uint64_t)(-22);
    if ((uint64_t)n > bufsiz) n = (int)bufsiz;
    if (vm_copy_to_user(mm, buf, target, (uint64_t)n) < 0) return (uint64_t)(-14);
    return (uint64_t)n;
}

/* 90: chmod — 改文件权限位。仅文件属主或 root 可改。 */
static int chmod_path(const char* pathbuf, uint64_t mode)
{
    uint32_t ino;
    if (ext2_lookup_path(pathbuf, &ino) < 0) return -2;   /* ENOENT */
    if (current_task && current_task->euid != 0) {
        ext2_inode_t inode;
        if (ext2_read_inode(ino, &inode) == 0 &&
            inode.i_uid != current_task->euid)
            return -1;   /* EPERM */
    }
    if (ext2_chmod(ino, (uint32_t)mode) < 0) return -2;
    return 0;
}

static uint64_t sys_chmod(uint64_t path, uint64_t mode,
                          uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3;(void)a4;(void)a5;(void)a6;
    if (!current_task || !current_task->mm_context) return (uint64_t)(-14);
    char pathbuf[96];
    if (exec_strncpy_from_user(current_task->mm_context, pathbuf, path,
                               sizeof(pathbuf) - 1) < 0) return (uint64_t)(-14);
    char resolved[96];
    cwd_resolve(current_task->cwd[0] ? current_task->cwd : "/", pathbuf,
                resolved, sizeof(resolved));
    return (uint64_t)chmod_path(resolved, mode);
}

/* 268: fchmodat — dirfd 支持 AT_FDCWD；flags 支持 AT_SYMLINK_NOFOLLOW(忽略)。 */
#define AT_FDCWD (-100)
static uint64_t sys_fchmodat(uint64_t dirfd, uint64_t path, uint64_t mode,
                             uint64_t flags, uint64_t a5, uint64_t a6)
{
    (void)a5;(void)a6;
    if ((int)dirfd != AT_FDCWD) return (uint64_t)(-38);   /* ENOSYS */
    (void)flags;
    return sys_chmod(path, mode, 0, 0, 0, 0);
}

/* 95: umask — 设置任务 umask，返回旧值。 */
static uint64_t sys_umask(uint64_t mask, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    if (!current_task) return 0;
    uint64_t old = current_task->umask;
    current_task->umask = (uint16_t)(mask & 0x1FF);
    return old;
}

/* 76/77: truncate / ftruncate — 截断到 0（本内核 ext2_truncate 仅支持 size 0）。 */
static uint64_t sys_truncate(uint64_t path, uint64_t length,
                             uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3;(void)a4;(void)a5;(void)a6;
    (void)length;   /* 仅支持截断到 0 */
    if (!current_task || !current_task->mm_context) return (uint64_t)(-14);
    char pathbuf[96];
    if (exec_strncpy_from_user(current_task->mm_context, pathbuf, path,
                               sizeof(pathbuf) - 1) < 0) return (uint64_t)(-14);
    char resolved[96];
    cwd_resolve(current_task->cwd[0] ? current_task->cwd : "/", pathbuf,
                resolved, sizeof(resolved));
    uint32_t ino;
    if (ext2_lookup_path(resolved, &ino) < 0) return (uint64_t)(-2);
    if (ext2_truncate(ino) < 0) return (uint64_t)(-2);
    return 0;
}

static uint64_t sys_ftruncate(uint64_t fd, uint64_t length,
                              uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3;(void)a4;(void)a5;(void)a6;
    (void)length;
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);   /* EBADF */
    uint32_t ino = (uint32_t)(uintptr_t)f->private_data;
    if (ino == 0) return (uint64_t)(-22);
    if (ext2_truncate(ino) < 0) return (uint64_t)(-22);
    f->size = 0;
    return 0;
}

/* 137/138: statfs / fstatfs — 从 ext2 超级块填 struct statfs（x86-64 88 字节）。 */
static int statfs_fill(vm_context_t* mm, uint64_t out)
{
    if (!mm || !out) return -14;
    uint64_t vals[11];
    uint32_t f_blocks = 0, f_bfree = 0, f_files = 0, f_ffree = 0;
    ext2_superblock_counts(&f_blocks, &f_bfree, &f_files, &f_ffree);
    vals[0] = 0xEF53;                 /* f_type */
    vals[1] = ext2_block_size();      /* f_bsize */
    vals[2] = f_blocks;               /* f_blocks */
    vals[3] = f_bfree;
    vals[4] = f_bfree;
    vals[5] = f_files;
    vals[6] = f_ffree;
    vals[7] = 0;                      /* f_fsid 占位（4 字节，只写 0） */
    vals[8] = 255;                    /* f_namelen */
    vals[9] = ext2_block_size();      /* f_frsize */
    vals[10] = 0;                     /* f_flags */
    /* 逐字段写（f_fsid 为 2×int 共 8 字节，vals[7] 覆盖前 4 字节，再补 4） */
    if (vm_copy_to_user(mm, out, vals, 8 * 7) < 0) return -14;
    uint32_t fsid = 0;
    if (vm_copy_to_user(mm, out + 8 * 7, &fsid, 4) < 0) return -14;
    if (vm_copy_to_user(mm, out + 8 * 7 + 4, &fsid, 4) < 0) return -14;
    if (vm_copy_to_user(mm, out + 8 * 8, &vals[8], 8) < 0) return -14;
    if (vm_copy_to_user(mm, out + 8 * 9, &vals[9], 8) < 0) return -14;
    if (vm_copy_to_user(mm, out + 8 * 10, &vals[10], 8) < 0) return -14;
    return 0;
}

static uint64_t sys_statfs(uint64_t path, uint64_t buf,
                           uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3;(void)a4;(void)a5;(void)a6;
    (void)path;   /* 单一文件系统，路径仅用于存在性 */
    if (!current_task) return (uint64_t)(-14);
    return (uint64_t)statfs_fill(current_task->mm_context, buf);
}

static uint64_t sys_fstatfs(uint64_t fd, uint64_t buf,
                            uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3;(void)a4;(void)a5;(void)a6;
    (void)fd;
    if (!current_task) return (uint64_t)(-14);
    return (uint64_t)statfs_fill(current_task->mm_context, buf);
}

/* 99: sysinfo — 系统信息（uptime / 内存 / 进程数）。 */
static uint64_t sys_sysinfo(uint64_t info, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    if (!current_task || !current_task->mm_context) return (uint64_t)(-14);
    vm_context_t* mm = current_task->mm_context;
    uint64_t total = 0, used = 0, free_pages = 0;
    mm_stats(&total, &used, &free_pages);
    uint64_t uptime = timer_ms() / 1000;
    uint64_t totalram = total * PAGE_SIZE;
    uint64_t freeram = free_pages * PAGE_SIZE;
    uint64_t procs = (uint64_t)sched_task_count();
    uint64_t zero = 0;
    uint64_t off = 0;
    /* struct sysinfo: uptime(8) loads[3](24) totalram(8) freeram(8)
       sharedram(8) bufferram(8) totalswap(8) freeswap(8) procs(2) pad(2)
       totalhigh(8) freehigh(8) mem_unit(4) _f(20) */
    if (vm_copy_to_user(mm, info + off, &uptime, 8) < 0) return (uint64_t)(-14); off += 8;
    if (vm_copy_to_user(mm, info + off, &zero, 24) < 0) return (uint64_t)(-14); off += 24;
    if (vm_copy_to_user(mm, info + off, &totalram, 8) < 0) return (uint64_t)(-14); off += 8;
    if (vm_copy_to_user(mm, info + off, &freeram, 8) < 0) return (uint64_t)(-14); off += 8;
    if (vm_copy_to_user(mm, info + off, &zero, 24) < 0) return (uint64_t)(-14); off += 24;
    uint16_t p16 = (uint16_t)procs;
    if (vm_copy_to_user(mm, info + off, &p16, 2) < 0) return (uint64_t)(-14); off += 2;
    uint16_t pad = 0;
    if (vm_copy_to_user(mm, info + off, &pad, 2) < 0) return (uint64_t)(-14); off += 2;
    if (vm_copy_to_user(mm, info + off, &zero, 16) < 0) return (uint64_t)(-14); off += 16;
    uint32_t mem_unit = 1;
    if (vm_copy_to_user(mm, info + off, &mem_unit, 4) < 0) return (uint64_t)(-14); off += 4;
    if (vm_copy_to_user(mm, info + off, &zero, 20) < 0) return (uint64_t)(-14);
    return 0;
}

/* 273: mlock2 — no-op (bash only uses it opportunistically). */
static uint64_t sys_mlock2(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6; return 0; }

/* 334: rseq — restartable sequences not supported; ENOSYS lets glibc
 * silently fall back to plain per-thread TLS setup. */
static uint64_t sys_rseq(uint64_t a1, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6; return (uint64_t)(-38); }

/* 63: sys_uname */
static uint64_t sys_uname(uint64_t buf, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;

    /*
     * struct utsname (simplified):
     *   sysname[65]  = "BananaOS"
     *   nodename[65] = "axion-ban"
     *   release[65]  = "5.15.0-axion"  (>= 3.2，glibc 2.35 启动时检查内核版本)
     *   version[65]  = "Axion-Ban v2.0.1"
     *   machine[65]  = "x86_64"
     *   domainname[65] = ""
     */
    char* dest = (char*)buf;
    const char* sysname = "BananaOS";
    const char* nodename = "axion-ban";
    const char* release  = "5.15.0-axion";
    const char* version  = "Axion-Ban v2.0.1";
    const char* machine  = "x86_64";

    for (int i = 0; i < 65; i++) {
        dest[i] = (i < 8) ? sysname[i] : 0;
    }
    for (int i = 0; i < 65; i++) {
        dest[65 + i] = (i < 8) ? nodename[i] : 0;
    }
    for (int i = 0; i < 65; i++) {
        dest[130 + i] = (i < 13) ? release[i] : 0;
    }
    for (int i = 0; i < 65; i++) {
        dest[195 + i] = (i < 15) ? version[i] : 0;
    }
    for (int i = 0; i < 65; i++) {
        dest[260 + i] = (i < 6) ? machine[i] : 0;
    }
    return 0;
}

/* 96: sys_gettimeofday */
static uint64_t sys_gettimeofday(uint64_t tv, uint64_t tz,
                                 uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)tz; (void)a3; (void)a4; (void)a5; (void)a6;

    /*
     * struct timeval:
     *   tv_sec  (8 bytes)
     *   tv_usec (8 bytes)
     */
    uint64_t ms = timer_ms();
    uint64_t sec = ms / 1000;
    uint64_t usec = (ms % 1000) * 1000;

    if (tv) {
        ((uint64_t*)tv)[0] = sec;
        ((uint64_t*)tv)[1] = usec;
    }
    return 0;
}

/* ---- bash 最小运行所需补充 syscall ---- */

/* 10: mprotect — no real protection tracking; accept and succeed. */
static uint64_t sys_mprotect(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1; (void)a2; (void)a3; (void)a4; (void)a5; (void)a6;
    return 0;
}

/* 16: ioctl — tty requests needed by bash/readline.
 * TCGETS returns a standard interactive termios (ISIG|ICANON|ECHO,
 * OPOST|ONLCR) so readline keeps line editing and echoing; TCSETS and the
 * pgrp/winsize requests are accepted so bash's job-control setup succeeds. */
static uint64_t sys_ioctl(uint64_t fd, uint64_t req, uint64_t arg,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4; (void)a5; (void)a6;
    if (!vfs_fd_get((int)fd))
        return (uint64_t)(-9);   /* EBADF */
    vm_context_t* mm = current_task ? current_task->mm_context : 0;

    switch (req) {
    case 0x5401: {  /* TCGETS */
        if (!mm || !arg) return (uint64_t)(-14);  /* EFAULT */
        uint32_t lflag = 0, oflag = 0;
        devfs_tty_get_flags(&lflag, &oflag);
        uint8_t t[36];
        for (int i = 0; i < 36; i++) t[i] = 0;
        *(uint32_t*)(t + 4)  = oflag;   /* c_oflag */
        *(uint32_t*)(t + 12) = lflag;   /* c_lflag */
        if (vm_copy_to_user(mm, arg, t, 36) < 0)
            return (uint64_t)(-14);
        return 0;
    }
    case 0x5402: case 0x5403: case 0x5404: {  /* TCSETS/TCSETSW/TCSETSF */
        if (!mm || !arg) return (uint64_t)(-14);
        uint32_t lflag = 0, oflag = 0;
        if (vm_copy_from_user(mm, &oflag, arg + 4, 4) < 0 ||
            vm_copy_from_user(mm, &lflag, arg + 12, 4) < 0)
            return (uint64_t)(-14);
        devfs_tty_set_flags(lflag, oflag);
        return 0;
    }
    case 0x540F: {  /* TIOCGPGRP — return the foreground process group */
        if (!mm || !arg) return (uint64_t)(-14);
        uint32_t pgrp = devfs_tty_get_pgrp();
        if (vm_copy_to_user(mm, arg, &pgrp, 4) < 0)
            return (uint64_t)(-14);
        return 0;
    }
    case 0x5410: {  /* TIOCSPGRP — set the foreground process group */
        if (!mm || !arg) return (uint64_t)(-14);
        uint32_t pgrp = 0;
        if (vm_copy_from_user(mm, &pgrp, arg, 4) < 0)
            return (uint64_t)(-14);
        devfs_tty_set_pgrp(pgrp);
        return 0;
    }
    case 0x5413: {  /* TIOCGWINSZ — 80x24 */
        if (!mm || !arg) return (uint64_t)(-14);
        uint16_t ws[4] = { 24, 80, 0, 0 };  /* row, col, xpixel, ypixel */
        if (vm_copy_to_user(mm, arg, ws, 8) < 0)
            return (uint64_t)(-14);
        return 0;
    }
    case 0x5414:  /* TIOCSWINSZ — accept */
        return 0;
    default:
        return (uint64_t)(-25);  /* ENOTTY */
    }
}

/* glibc x86-64 struct stat layout (144 bytes). Only the fields bash touches
 * are populated; the rest are zero. */
static void fill_stat(uint64_t ino, uint64_t size, uint64_t mode, uint64_t buf)
{
    if (!buf) return;   /* existence-only probe (access) */
    uint64_t* s = (uint64_t*)buf;
    for (int i = 0; i < 18; i++) s[i] = 0;
    s[0] = 0;                        /* st_dev */
    s[1] = ino;                      /* st_ino */
    s[2] = 1;                        /* st_nlink */
    s[3] = mode | 0x1FF;             /* st_mode (uid/pad zero) */
    s[6] = size;                     /* st_size */
    s[7] = 1024;                     /* st_blksize */
    s[8] = (size + 511) / 512;       /* st_blocks */
}

/* Resolve a path without consuming the ext2 file_pool (stat/access hot path).
 * Returns 0 on success and fills the stat at `buf`, or -errno. */
static int stat_path(const char* p, uint64_t buf)
{
    file_t* f = devfs_open(p, 0);
    if (f) { fill_stat(f->inode, f->size, 0x2000, buf); return 0; }
    f = tmpfs_open(p, 0);
    if (f) { fill_stat(f->inode, f->size, 0x8000, buf); return 0; }

    /* 伪文件系统（生成式）：open 后立即 close 归还槽位 */
    if (procfs_is_path(p)) {
        f = procfs_open(p, 0);
        if (f) {
            fill_stat(f->inode, f->size, 0x8000, buf);
            vfs_close(f);
            return 0;
        }
    }
    if (sysfs_is_path(p)) {
        f = sysfs_open(p, 0);
        if (f) {
            fill_stat(f->inode, f->size, 0x8000, buf);
            vfs_close(f);
            return 0;
        }
    }

    uint32_t ino;
    if (ext2_lookup_path(p, &ino) < 0) {
        return -2;   /* ENOENT */
    }
    uint32_t type = 0, sz = 0;
    ext2_inode_type(ino, &type);
    ext2_inode_size(ino, &sz);
    if (type == 0) type = 0x8000;
    fill_stat(ino, sz, type, buf);
    return 0;
}

/* 4/5/6: stat/fstat/lstat */
static uint64_t sys_stat(uint64_t path, uint64_t buf,
                         uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    int r = stat_path((const char*)path, buf);
    return r < 0 ? (uint64_t)r : 0;
}
static uint64_t sys_lstat(uint64_t path, uint64_t buf,
                          uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    return sys_stat(path, buf, a3, a4, a5, a6);   /* no symlink resolution */
}

static uint64_t sys_fstat(uint64_t fd, uint64_t buf,
                          uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);   /* EBADF */
    uint64_t mode;
    if (f->f_type == EXT2_S_IFDIR || f->f_type == EXT2_S_IFREG ||
        f->f_type == EXT2_S_IFLNK)
        mode = f->f_type;
    else
        mode = (f->inode >= 1 && f->inode <= 3) ? 0x2000 : 0x8000;
    fill_stat(f->inode, f->size, mode, buf);
    return 0;
}

/* 21: access */
static uint64_t sys_access(uint64_t path, uint64_t mode,
                           uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)mode; (void)a3; (void)a4; (void)a5; (void)a6;
    int r = stat_path((const char*)path, 0);
    return r < 0 ? (uint64_t)r : 0;
}

/* 33: dup2 */
static uint64_t sys_dup2(uint64_t oldfd, uint64_t newfd,
                         uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3; (void)a4; (void)a5; (void)a6;
    int r = vfs_fd_dup2((int)oldfd, (int)newfd);
    return r < 0 ? (uint64_t)(-9) : (uint64_t)r;   /* EBADF */
}

/* 102/104/107/108: uid/gid (W7: 从 current_task 读取 real/effective) */
static uint64_t sys_getuid(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
  return current_task ? current_task->uid : 0; }
static uint64_t sys_getgid(uint64_t a1, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
  return current_task ? current_task->gid : 0; }
static uint64_t sys_geteuid(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
  return current_task ? current_task->euid : 0; }
static uint64_t sys_getegid(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
  return current_task ? current_task->egid : 0; }

/* 105/106: setuid/setgid — Linux 语义：
 *  - euid==0 (root) 可设置任意值；
 *  - 普通用户仅允许设置为自己的 real/effective uid（无操作）。
 * 设后 real/effective 同步更新（本内核无 saved id 概念）。 */
static uint64_t sys_setuid(uint64_t uid, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    if (!current_task) return 0;
    uint32_t newuid = (uint32_t)uid;
    if (current_task->euid != 0 &&
        newuid != current_task->uid && newuid != current_task->euid)
        return (uint64_t)(-1);   /* EPERM */
    current_task->uid = newuid;
    current_task->euid = newuid;
    return 0;
}
static uint64_t sys_setgid(uint64_t gid, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    if (!current_task) return 0;
    uint32_t newgid = (uint32_t)gid;
    if (current_task->egid != 0 &&
        newgid != current_task->gid && newgid != current_task->egid)
        return (uint64_t)(-1);   /* EPERM */
    current_task->gid = newgid;
    current_task->egid = newgid;
    return 0;
}

/* 110: getppid */
static uint64_t sys_getppid(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    if (current_task) return current_task->ppid;
    return 0;
}

/* 158: arch_prctl — set/query the user fs/gs base via MSR (works without
 * CR4.FSGSBASE; wrmsr at CPL0 is always allowed). */
#define MSR_FS_BASE 0xC0000100
#define MSR_GS_BASE 0xC0000101
static uint64_t sys_arch_prctl(uint64_t code, uint64_t addr,
                               uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3;(void)a4;(void)a5;(void)a6;
    switch (code) {
    case 0x1001:  /* ARCH_SET_GS */
        if (current_task) current_task->gs_base = addr;
        wrmsr(MSR_GS_BASE, addr);
        return 0;
    case 0x1002:  /* ARCH_SET_FS */
        if (current_task) current_task->fs_base = addr;
        wrmsr(MSR_FS_BASE, addr);
        return 0;
    case 0x1003:  /* ARCH_GET_FS */
        if (!addr) return (uint64_t)(-22);   /* EINVAL */
        ((uint64_t*)addr)[0] = rdmsr(MSR_FS_BASE);
        return 0;
    case 0x1004:  /* ARCH_GET_GS */
        if (!addr) return (uint64_t)(-22);
        ((uint64_t*)addr)[0] = rdmsr(MSR_GS_BASE);
        return 0;
    default:
        return (uint64_t)(-22);   /* EINVAL */
    }
}

/* 202: futex — 单地址等待队列真实现（协作式）。
 * FUTEX_WAIT(0): *uaddr == val 才阻塞（WAIT_FUTEX, arg=uaddr），
 *   超时（wait_deadline）或 FUTEX_WAKE 或信号唤醒后重查；
 * FUTEX_WAKE(1): 唤醒至多 val 个等待该地址的任务。 */
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1
#define FUTEX_PRIVATE_FLAG 0x80
static uint64_t sys_futex(uint64_t uaddr, uint64_t op, uint64_t val,
                          uint64_t timeout, uint64_t a5, uint64_t a6)
{
    (void)a5;(void)a6;
    if (!current_task || !current_task->mm_context) return (uint64_t)(-14);
    vm_context_t* mm = current_task->mm_context;
    int cmd = (int)(op & 0x7F);

    if (cmd == FUTEX_WAIT) {
        int32_t cur = 0;
        if (vm_copy_from_user(mm, &cur, uaddr, 4) < 0) return (uint64_t)(-14);
        if ((int32_t)val != cur) return (uint64_t)(-11);   /* EAGAIN */
        current_task->wait_deadline = 0;
        if (timeout) {
            uint64_t sec = 0, nsec = 0;
            if (vm_copy_from_user(mm, &sec, timeout, 8) < 0 ||
                vm_copy_from_user(mm, &nsec, timeout + 8, 8) < 0)
                return (uint64_t)(-14);
            current_task->wait_deadline =
                timer_ms() + sec * 1000 + nsec / 1000000;
        }
        for (;;) {
            sched_block_and_switch(WAIT_FUTEX, uaddr);
            /* 唤醒后重查：值已变化 → 成功 */
            if (vm_copy_from_user(mm, &cur, uaddr, 4) < 0) return (uint64_t)(-14);
            if ((int32_t)val != cur) break;
            uint64_t now = timer_ms();
            if (current_task->wait_deadline && now >= current_task->wait_deadline)
                return (uint64_t)(-110);                     /* ETIMEDOUT */
            if (current_task->sig_pending & ~current_task->sig_blocked)
                return (uint64_t)(-4);                       /* EINTR */
        }
        return 0;
    }

    if (cmd == FUTEX_WAKE) {
        int woke = 0;
        for (int i = 0; i < sched_task_count(); i++) {
            task_t* t = sched_get_task(i);
            if (!t) continue;
            if (t->state == TASK_STATE_BLOCKED && t->wait_kind == WAIT_FUTEX &&
                t->wait_arg == uaddr) {
                sched_wake(t);
                if (++woke >= (int)val) break;
            }
        }
        return (uint64_t)woke;
    }
    (void)FUTEX_PRIVATE_FLAG;
    return (uint64_t)(-38);   /* ENOSYS：其余操作暂不支持 */
}

/* ================= W7 Phase 2.2: 时间 / 同步 / 信号批 ================= */

/* timespec (16B) → ms；失败返回 0 并置 *err=-EFAULT */
static uint64_t timespec_to_ms(vm_context_t* mm, uint64_t tsp, int* err)
{
    uint64_t sec = 0, nsec = 0;
    if (vm_copy_from_user(mm, &sec, tsp, 8) < 0 ||
        vm_copy_from_user(mm, &nsec, tsp + 8, 8) < 0) {
        if (err) *err = -14;
        return 0;
    }
    return sec * 1000 + nsec / 1000000;
}

/* 35: nanosleep — 挂起至少 req 毫秒；被信号打断时写回剩余时间。 */
static uint64_t sys_nanosleep(uint64_t req, uint64_t rem,
                              uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3;(void)a4;(void)a5;(void)a6;
    if (!current_task || !current_task->mm_context) return (uint64_t)(-14);
    vm_context_t* mm = current_task->mm_context;
    int err = 0;
    uint64_t ms = timespec_to_ms(mm, req, &err);
    if (err) return (uint64_t)err;
    if (ms == 0) return 0;

    uint64_t start = timer_ms();
    uint64_t deadline = start + ms;
    sched_block_and_switch(WAIT_TIME, deadline);
    uint64_t now = timer_ms();
    int64_t left = (int64_t)deadline - (int64_t)now;
    if (left < 0) left = 0;
    if (rem && (current_task->sig_pending & ~current_task->sig_blocked)) {
        uint64_t rsec = (uint64_t)left / 1000;
        uint64_t rnsec = (uint64_t)(left % 1000) * 1000000;
        if (vm_copy_to_user(mm, rem, &rsec, 8) < 0 ||
            vm_copy_to_user(mm, rem + 8, &rnsec, 8) < 0)
            return (uint64_t)(-14);
        return (uint64_t)(-4);   /* EINTR */
    }
    return 0;
}

/* 229: clock_getres — 精度 1ms。 */
static uint64_t sys_clock_getres(uint64_t which, uint64_t tp,
                                 uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)which;(void)a3;(void)a4;(void)a5;(void)a6;
    if (!current_task || !current_task->mm_context) return (uint64_t)(-14);
    uint64_t sec = 0, nsec = 1000000;   /* 1ms */
    if (vm_copy_to_user(current_task->mm_context, tp, &sec, 8) < 0 ||
        vm_copy_to_user(current_task->mm_context, tp + 8, &nsec, 8) < 0)
        return (uint64_t)(-14);
    return 0;
}

/* 228: clock_gettime — CLOCK_REALTIME(0)/MONOTONIC(1)/MONOTONIC_RAW(4)。
 * 无 RTC，REALTIME = 固定基准 + 单调运行时间。 */
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_MONOTONIC_RAW 4
#define BOOT_EPOCH_SEC 1600000000ULL   /* 启动基准（2020-09-13） */
static uint64_t sys_clock_gettime(uint64_t which, uint64_t tp,
                                  uint64_t a3, uint64_t a4, uint64_t a5,
                                  uint64_t a6)
{
    (void)a3;(void)a4;(void)a5;(void)a6;
    if (!current_task || !current_task->mm_context) return (uint64_t)(-14);
    uint64_t ms = timer_ms();
    uint64_t sec = ms / 1000;
    uint64_t nsec = (ms % 1000) * 1000000;
    if (which == CLOCK_REALTIME) sec += BOOT_EPOCH_SEC;
    else if (which != CLOCK_MONOTONIC && which != CLOCK_MONOTONIC_RAW)
        return (uint64_t)(-22);   /* EINVAL */
    if (vm_copy_to_user(current_task->mm_context, tp, &sec, 8) < 0 ||
        vm_copy_to_user(current_task->mm_context, tp + 8, &nsec, 8) < 0)
        return (uint64_t)(-14);
    return 0;
}

/* itimerval (32B): [it_interval sec/usec] [it_value sec/usec]。 */
static void itimer_read_user(vm_context_t* mm, uint64_t p, uint64_t* iv_ms,
                             uint64_t* val_ms)
{
    uint64_t isec = 0, iusec = 0, vsec = 0, vusec = 0;
    vm_copy_from_user(mm, &isec, p, 8);
    vm_copy_from_user(mm, &iusec, p + 8, 8);
    vm_copy_from_user(mm, &vsec, p + 16, 8);
    vm_copy_from_user(mm, &vusec, p + 24, 8);
    *iv_ms  = isec * 1000 + iusec / 1000;
    *val_ms = vsec * 1000 + vusec / 1000;
}

static void itimer_write_user(vm_context_t* mm, uint64_t p, uint64_t iv_ms,
                              uint64_t val_ms)
{
    uint64_t isec = iv_ms / 1000, iusec = (iv_ms % 1000) * 1000;
    uint64_t vsec = val_ms / 1000, vusec = (val_ms % 1000) * 1000;
    vm_copy_to_user(mm, p, &isec, 8);
    vm_copy_to_user(mm, p + 8, &iusec, 8);
    vm_copy_to_user(mm, p + 16, &vsec, 8);
    vm_copy_to_user(mm, p + 24, &vusec, 8);
}

/* 36/38: getitimer / setitimer — 仅 ITIMER_REAL(0)。 */
static uint64_t sys_setitimer(uint64_t which, uint64_t newv, uint64_t oldv,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4;(void)a5;(void)a6;
    if (!current_task || !current_task->mm_context) return (uint64_t)(-14);
    vm_context_t* mm = current_task->mm_context;
    if (which != 0) return 0;   /* 仅支持 ITIMER_REAL */
    if (newv) {
        uint64_t iv_ms = 0, val_ms = 0;
        itimer_read_user(mm, newv, &iv_ms, &val_ms);
        current_task->itimer_interval = iv_ms;
        current_task->itimer_deadline = val_ms ? timer_ms() + val_ms : 0;
    }
    if (oldv) {
        uint64_t now = timer_ms();
        uint64_t rem = (current_task->itimer_deadline > now)
            ? current_task->itimer_deadline - now : 0;
        itimer_write_user(mm, oldv, current_task->itimer_interval, rem);
    }
    return 0;
}

static uint64_t sys_getitimer(uint64_t which, uint64_t value,
                              uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3;(void)a4;(void)a5;(void)a6;
    return sys_setitimer(which, 0, value, 0, 0, 0);
}

/* 37: alarm — 一次性定时（秒）。返回上次未到期 alarm 的剩余秒数。 */
static uint64_t sys_alarm(uint64_t seconds, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    if (!current_task) return 0;
    uint64_t prev = 0;
    if (current_task->itimer_deadline) {
        uint64_t now = timer_ms();
        int64_t rem = (int64_t)(current_task->itimer_deadline - now);
        if (rem > 0) prev = ((uint64_t)rem + 999) / 1000;
    }
    current_task->itimer_deadline = seconds ? timer_ms() + seconds * 1000 : 0;
    current_task->itimer_interval = 0;
    return prev;
}

/* 131: sigaltstack — 设/取备用信号栈（struct stack_t 24B：ss_sp(8) ss_flags(4)+pad(4) ss_size(8)）。 */
#define SS_DISABLE 2
static uint64_t sys_sigaltstack(uint64_t ss, uint64_t old_ss,
                                uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3;(void)a4;(void)a5;(void)a6;
    if (!current_task || !current_task->mm_context) return (uint64_t)(-14);
    vm_context_t* mm = current_task->mm_context;
    if (old_ss) {
        uint64_t v[3];
        v[0] = current_task->altstack_base;
        v[1] = current_task->altstack_onstack ? 1 : 0;
        v[2] = current_task->altstack_size;
        if (vm_copy_to_user(mm, old_ss, v, 24) < 0) return (uint64_t)(-14);
    }
    if (ss) {
        uint64_t v[3];
        if (vm_copy_from_user(mm, v, ss, 24) < 0) return (uint64_t)(-14);
        if (v[1] & SS_DISABLE) {
            current_task->altstack_base = 0;
            current_task->altstack_size = 0;
        } else {
            current_task->altstack_base = v[0];
            current_task->altstack_size = v[2];
        }
    }
    return 0;
}

/* 128: rt_sigtimedwait — 等待 set 中的信号（带超时）。 */
static uint64_t sys_rt_sigtimedwait(uint64_t set, uint64_t info,
                                    uint64_t timeout, uint64_t a4,
                                    uint64_t a5, uint64_t a6)
{
    (void)a4;(void)a5;(void)a6;
    (void)info;
    if (!current_task || !current_task->mm_context) return (uint64_t)(-14);
    vm_context_t* mm = current_task->mm_context;
    uint64_t wanted = 0;
    if (vm_copy_from_user(mm, &wanted, set, 8) < 0) return (uint64_t)(-14);
    wanted &= ~((1ULL << 9) | (1ULL << 19));   /* SIGKILL/SIGSTOP 不可等 */
    wanted &= ~current_task->sig_blocked;      /* 等待集在等待期间视为未阻塞 */

    uint64_t deadline = ~0ULL;
    if (timeout) {
        int err = 0;
        uint64_t ms = timespec_to_ms(mm, timeout, &err);
        if (err) return (uint64_t)err;
        deadline = timer_ms() + ms;
    }

    for (;;) {
        uint64_t m = current_task->sig_pending & wanted;
        if (m) {
            int sig = 0;
            for (int i = 1; i < 64; i++)
                if (m & (1ULL << i)) { sig = i; break; }
            current_task->sig_pending &= ~(1ULL << sig);
            return (uint64_t)sig;
        }
        uint64_t now = timer_ms();
        if (now >= deadline) return (uint64_t)(-11);   /* EAGAIN：超时无信号 */
        /* 有其它未决信号（不在等待集）→ 中断（近似 EINTR） */
        if (current_task->sig_pending & ~current_task->sig_blocked)
            return (uint64_t)(-4);                     /* EINTR */
        sched_block_and_switch(WAIT_SIGTIMED, deadline);
    }
}

/* 58: vfork — 路由到 fork（协作式下无写时复制差异）。 */
static uint64_t sys_vfork(uint64_t a1, uint64_t a2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    return sys_fork(0, 0, 0, 0, 0, 0);
}

/* 157: prctl — PR_SET_NAME(15) 写进程名；其余返回 0。 */
static uint64_t sys_prctl(uint64_t option, uint64_t arg2, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3;(void)a4;(void)a5;(void)a6;
    if (!current_task || !current_task->mm_context) return (uint64_t)(-14);
    if (option == 15) {   /* PR_SET_NAME */
        char name[17];
        if (exec_strncpy_from_user(current_task->mm_context, name, arg2, 16) < 0)
            return (uint64_t)(-14);
        name[16] = '\0';
        int i = 0;
        while (name[i] && i < 31) {
            current_task->task_name[i] = name[i];
            i++;
        }
        current_task->task_name[i] = '\0';
    }
    return 0;
}

/* 28: madvise — no-op（gcc/python 惯例调用）。 */
static uint64_t sys_madvise(uint64_t a1, uint64_t a2, uint64_t a3,
                            uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6; return 0; }

/* 140/141: getpriority / setpriority — no-op。 */
static uint64_t sys_getpriority(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6; return 0; }
static uint64_t sys_setpriority(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6)
{ (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6; return 0; }

/* 218: set_tid_address */
static uint64_t sys_set_tid_address(uint64_t a1, uint64_t a2, uint64_t a3,
                                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1;(void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    if (current_task) return current_task->pid;
    return 0;
}

/* 257: openat — AT_FDCWD 或具体目录 fd 上的相对/绝对路径打开。
 * NOTE: glibc passes int args zero-extended in 32-bit registers (e.g. dirfd
 * arrives as 0x00000000ffffff9c), so dirfd MUST be cast to int32 before the
 * AT_FDCWD comparison, otherwise the sign-extension mismatch returns ENOSYS. */
#define AT_FDCWD (-100)
static uint64_t sys_openat(uint64_t dirfd, uint64_t path, uint64_t flags,
                           uint64_t mode, uint64_t a5, uint64_t a6)
{
    (void)a5;(void)a6;
    if (!current_task || !current_task->mm_context)
        return (uint64_t)(-14);  /* EFAULT */

    char pathbuf[96];
    if (exec_strncpy_from_user(current_task->mm_context, pathbuf, path,
                               sizeof(pathbuf) - 1) < 0)
        return (uint64_t)(-14);  /* EFAULT */

    /* 相对路径的基准目录：AT_FDCWD → cwd；具体 dirfd → 该 fd 记录路径。 */
    const char* base = current_task->cwd[0] ? current_task->cwd : "/";
    char basebuf[96];
    if ((int)dirfd != AT_FDCWD) {
        file_t* dirf = vfs_fd_get((int)dirfd);
        if (!dirf) return (uint64_t)(-9);   /* EBADF */
        if (!dirf->name[0]) return (uint64_t)(-20);  /* ENOTDIR */
        if (dirf->name[0] == '/') {
            base = dirf->name;
        } else {
            cwd_resolve(base, dirf->name, basebuf, sizeof(basebuf));
            base = basebuf;
        }
    }

    char resolved[96];
    const char* p = pathbuf;
    if (pathbuf[0] != '/')
        cwd_resolve(base, pathbuf, resolved, sizeof(resolved)), p = resolved;

    file_t* f = vfs_open_mode(p, flags, mode);
    if (!f) {
        if (vfs_open_errno == -13) return (uint64_t)(-13);  /* EACCES */
        return (uint64_t)(-2);  /* ENOENT */
    }
    int fd = vfs_fd_alloc(f);
    if (fd < 0) return (uint64_t)(-24);  /* EMFILE */
    return (uint64_t)fd;
}

/* 87/263: unlink / unlinkat — 拆分绝对路径为父目录 + 文件名，调 ext2_unlink。 */
static int unlink_path(const char* p)
{
    char parent[256];
    char name[256];
    const char* slash = 0;
    for (const char* q = p; *q; q++) if (*q == '/') slash = q;
    if (!slash) return -2;   /* ENOENT（相对路径不支持） */
    int plen = (int)(slash - p);
    if (plen == 0) {
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        if (plen >= (int)sizeof(parent)) return -36;   /* ENAMETOOLONG */
        for (int i = 0; i < plen; i++) parent[i] = p[i];
        parent[plen] = '\0';
    }
    int nlen = 0;
    for (const char* q = slash + 1; *q && nlen < 255; q++) name[nlen++] = *q;
    name[nlen] = '\0';
    if (nlen == 0) return -2;   /* ENOENT（尾部斜杠） */

    uint32_t parent_ino;
    if (ext2_lookup_path(parent, &parent_ino) < 0) return -2;
    if (ext2_unlink(parent_ino, name) < 0) return -2;
    return 0;
}

static uint64_t sys_unlink(uint64_t path, uint64_t a2, uint64_t a3,
                           uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    vm_context_t* mm = current_task ? current_task->mm_context : 0;
    if (!mm) return (uint64_t)(-14);   /* EFAULT */
    char pathbuf[64];
    if (exec_strncpy_from_user(mm, pathbuf, path, sizeof(pathbuf) - 1) < 0)
        return (uint64_t)(-14);
    int r = unlink_path(pathbuf);
    return r < 0 ? (uint64_t)r : 0;
}

static uint64_t sys_unlinkat(uint64_t dirfd, uint64_t path, uint64_t flags,
                             uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)flags;(void)a4;(void)a5;(void)a6;
    if ((int)dirfd != AT_FDCWD) return (uint64_t)(-38);   /* ENOSYS */
    return sys_unlink(path, 0, 0, 0, 0, 0);
}

/* 17: pread64 — read from fd at a fixed offset without moving the offset. */
static uint64_t sys_pread64(uint64_t fd, uint64_t buf, uint64_t count,
                            uint64_t offset, uint64_t a5, uint64_t a6)
{
    (void)a5;(void)a6;
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);  /* EBADF */
    uint64_t saved = f->offset;
    f->offset = offset;
    uint64_t r = vfs_read(f, (void*)buf, count);
    f->offset = saved;
    return r;
}

/* 20: writev — write a scatter-gather iovec array to fd. */
static uint64_t sys_writev(uint64_t fd, uint64_t iov, uint64_t iovcnt,
                            uint64_t a4, uint64_t a5, uint64_t a6)
  {
    (void)a4;(void)a5;(void)a6;
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);  /* EBADF */
    vm_context_t* mm = current_task ? current_task->mm_context : 0;
    if (!mm) return (uint64_t)(-14);  /* EFAULT */
    uint64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        uint64_t base = 0, len = 0;
        if (vm_copy_from_user(mm, &base, iov + i * 16, 8) < 0 ||
            vm_copy_from_user(mm, &len,  iov + i * 16 + 8, 8) < 0)
            return (uint64_t)(-14);  /* EFAULT */
        while (len > 0) {
            uint64_t chunk = (len > 512) ? 512 : len;
            uint8_t tmp[512];
            if (vm_copy_from_user(mm, tmp, base, chunk) < 0)
                return (uint64_t)(-14);  /* EFAULT */
            uint64_t w = vfs_write(f, tmp, chunk);
            if (w != chunk) return total + w;
            total += chunk; base += chunk; len -= chunk;
        }
    }
    return total;
}

/* ================= W7 Phase 2.1: 管道 / I/O 批 ================= */

/* 22: pipe — 创建管道：fds[0]=读端 fds[1]=写端。 */
static uint64_t sys_pipe(uint64_t pipefd, uint64_t a2, uint64_t a3,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a2;(void)a3;(void)a4;(void)a5;(void)a6;
    vm_context_t* mm = current_task ? current_task->mm_context : 0;
    if (!mm || !pipefd) return (uint64_t)(-14);   /* EFAULT */
    int fds[2];
    if (pipe_create_fds(fds, 0) < 0) return (uint64_t)(-1);  /* EPERM：池满 */
    if (vm_copy_to_user(mm, pipefd, fds, 8) < 0) return (uint64_t)(-14);
    return 0;
}

/* 293: pipe2 — pipe + flags（O_NONBLOCK / O_CLOEXEC，O_CLOEXEC 不追踪）。 */
static uint64_t sys_pipe2(uint64_t pipefd, uint64_t flags, uint64_t a3,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a3;(void)a4;(void)a5;(void)a6;
    vm_context_t* mm = current_task ? current_task->mm_context : 0;
    if (!mm || !pipefd) return (uint64_t)(-14);   /* EFAULT */
    int fds[2];
    if (pipe_create_fds(fds, flags & O_NONBLOCK) < 0) return (uint64_t)(-1);
    if (vm_copy_to_user(mm, pipefd, fds, 8) < 0) return (uint64_t)(-14);
    return 0;
}

/* 19: readv — 从 fd 顺序读入 iovec 数组（每 iovec: base(8) len(8)）。 */
static uint64_t sys_readv(uint64_t fd, uint64_t iov, uint64_t iovcnt,
                          uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4;(void)a5;(void)a6;
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);   /* EBADF */
    vm_context_t* mm = current_task ? current_task->mm_context : 0;
    if (!mm) return (uint64_t)(-14);   /* EFAULT */
    uint64_t total = 0;
    for (uint64_t i = 0; i < iovcnt; i++) {
        uint64_t base = 0, len = 0;
        if (vm_copy_from_user(mm, &base, iov + i * 16, 8) < 0 ||
            vm_copy_from_user(mm, &len,  iov + i * 16 + 8, 8) < 0)
            return (uint64_t)(-14);   /* EFAULT */
        if (len == 0) continue;
        uint64_t r = vfs_read(f, (void*)base, len);
        if ((int64_t)r < 0) return (uint64_t)(int64_t)r;
        total += r;
        if (r < len) break;   /* EOF / 管道数据已尽 */
    }
    return total;
}

/* 217: getdents64 — 按 fd 偏移读取目录项（linux_dirent64）。 */
static uint64_t sys_getdents64(uint64_t fd, uint64_t dirp, uint64_t count,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4;(void)a5;(void)a6;
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);   /* EBADF */
    if (f->f_type != EXT2_S_IFDIR) return (uint64_t)(-20);  /* ENOTDIR */
    uint32_t ino = (uint32_t)(uintptr_t)f->private_data;
    uint64_t next_off = 0;
    int r = ext2_getdents(ino, f->offset, (void*)dirp, count, &next_off);
    if (r < 0) return (uint64_t)(-21);   /* EISDIR */
    f->offset = next_off;   /* 下次调用续读 */
    return (uint64_t)r;
}

/* ---- 7: poll — 多 fd 就绪检查 + 协作式阻塞 ---- */
#define POLLIN   0x001
#define POLLPRI  0x002
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

/* 单 fd 就绪检查。返回 revents；fd 无效返回 POLLNVAL。 */
static short poll_check_one(file_t* f, short events)
{
    short rev = 0;
    if (!f) return POLLNVAL;

    if (pipe_is_pipe(f)) {
        if (f->f_type == 1) {            /* 读端 */
            if ((events & POLLIN) && pipe_readable(f)) rev |= POLLIN;
            if (pipe_writers_gone(f)) rev |= POLLHUP;   /* EOF 可读 */
        } else {                         /* 写端 */
            if ((events & POLLOUT) && pipe_writable(f)) rev |= POLLOUT;
            if (pipe_readers_gone(f)) rev |= POLLERR | POLLHUP;
        }
        return rev;
    }
    if (devfs_is_tty(f)) {
        if ((events & POLLIN) && devfs_tty_readable()) rev |= POLLIN;
        if (events & POLLOUT) rev |= POLLOUT;
        return rev;
    }
    /* 普通文件 / 其它设备：读写恒就绪 */
    if (events & POLLIN) rev |= POLLIN;
    if (events & POLLOUT) rev |= POLLOUT;
    return rev;
}

static uint64_t sys_poll(uint64_t fds, uint64_t nfds, uint64_t timeout_ms,
                         uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a4;(void)a5;(void)a6;
    vm_context_t* mm = current_task ? current_task->mm_context : 0;
    if (!mm) return (uint64_t)(-14);
    if (fds == 0 && nfds != 0) return (uint64_t)(-14);

    uint64_t deadline;
    if ((int64_t)timeout_ms < 0) deadline = ~0ULL;          /* 无限阻塞 */
    else if (timeout_ms == 0) deadline = 0;                 /* 非阻塞 */
    else deadline = timer_ms() + timeout_ms;

    for (;;) {
        int ready = 0;
        for (uint64_t i = 0; i < nfds; i++) {
            uint64_t base = fds + i * 8;                    /* struct pollfd = 8B */
            int32_t fd = 0;
            short events = 0, rev = 0;
            if (vm_copy_from_user(mm, &fd, base, 4) < 0 ||
                vm_copy_from_user(mm, &events, base + 4, 2) < 0)
                return (uint64_t)(-14);
            file_t* f = vfs_fd_get((int)fd);
            rev = poll_check_one(f, (short)events);
            if (vm_copy_to_user(mm, base + 6, &rev, 2) < 0)
                return (uint64_t)(-14);
            if (rev) ready++;
        }
        if (ready) return (uint64_t)ready;
        if (deadline == 0) return 0;                        /* 非阻塞：无就绪 */
        uint64_t now = timer_ms();
        if (now >= deadline) return 0;                      /* 超时 */
        if (current_task &&
            (current_task->sig_pending & ~current_task->sig_blocked))
            return (uint64_t)(-4);                          /* EINTR */
        /* 阻塞到 deadline（fd 状态变化 / 超时 / 信号唤醒后重扫） */
        sched_block_and_switch(WAIT_SELECT, deadline);
    }
}

/* 262: newfstatat — glibc stat()/fstatat() on x86-64.
 * 支持两种 dirfd 语义：
 *   - AT_FDCWD：从根解析绝对路径（单根，等价于 stat）
 *   - 有效 fd + 空路径（AT_EMPTY_PATH 风格，glibc open_verify 用 __fstatat
 *     检查已打开的文件）：等价于 fstat(fd) */
static uint64_t sys_newfstatat(uint64_t dirfd, uint64_t path, uint64_t buf,
                               uint64_t flags, uint64_t a5, uint64_t a6)
{
    (void)flags;(void)a5;(void)a6;
    const char* p = (const char*)path;
    if (!p || p[0] == '\0') {
        return sys_fstat(dirfd, buf, 0, 0, 0, 0);   /* AT_EMPTY_PATH: fstat(fd) */
    }
    if ((int)dirfd != AT_FDCWD) return (uint64_t)(-38);   /* ENOSYS */
    return sys_stat(path, buf, 0, 0, 0, 0);
}

/* 269/439: faccessat / faccessat2 — like access, but with a dirfd. */
static uint64_t sys_faccessat(uint64_t dirfd, uint64_t path, uint64_t mode,
                              uint64_t flags, uint64_t a5, uint64_t a6)
{
    (void)dirfd;(void)flags;(void)a5;(void)a6;
    return sys_access(path, mode, 0, 0, 0, 0);
}
static uint64_t sys_faccessat2(uint64_t a1, uint64_t path, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)a1;(void)a3;(void)a4;(void)a5;(void)a6;
    return sys_access(path, a3, 0, 0, 0, 0);
}

/* 318: getrandom — pseudo-random bytes derived from the TSC. */
static uint64_t sys_getrandom(uint64_t buf, uint64_t count, uint64_t flags,
                              uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)flags;(void)a4;(void)a5;(void)a6;
    uint8_t* p = (uint8_t*)buf;
    uint64_t t = rdtsc();
    for (uint64_t i = 0; i < count; i++) {
        p[i] = (uint8_t)(t >> ((i & 7) * 8));
    }
    return count;
}

/* ---- Table registration ---- */

/* W7 (Task 2.4): 解析 /etc/passwd 的 "root:x:UID:GID" 行，记录 root 用户
 * uid/gid（默认 0）。文件不存在时保持默认（uid 0 = root）。
 * kmain 在 fs 挂载后调用。 */
uint32_t g_root_uid = 0;
uint32_t g_root_gid = 0;

/* 简单十进制解析 */
static uint32_t parse_dec(const char* s, const char** endp)
{
    uint32_t v = 0;
    while (*s >= '0' && *s <= '9') { v = v * 10 + (uint32_t)(*s - '0'); s++; }
    if (endp) *endp = s;
    return v;
}

/* 在行内按 ':' 切字段；返回第 idx 个字段起始（0 起），越界返回 0 */
static const char* field_at(const char* line, const char* nl, int idx)
{
    const char* p = line;
    int cur = 0;
    while (p < nl) {
        if (cur == idx) return p;
        while (p < nl && *p != ':') p++;
        if (p >= nl) return 0;
        p++;
        cur++;
    }
    return 0;
}

void users_init(void)
{
    g_root_uid = 0;
    g_root_gid = 0;
    g_default_uid = 0;
    g_default_gid = 0;

    file_t* f = vfs_open("/etc/passwd", 0);
    if (!f) return;                       /* 文件不存在：uid 默认 0 */

    char buf[512];
    uint64_t n = vfs_read(f, buf, sizeof(buf) - 1);
    vfs_close(f);
    buf[n] = '\0';

    /* 逐行匹配 "root:" 与 "banana:" 前缀 */
    const char* line = buf;
    const char* end = buf + n;
    while (line < end) {
        const char* nl = line;
        while (nl < end && *nl != '\n') nl++;

        /* 字段: name(0) passwd(1) uid(2) gid(3) ... */
        const char* uidf = field_at(line, nl, 2);
        const char* gidf = field_at(line, nl, 3);
        if (uidf && gidf) {
            if (nl - line >= 5 && line[0] == 'r' && line[1] == 'o' &&
                line[2] == 'o' && line[3] == 't' && line[4] == ':') {
                g_root_uid = parse_dec(uidf, 0);
                g_root_gid = parse_dec(gidf, 0);
            } else if (nl - line >= 7 && line[0] == 'b' && line[1] == 'a' &&
                       line[2] == 'n' && line[3] == 'a' && line[4] == 'n' &&
                       line[5] == 'a' && line[6] == ':') {
                g_default_uid = parse_dec(uidf, 0);
                g_default_gid = parse_dec(gidf, 0);
            }
        }
        line = nl + 1;
    }
}

/* 23/270: select / pselect6 — bash's readline waits on stdin (fd 0) with
 * pselect6 before reading a command. Without it, ENOSYS makes readline
 * treat the tty as failed and bash exits right after printing the prompt.
 * Minimal implementation: block until fd 0 has a key, then mark it ready. */
static uint64_t sys_select_common(uint64_t nfds, uint64_t readfds,
                                  uint64_t writefds, uint64_t exceptfds)
{
    (void)nfds; (void)writefds; (void)exceptfds;
    /* 忙等改为阻塞（WAIT_KBD）：select 让出 CPU 由主循环轮询键盘，
     * 与 tty_read 的 tty_getc 同一唤醒路径。忙等会独占 BSP，导致
     * 主循环定时器/调度停摆，且 QEMU 串口注入在忙等下时序不稳。 */
    for (;;) {
        if (kbd_has_key()) break;
        sched_block_and_switch(WAIT_KBD, 0);
    }
    if (readfds) {
        vm_context_t* mm = current_task ? current_task->mm_context : 0;
        if (mm) {
            uint8_t ready = 0x01;   /* bit 0 = fd 0 */
            vm_copy_to_user(mm, readfds, &ready, 1);
        }
    }
    return 1;   /* one fd ready */
}

static uint64_t sys_select(uint64_t nfds, uint64_t readfds, uint64_t writefds,
                           uint64_t exceptfds, uint64_t timeout, uint64_t a6)
{
    (void)timeout;(void)a6;
    return sys_select_common(nfds, readfds, writefds, exceptfds);
}

static uint64_t sys_pselect6(uint64_t nfds, uint64_t readfds, uint64_t writefds,
                             uint64_t exceptfds, uint64_t timeout, uint64_t sigmask)
{
    (void)timeout;(void)sigmask;
    return sys_select_common(nfds, readfds, writefds, exceptfds);
}

void syscall_table_init(void)
{
    syscall_register(0,  sys_read);
    syscall_register(1,  sys_write);
    syscall_register(2,  sys_open);
    syscall_register(3,  sys_close);
    syscall_register(4,  sys_stat);
    syscall_register(5,  sys_fstat);
    syscall_register(6,  sys_lstat);
    syscall_register(7,  sys_poll);
    syscall_register(8,  sys_lseek);
    syscall_register(9,  sys_mmap);
    syscall_register(10, sys_mprotect);
    syscall_register(11, sys_munmap);
    syscall_register(12, sys_brk);
    syscall_register(13, sys_rt_sigaction);
    syscall_register(14, sys_rt_sigprocmask);
    syscall_register(15, sys_rt_sigreturn);
    syscall_register(16, sys_ioctl);
    syscall_register(17, sys_pread64);
    syscall_register(19, sys_readv);
    syscall_register(20, sys_writev);
    syscall_register(21, sys_access);
    syscall_register(22, sys_pipe);
    syscall_register(23, sys_select);
    syscall_register(24, sys_sched_yield);
    syscall_register(28, sys_madvise);
    syscall_register(32, sys_dup);
    syscall_register(33, sys_dup2);
    syscall_register(35, sys_nanosleep);
    syscall_register(36, sys_getitimer);
    syscall_register(37, sys_alarm);
    syscall_register(38, sys_setitimer);
    syscall_register(39, sys_getpid);
    syscall_register(56, sys_clone);
    syscall_register(57, sys_fork);
    syscall_register(58, sys_vfork);
    syscall_register(59, sys_execve);
    syscall_register(60, sys_exit);
    syscall_register(61, sys_wait4);
    syscall_register(62, sys_kill);
    syscall_register(63, sys_uname);
    syscall_register(72, sys_fcntl);
    syscall_register(76, sys_truncate);
    syscall_register(77, sys_ftruncate);
    syscall_register(79, sys_getcwd);
    syscall_register(80, sys_chdir);
    syscall_register(81, sys_fchdir);
    syscall_register(82, sys_rename);
    syscall_register(83, sys_mkdir);
    syscall_register(84, sys_rmdir);
    syscall_register(87, sys_unlink);
    syscall_register(89, sys_readlink);
    syscall_register(90, sys_chmod);
    syscall_register(95, sys_umask);
    syscall_register(96, sys_gettimeofday);
    syscall_register(97, sys_getrlimit);
    syscall_register(98, sys_getrusage);
    syscall_register(99, sys_sysinfo);
    syscall_register(102, sys_getuid);
    syscall_register(104, sys_getgid);
    syscall_register(105, sys_setuid);
    syscall_register(106, sys_setgid);
    syscall_register(107, sys_geteuid);
    syscall_register(108, sys_getegid);
    syscall_register(109, sys_setpgid);
    syscall_register(110, sys_getppid);
    syscall_register(111, sys_getpgrp);       /* x86-64 111 = getpgrp */
    syscall_register(112, sys_setsid);
    syscall_register(121, sys_getpgid);
    syscall_register(127, sys_rt_sigpending); /* x86-64 127 = rt_sigpending */
    syscall_register(128, sys_rt_sigtimedwait);
    syscall_register(131, sys_sigaltstack);
    syscall_register(137, sys_statfs);
    syscall_register(138, sys_fstatfs);
    syscall_register(140, sys_getpriority);
    syscall_register(141, sys_setpriority);
    syscall_register(157, sys_prctl);
    syscall_register(158, sys_arch_prctl);
    syscall_register(160, sys_setrlimit);     /* x86-64 160 = setrlimit */
    syscall_register(201, sys_time);
    syscall_register(202, sys_futex);
    syscall_register(217, sys_getdents64);
    syscall_register(218, sys_set_tid_address);
    syscall_register(228, sys_clock_gettime);
    syscall_register(229, sys_clock_getres);  /* x86-64 229 = clock_getres */
    syscall_register(231, sys_exit_group);
    syscall_register(257, sys_openat);
    syscall_register(262, sys_newfstatat);
    syscall_register(263, sys_unlinkat);
    syscall_register(268, sys_fchmodat);
    syscall_register(269, sys_faccessat);
    syscall_register(270, sys_pselect6);
    syscall_register(273, sys_mlock2);        /* 273=set_robust_list: 0-stub 兼容 */
    syscall_register(293, sys_pipe2);
    syscall_register(302, sys_prlimit64);
    syscall_register(318, sys_getrandom);
    syscall_register(325, sys_mlock2);        /* x86-64 325 = mlock2 */
    syscall_register(334, sys_rseq);
    syscall_register(439, sys_faccessat2);

    /* [ANCHOR-SHM] append shm syscall registrations below (Wave 1, Agent B) */
    extern uint64_t sys_shmget(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6);
    extern uint64_t sys_shmat(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6);
    extern uint64_t sys_shmctl(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6);
    extern uint64_t sys_shmdt(uint64_t a1, uint64_t a2, uint64_t a3,
                              uint64_t a4, uint64_t a5, uint64_t a6);
    syscall_register(29, sys_shmget);
    syscall_register(30, sys_shmat);
    syscall_register(31, sys_shmctl);
    syscall_register(67, sys_shmdt);
    /* [ANCHOR-NET] append net syscall registrations below (Wave 1, Agent C) */
    /* ---- Task 3.3: loopback TCP (net/loopback.c) ---- */
    extern uint64_t sys_socket(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6);
    extern uint64_t sys_connect(uint64_t a1, uint64_t a2, uint64_t a3,
                                uint64_t a4, uint64_t a5, uint64_t a6);
    extern uint64_t sys_accept(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6);
    extern uint64_t sys_sendto(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6);
    extern uint64_t sys_recvfrom(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6);
    extern uint64_t sys_shutdown(uint64_t a1, uint64_t a2, uint64_t a3,
                                 uint64_t a4, uint64_t a5, uint64_t a6);
    extern uint64_t sys_bind(uint64_t a1, uint64_t a2, uint64_t a3,
                             uint64_t a4, uint64_t a5, uint64_t a6);
    extern uint64_t sys_listen(uint64_t a1, uint64_t a2, uint64_t a3,
                               uint64_t a4, uint64_t a5, uint64_t a6);
    syscall_register(41, sys_socket);
    syscall_register(42, sys_connect);
    syscall_register(43, sys_accept);
    syscall_register(44, sys_sendto);
    syscall_register(45, sys_recvfrom);
    syscall_register(48, sys_shutdown);
    syscall_register(49, sys_bind);
    syscall_register(50, sys_listen);
}