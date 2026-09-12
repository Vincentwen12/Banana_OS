#include "sysfs.h"
#include "vga.h"

/* 打开的文件槽位池：open 时生成内容到槽位缓冲，close 时归还 */
#define SYSFS_POOL     8
#define SYSFS_DATA_LEN 128

static file_t sys_files[SYSFS_POOL];
static char   sys_data[SYSFS_POOL][SYSFS_DATA_LEN];
static int    sys_busy[SYSFS_POOL];
static int    sys_ops_inited = 0;

static file_ops_t sys_ops;

static uint64_t sysfs_boot_ms = 0;

/* ---- 小工具 ---- */

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

/* ---- file ops ---- */

static uint64_t sys_read(void* f, uint64_t off, void* buf, uint64_t size)
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

static uint64_t sys_write(void* f, uint64_t off, const void* buf, uint64_t size)
{
    (void)f; (void)off; (void)buf;
    return size;  /* 只读 */
}

static uint64_t sys_close(void* f)
{
    file_t* file = (file_t*)f;
    for (int i = 0; i < SYSFS_POOL; i++) {
        if (sys_data[i] == (char*)file->private_data) {
            sys_busy[i] = 0;
            break;
        }
    }
    return 0;
}

static void sysfs_init_ops(void)
{
    if (sys_ops_inited) return;
    sys_ops.read  = sys_read;
    sys_ops.write = sys_write;
    sys_ops.open  = 0;
    sys_ops.close = sys_close;
    sys_ops.ioctl = 0;
    sys_ops.lseek = 0;
    sys_ops_inited = 1;
}

void sysfs_init(void)
{
    sysfs_boot_ms = 0;
}

void sysfs_set_boot_ms(uint64_t ms)
{
    sysfs_boot_ms = ms;
}

file_t* sysfs_open(const char* path, uint64_t flags)
{
    (void)flags;
    if (!path || path[0] != '/' || path[1] != 's' || path[2] != 'y' ||
        path[3] != 's') return (file_t*)0;

    sysfs_init_ops();

    int slot = -1;
    for (int i = 0; i < SYSFS_POOL; i++) {
        if (!sys_busy[i]) { slot = i; break; }
    }
    if (slot < 0) return (file_t*)0;

    char* out = sys_data[slot];
    int len = 0;

    if (str_eq(path, "/sys/kernel/version") == 0) {
        len = append_str(out, SYSFS_DATA_LEN, 0, "Axion-Ban 0.5.0\n");
    } else if (str_eq(path, "/sys/kernel/boottime") == 0) {
        len = append_uint(out, SYSFS_DATA_LEN, 0, sysfs_boot_ms);
        len = append_str(out, SYSFS_DATA_LEN, len, " ms\n");
    } else {
        sys_busy[slot] = 0;
        return (file_t*)0;
    }

    file_t* f = &sys_files[slot];
    f->inode = 200 + (uint64_t)slot;
    f->size  = (uint64_t)len;
    f->offset = 0;
    f->ops   = &sys_ops;
    f->private_data = out;
    f->ref_count = 1;
    for (int i = 0; i < 64; i++) f->name[i] = 0;
    for (int i = 0; path[i] && i < 63; i++) f->name[i] = path[i];
    return f;
}

int sysfs_list(const char* path, char* buf, int max)
{
    if (!path || !buf) return -1;
    int pos = 0;
    if (str_eq(path, "/sys") == 0) {
        pos = append_str(buf, max, pos, "kernel ");
    } else if (str_eq(path, "/sys/kernel") == 0) {
        pos = append_str(buf, max, pos, "version ");
        pos = append_str(buf, max, pos, "boottime ");
    } else {
        return -1;
    }
    if (pos < max) buf[pos] = '\0';
    return pos;
}

int sysfs_is_path(const char* path)
{
    if (!path) return 0;
    return str_begins(path, "/sys");
}
