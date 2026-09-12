/* pipe.c — 内核管道（环形缓冲 + 协作式阻塞读写）。
 *
 * 设计：8 个管道池，每管道 32KB 环形缓冲（pmalloc_contig 动态分配，避免
 * 把缓冲放进 .bss 撑大 kernel.flat）。读写端各一个 file_t（独立 fd）。
 * 阻塞语义用协作调度器：读空/写满时 sched_block_and_switch(WAIT_PIPE_*),
 * 对端推进时 sched_wake_cond 唤醒。O_NONBLOCK 时直接返回 -EAGAIN。
 */
#include "pipe.h"
#include "mm.h"
#include "sched.h"
#include "keyboard.h"

#define MAX_PIPES 8
#define PIPE_BUFSZ 32768

typedef struct pipe {
    uint32_t id;
    uint8_t* buf;          /* 环形缓冲（pmalloc_contig 32KB） */
    uint32_t head;         /* 读位置 */
    uint32_t tail;         /* 写位置 */
    uint32_t count;        /* 有效字节数 */
    int readers;           /* 读端引用数 */
    int writers;           /* 写端引用数 */
    int in_use;
} pipe_t;

static pipe_t pipes[MAX_PIPES];
static file_t pipe_file_pool[MAX_PIPES * 2];
static uint8_t pipe_file_used[MAX_PIPES * 2];

static pipe_t* pipe_find(uint32_t id)
{
    if (id >= MAX_PIPES || !pipes[id].in_use) return 0;
    return &pipes[id];
}

static pipe_t* pipe_alloc(void)
{
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!pipes[i].in_use) {
            pipes[i].id = (uint32_t)i;
            pipes[i].buf = (uint8_t*)pmalloc_contig(PIPE_BUFSZ / PAGE_SIZE);
            if (!pipes[i].buf) return 0;
            pipes[i].head = 0;
            pipes[i].tail = 0;
            pipes[i].count = 0;
            pipes[i].readers = 0;
            pipes[i].writers = 0;
            pipes[i].in_use = 1;
            return &pipes[i];
        }
    }
    return 0;
}

static void pipe_free(pipe_t* p)
{
    if (!p) return;
    pfree_contig(p->buf, PIPE_BUFSZ / PAGE_SIZE);
    p->in_use = 0;
}

static uint64_t pipe_read_op(void* f, uint64_t off, void* buf, uint64_t size)
{
    (void)off;
    file_t* file = (file_t*)f;
    pipe_t* p = pipe_find((uint32_t)(uintptr_t)file->private_data);
    if (!p) return (uint64_t)(-9);   /* EBADF */
    if (size == 0) return 0;

    uint8_t* dst = (uint8_t*)buf;
    uint64_t total = 0;
    while (total < size) {
        if (p->count > 0) {
            uint64_t n = size - total;
            if (n > p->count) n = p->count;
            /* 环形拷贝（≤2 段，rep movsb 加速） */
            uint64_t head = p->head;
            uint64_t first = PIPE_BUFSZ - head;
            if (first > n) first = n;
            memcpy(dst + total, p->buf + head, first);
            if (n > first)
                memcpy(dst + total + first, p->buf, n - first);
            p->head = (p->head + (uint32_t)n) % PIPE_BUFSZ;
            p->count -= (uint32_t)n;
            total += n;
            /* 有空间了：唤醒写端 */
            sched_wake_cond(WAIT_PIPE_WR, p->id);
            sched_wake_select_all();
            if (total == size) break;
        }
        /* 缓冲空：写端全关闭 → EOF */
        if (p->writers == 0) break;
        if (file->flags & O_NONBLOCK) {
            if (total == 0) return (uint64_t)(-11);  /* EAGAIN */
            break;
        }
        sched_block_and_switch(WAIT_PIPE_RD, p->id);
        /* 信号打断检查：有未决信号则返回已读字节 */
        if (current_task && (current_task->sig_pending & ~current_task->sig_blocked)) {
            if (total == 0) return (uint64_t)(-4);   /* EINTR */
            break;
        }
    }
    return total;
}

static uint64_t pipe_write_op(void* f, uint64_t off, const void* buf, uint64_t size)
{
    (void)off;
    file_t* file = (file_t*)f;
    pipe_t* p = pipe_find((uint32_t)(uintptr_t)file->private_data);
    if (!p) return (uint64_t)(-9);
    if (p->readers == 0) return (uint64_t)(-32);   /* EPIPE */
    if (size == 0) return 0;

    const uint8_t* src = (const uint8_t*)buf;
    uint64_t total = 0;
    while (total < size) {
        if (p->count < PIPE_BUFSZ) {
            uint64_t n = size - total;
            uint64_t space = PIPE_BUFSZ - p->count;
            if (n > space) n = space;
            /* 环形写入（≤2 段，rep movsb 加速） */
            uint64_t tail = p->tail;
            uint64_t first = PIPE_BUFSZ - tail;
            if (first > n) first = n;
            memcpy(p->buf + tail, src + total, first);
            if (n > first)
                memcpy(p->buf, src + total + first, n - first);
            p->tail = (p->tail + (uint32_t)n) % PIPE_BUFSZ;
            p->count += (uint32_t)n;
            total += n;
            /* 有数据了：唤醒读端 */
            sched_wake_cond(WAIT_PIPE_RD, p->id);
            sched_wake_select_all();
            if (total == size) break;
        }
        if (p->readers == 0) return (uint64_t)(-32);   /* EPIPE */
        if (file->flags & O_NONBLOCK) {
            if (total == 0) return (uint64_t)(-11);   /* EAGAIN */
            break;
        }
        sched_block_and_switch(WAIT_PIPE_WR, p->id);
        if (current_task && (current_task->sig_pending & ~current_task->sig_blocked)) {
            if (total == 0) return (uint64_t)(-4);    /* EINTR */
            break;
        }
    }
    return total;
}

