#include "procfs.h"
#include "vga.h"
#include "timer.h"
#include "sched.h"

/* 打开的文件槽位池：open 时生成内容到槽位缓冲，close 时归还。
 * 同时打开 /proc 文件的进程很少，16 槽足够。 */
#define PROC_POOL     16
#define PROC_DATA_LEN 512

static file_t proc_files[PROC_POOL];
static char   proc_data[PROC_POOL][PROC_DATA_LEN];
static int    proc_busy[PROC_POOL];
static int    proc_ops_inited = 0;

static file_ops_t proc_ops;

/* ---- 小工具：字符串比较 / 追加 ---- */

static int str_eq(const char* a, const char* b)
{
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}

static int str_begins(const char* s, const char* pre)
{
    while (*pre) {
        if (*s != *pre) return 0;
        s++; pre++;
    }
    return 1;
}

static int append_str(char* out, int cap, int pos, const char* s)
{
    while (*s && pos < cap - 1) out[pos++] = *s++;
    return pos;
}

static int append_uint(char* out, int cap, int pos, uint64_t v)
{
    char tmp[24];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = '0' + (char)(v % 10); v /= 10; }
    while (i && pos < cap - 1) out[pos++] = tmp[--i];
    return pos;
}

/* ---- /proc/self/status：当前任务信息（sys_read 时 current_task 即读者） ---- */

static int gen_status(char* out, int cap)
{
    static const char* state_names[] = {"RUN", "RDY", "BLK", "SLP", "ZMB", "RPD"};
    task_t* t = current_task;
    if (!t) {
        return append_str(out, cap, 0, "Name:\t(none)\nPid:\t0\nState:\t?\n");
    }
    const char* st = "???";
    if (t->state >= 0 && t->state <= 5) st = state_names[t->state];

    int pos = append_str(out, cap, 0, "Name:\t");
    pos = append_str(out, cap, pos, t->name ? t->name : "(unnamed)");
    pos = append_str(out, cap, pos, "\nPid:\t");
    pos = append_uint(out, cap, pos, t->pid);
    pos = append_str(out, cap, pos, "\nPpid:\t");
    pos = append_uint(out, cap, pos, t->ppid);
    pos = append_str(out, cap, pos, "\nState:\t");
    pos = append_str(out, cap, pos, st);
    pos = append_str(out, cap, pos, "\nPriority:\t");
    pos = append_uint(out, cap, pos, (uint64_t)t->priority);
    pos = append_str(out, cap, pos, "\n");
    return pos;
}

/* ---- file ops：生成式读取 ---- */

static uint64_t proc_read(void* f, uint64_t off, void* buf, uint64_t size)
{
    file_t* file = (file_t*)f;
    if (off >= file->size) return 0;
    uint64_t remain = file->size - off;
    if (size > remain) size = remain;
    const char* src = (const char*)file->private_data;
    for (uint64_t i = 0; i < size; i++) ((char*)buf)[i] = src[off + i];
    file->offset = off + size;   /* 推进文件偏移（与 ext2 read op 一致） */
    return size;
}

static uint64_t proc_write(void* f, uint64_t off, const void* buf, uint64_t size)
{
    (void)f; (void)off; (void)buf;
    return size;  /* 只读 */
}

static uint64_t proc_close(void* f)
{
    file_t* file = (file_t*)f;
    for (int i = 0; i < PROC_POOL; i++) {
        if (proc_data[i] == (char*)file->private_data) {
            proc_busy[i] = 0;
            break;
        }
    }
    return 0;
}

static void procfs_init_ops(void)
{
    if (proc_ops_inited) return;
    proc_ops.read  = proc_read;
    proc_ops.write = proc_write;
    proc_ops.open  = 0;
    proc_ops.close = proc_close;
    proc_ops.ioctl = 0;
    proc_ops.lseek = 0;
    proc_ops_inited = 1;
}

file_t* procfs_open(const char* path, uint64_t flags)
{
    (void)flags;
    if (!path || path[0] != '/' || path[1] != 'p' || path[2] != 'r' ||
        path[3] != 'o' || path[4] != 'c') return (file_t*)0;

    procfs_init_ops();

    int slot = -1;
    for (int i = 0; i < PROC_POOL; i++) {
        if (!proc_busy[i]) { slot = i; break; }
    }
    if (slot < 0) return (file_t*)0;

    char* out = proc_data[slot];
    int len = 0;

    if (path[5] == '\0') {
        /* /proc 目录：进程表（pid/name/state 每行） */
        len = sched_list(out, PROC_DATA_LEN);
        if (len >= PROC_DATA_LEN) len = PROC_DATA_LEN - 1;
    } else if (str_eq(path, "/proc/uptime") == 0) {
        len = append_uint(out, PROC_DATA_LEN, 0, timer_ms() / 1000);
        len = append_str(out, PROC_DATA_LEN, len, "\n");
    } else if (str_eq(path, "/proc/self/status") == 0) {
        len = gen_status(out, PROC_DATA_LEN);
    } else {
        proc_busy[slot] = 0;
        return (file_t*)0;
    }

    file_t* f = &proc_files[slot];
    f->inode = 100 + (uint64_t)slot;
    f->size  = (uint64_t)len;
    f->offset = 0;
    f->ops   = &proc_ops;
    f->private_data = out;
    f->ref_count = 1;
    for (int i = 0; i < 64; i++) f->name[i] = 0;
    for (int i = 0; path[i] && i < 63; i++) f->name[i] = path[i];
    return f;
}

int procfs_list(const char* path, char* buf, int max)
{
    if (!path || !buf) return -1;
    if (str_eq(path, "/proc") == 0) {
        int pos = 0;
        pos = append_str(buf, max, pos, "uptime ");
        pos = append_str(buf, max, pos, "self ");
        if (pos < max) buf[pos] = '\0';
        return pos;
    }
    return -1;
}

int procfs_is_path(const char* path)
{
    if (!path) return 0;
    return str_begins(path, "/proc");
}
