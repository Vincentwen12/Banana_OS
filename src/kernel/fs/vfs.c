#include "vfs.h"
#include "vga.h"
#include "port.h"
#include "devfs.h"
#include "tmpfs.h"
#include "procfs.h"
#include "sysfs.h"
#include "ext2.h"
#include "sched.h"

static file_t* fd_table[MAX_FDS];
static int     fd_owner[MAX_FDS];   /* 打开该 fd 的 pid（-1 = 内核/未用）。
                                       进程退出时关闭自己打开的 fd，防止全局
                                       共享 fd 表在多次 fork/exec 后累积泄漏，
                                       挤占后继进程（python3 大量 import 时
                                       open 失败 → 配置 NULL → 崩溃）。 */

/* W7 (Task 2.4): 最近一次 vfs_open/vfs_open_mode 失败的 errno（权限拒绝为
 * -13）。sys_open 用它区分 ENOENT 与 EACCES。 */
int vfs_open_errno = 0;

void vfs_init(void)
{
    for (int i = 0; i < MAX_FDS; i++) {
        fd_table[i] = (file_t*)0;
        fd_owner[i] = -1;
    }
}

static int current_pid(void)
{
    extern task_t* current_task;
    return (current_task && current_task->pid > 0)
               ? current_task->pid : -1;
}

int vfs_fd_alloc(file_t* f)
{
    /* 保留 0/1/2 给标准输入/输出/错误：文件 open 永不占用 std fd，
     * 否则子进程的 stdout 会被目录/文件 fd 覆盖（静默输出丢失）。 */
    for (int i = 3; i < MAX_FDS; i++) {
        if (!fd_table[i]) {
            fd_table[i] = f;
            fd_owner[i] = current_pid();
            return i;
        }
    }
    return -1;  /* No free slots */
}

file_t* vfs_fd_get(int fd)
{
    if (fd < 0 || fd >= MAX_FDS) return (file_t*)0;
    return fd_table[fd];
}

void vfs_fd_free(int fd)
{
    if (fd >= 0 && fd < MAX_FDS) {
        fd_table[fd] = (file_t*)0;
        fd_owner[fd] = -1;
    }
}

/* 精确槽位注册（仅用于 spawn 时把 /dev/tty 挂到 0/1/2）。 */
int vfs_fd_set(file_t* f, int fd)
{
    if (fd < 0 || fd >= MAX_FDS) return -1;
    fd_table[fd] = f;
    fd_owner[fd] = current_pid();
    return 0;
}

int vfs_fd_alloc_min(file_t* f, int min)
{
    if (min < 3) min = 3;   /* F_DUPFD 不返回 std fd */
    for (int i = min; i < MAX_FDS; i++) {
        if (!fd_table[i]) {
            fd_table[i] = f;
            fd_owner[i] = current_pid();
            if (f->ref_count > 0) f->ref_count++;   /* dup 语义：共享同一 file */
            return i;
        }
    }
    return -1;  /* No free slots at or above min */
}

int vfs_fd_dup2(int oldfd, int newfd)
{
    if (oldfd < 0 || oldfd >= MAX_FDS || newfd < 0 || newfd >= MAX_FDS)
        return -1;
    file_t* f = fd_table[oldfd];
    if (!f) return -1;  /* EBADF */

    /* Close whatever newfd currently points at first (dup2 semantics). */
    if (fd_table[newfd]) {
        if (fd_table[newfd]->ops && fd_table[newfd]->ops->close)
            fd_table[newfd]->ops->close(fd_table[newfd]);
        fd_table[newfd] = (file_t*)0;
    }
    fd_table[newfd] = f;
    fd_owner[newfd] = current_pid();
    if (f->ref_count > 0) f->ref_count++;   /* dup2 语义：两个 fd 共享同一 file */
    return newfd;
}

/* 进程退出清理：关闭该 pid 打开的 fd 并释放槽位（不碰其它进程继承的 fd）。 */
void vfs_fd_close_all(int pid)
{
    if (pid <= 0) return;
    for (int i = 0; i < MAX_FDS; i++) {
        if (fd_table[i] && fd_owner[i] == pid) {
            vfs_close(fd_table[i]);
            fd_table[i] = (file_t*)0;
            fd_owner[i] = -1;
        }
    }
}

/* W7 (Task 2.4): ext2 文件权限检查。file->private_data 为 ext2 inode 号
 * (见 ext2.c ext2_open)，需读回真实 inode 取 i_mode/i_uid/i_gid。
 * euid==0 (root) 放行；否则按 owner/group/other 位检查读写权限。
 * 拒绝返回 -13 (EACCES)。devfs/tmpfs/procfs/sysfs 不经过这里。 */
static int vfs_check_perm(file_t* f, uint64_t flags)
{
    uint32_t ino = (uint32_t)(uintptr_t)f->private_data;
    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) < 0) return 0;   /* 读不到 inode：放行 */

    uint32_t uid = current_task ? current_task->euid : 0;
    uint32_t gid = current_task ? current_task->egid : 0;
    if (uid == 0) return 0;                            /* root 放行 */

    int need_r = 0, need_w = 0;
    uint32_t acc = flags & 0x3;                        /* O_RDONLY=0 O_WRONLY=1 O_RDWR=2 */
    if (acc == 0) need_r = 1;
    else if (acc == 1) need_w = 1;
    else if (acc == 2) { need_r = 1; need_w = 1; }
    if (flags & (O_TRUNC | O_CREAT)) need_w = 1;

    uint16_t mode = inode.i_mode;
    uint16_t perm;
    if (uid == inode.i_uid)        perm = (mode >> 6) & 7;   /* owner */
    else if (gid == inode.i_gid)   perm = (mode >> 3) & 7;   /* group */
    else                           perm = mode & 7;          /* other */

    if ((need_r && !(perm & 4)) || (need_w && !(perm & 2)))
        return -13;                                        /* EACCES */
    return 0;
}