static uint64_t pipe_close_op(void* f)
{
    file_t* file = (file_t*)f;
    pipe_t* p = pipe_find((uint32_t)(uintptr_t)file->private_data);
    if (!p) return 0;
    int is_read_end = (file->f_type == 1);   /* 读端标记 */
    if (is_read_end) {
        if (p->readers > 0) p->readers--;
    } else {
        if (p->writers > 0) p->writers--;
    }
    /* 两端全关：释放管道并回收两个 file 槽（否则池 16 槽耗尽后无法新建管道） */
    if (p->readers == 0 && p->writers == 0) {
        for (int i = 0; i < MAX_PIPES * 2; i++) {
            if (pipe_file_used[i] &&
                (uintptr_t)pipe_file_pool[i].private_data == (uintptr_t)p->id)
                pipe_file_used[i] = 0;
        }
        pipe_free(p);
    } else {
        /* 一端关闭：唤醒对端处理 EOF/EAGAIN */
        sched_wake_cond(WAIT_PIPE_RD, p->id);
        sched_wake_cond(WAIT_PIPE_WR, p->id);
        sched_wake_select_all();
    }
    return 0;
}

static file_ops_t pipe_read_ops = {
    pipe_read_op, 0, 0, pipe_close_op, 0, 0
};
static file_ops_t pipe_write_ops = {
    0, pipe_write_op, 0, pipe_close_op, 0, 0
};

/* 分配一个管道 file_t（0 = 读端，1 = 写端）。 */
static file_t* pipe_alloc_file(pipe_t* p, int is_read_end)
{
    for (int i = 0; i < MAX_PIPES * 2; i++) {
        if (!pipe_file_used[i]) {
            pipe_file_used[i] = 1;
            file_t* f = &pipe_file_pool[i];
            f->inode = 0;
            f->size = PIPE_BUFSZ;
            f->offset = 0;
            f->ops = is_read_end ? &pipe_read_ops : &pipe_write_ops;
            f->private_data = (void*)(uintptr_t)p->id;
            f->ref_count = 1;
            f->name[0] = '\0';
            f->flags = 0;
            f->f_type = is_read_end ? 1 : 2;
            return f;
        }
    }
    return 0;
}

/* 创建管道：fds[0]=读端，fds[1]=写端。flags: O_NONBLOCK 等。 */
int pipe_create_fds(int fds[2], uint64_t flags)
{
    pipe_t* p = pipe_alloc();
    if (!p) return -1;
    file_t* rd = pipe_alloc_file(p, 1);
    file_t* wr = pipe_alloc_file(p, 0);
    if (!rd || !wr) {
        if (rd) { /* 释放占用的 file 槽 */
            int idx = (int)(rd - pipe_file_pool);
            if (idx >= 0 && idx < MAX_PIPES * 2) pipe_file_used[idx] = 0;
        }
        pipe_free(p);
        return -1;
    }
    rd->flags = flags;
    wr->flags = flags;
    p->readers = 1;
    p->writers = 1;

    int rfd = vfs_fd_alloc(rd);
    int wfd = vfs_fd_alloc(wr);
    if (rfd < 0 || wfd < 0) {
        if (rfd >= 0) vfs_fd_free(rfd);
        if (wfd >= 0) vfs_fd_free(wfd);
        return -1;
    }
    fds[0] = rfd;
    fds[1] = wfd;
    return 0;
}

/* 管道就绪判断（poll/select 用）。 */
int pipe_readable(file_t* f)
{
    pipe_t* p = pipe_find((uint32_t)(uintptr_t)f->private_data);
    if (!p) return 0;
    return (p->count > 0) || (p->writers == 0);
}

int pipe_writable(file_t* f)
{
    pipe_t* p = pipe_find((uint32_t)(uintptr_t)f->private_data);
    if (!p) return 0;
    return (p->count < PIPE_BUFSZ) || (p->readers == 0);
}

int pipe_is_pipe(file_t* f)
{
    return f && (f->ops == &pipe_read_ops || f->ops == &pipe_write_ops);
}

/* 写端是否已无读者（poll：写端报告 POLLERR|POLLHUP）。 */
int pipe_readers_gone(file_t* f)
{
    pipe_t* p = pipe_find((uint32_t)(uintptr_t)f->private_data);
    if (!p) return 1;
    return p->readers == 0;
}

/* 读端是否已无写者（poll：读端报告 POLLHUP/EOF）。 */
int pipe_writers_gone(file_t* f)
{
    pipe_t* p = pipe_find((uint32_t)(uintptr_t)f->private_data);
    if (!p) return 1;
    return p->writers == 0;
}