/* W7: O_CREAT 时检查父目录的写权限（非 root 用户写入 root 目录 → EACCES）。
 * 返回 0 放行 / -13 拒绝。 */
int vfs_dir_write_perm(uint32_t parent_ino)
{
    if (!current_task || current_task->euid == 0) return 0;
    ext2_inode_t inode;
    if (ext2_read_inode(parent_ino, &inode) < 0) return 0;
    uint32_t uid = current_task->euid, gid = current_task->egid;
    uint16_t mode = inode.i_mode;
    uint16_t perm;
    if (uid == inode.i_uid)        perm = (mode >> 6) & 7;
    else if (gid == inode.i_gid)   perm = (mode >> 3) & 7;
    else                           perm = mode & 7;
    return (perm & 2) ? 0 : -13;
}

/* W7 (Task 2.4): 带 mode 的打开路径。ext2 分支在 O_CREAT 且文件不存在时
 * 用调用者 mode 创建（透传 ext2_create_file），随后做权限检查；
 * devfs/tmpfs/procfs/sysfs 不检查。mode 仅对新建文件生效。 */
file_t* vfs_open_mode(const char* path, uint64_t flags, uint64_t mode)
{
    if (!path) return (file_t*)0;
    vfs_open_errno = 0;

    /* 路径以 /dev 开头时先走 devfs */
    file_t* f = devfs_open(path, flags);
    if (f) return f;

    /* 伪文件系统按路径前缀直接分发：/proc、/sys */
    if (procfs_is_path(path)) return procfs_open(path, flags);
    if (sysfs_is_path(path)) return sysfs_open(path, flags);

    /* 其余路径先走 EXT2 磁盘文件系统。O_CREAT 且文件不存在：先按调用者
     * mode 显式创建（ext2_open 内部硬编码 0x01FF，无法透传 mode）。 */
    if ((flags & O_CREAT)) {
        uint32_t ino;
        if (ext2_lookup_path(path, &ino) < 0) {
            const char* slash = 0;
            for (const char* q = path; *q; q++) if (*q == '/') slash = q;
            if (slash && slash[1]) {
                char parent[256];
                int plen = (int)(slash - path);
                if (plen == 0) { parent[0] = '/'; parent[1] = 0; }
                else if (plen < (int)sizeof(parent)) {
                    for (int i = 0; i < plen; i++) parent[i] = path[i];
                    parent[plen] = 0;
                } else {
                    plen = -1;
                }
                if (plen >= 0) {
                    uint32_t parent_ino;
                    if (ext2_lookup_path(parent, &parent_ino) == 0) {
                        /* W7: 非 root 写入父目录需写权限（/bin 等 root 目录
                         * 0755 → banana 拒绝 → EACCES）。 */
                        if (vfs_dir_write_perm(parent_ino) < 0) {
                            vfs_open_errno = -13;
                            return (file_t*)0;
                        }
                        ext2_create_file(parent_ino, slash + 1,
                                         (uint32_t)(mode & 0x0FFF));
                    }
                }
            }
        }
    }

    f = ext2_open(path, flags);
    if (f) {
        if (vfs_check_perm(f, flags) < 0) {
            vfs_close(f);
            vfs_open_errno = -13;                       /* EACCES */
            return (file_t*)0;
        }
        return f;
    }

    /* W7 (Task 4.8): tmpfs 最后（消除 ext2 已覆盖路径上的 tmpfs 噪音；
     * 仅 ext2 不存在的路径（如 /bin/hello）落到这里）。 */
    return tmpfs_open(path, flags);
}

file_t* vfs_open(const char* path, uint64_t flags)
{
    return vfs_open_mode(path, flags, 0x01FF);   /* 旧行为：O_CREAT 时 0777 */
}

int vfs_list_dir(const char* path, char* buf, int max)
{
    if (!path || !buf) return -1;
    if (procfs_is_path(path)) return procfs_list(path, buf, max);
    if (sysfs_is_path(path)) return sysfs_list(path, buf, max);
    return ext2_list(path, buf, max);
}

uint64_t vfs_read(file_t* f, void* buf, uint64_t size)
{
    if (!f) return (uint64_t)(-1);
    if (!f->ops || !f->ops->read) return (uint64_t)(-1);
    return f->ops->read(f, f->offset, buf, size);
}

uint64_t vfs_write(file_t* f, const void* buf, uint64_t size)
{
    if (!f) return (uint64_t)(-1);
    if (!f->ops || !f->ops->write) return (uint64_t)(-1);
    return f->ops->write(f, f->offset, buf, size);
}

uint64_t vfs_close(file_t* f)
{
    if (!f || !f->ops || !f->ops->close) return (uint64_t)(-1);
    return f->ops->close(f);
}

uint64_t vfs_ioctl(file_t* f, uint64_t req, void* arg)
{
    if (!f || !f->ops || !f->ops->ioctl) return (uint64_t)(-1);
    return f->ops->ioctl(f, req, arg);
}

uint64_t vfs_lseek(file_t* f, uint64_t offset, uint64_t whence)
{
    if (!f) return (uint64_t)(-1);

    switch (whence) {
    case 0: /* SEEK_SET */
        f->offset = offset;
        break;
    case 1: /* SEEK_CUR */
        f->offset += offset;
        break;
    case 2: /* SEEK_END */
        f->offset = f->size + offset;
        break;
    default:
        return (uint64_t)(-1);
    }
    return f->offset;
}