#ifndef VFS_H
#define VFS_H

#include "axion.h"

/* File operations interface */
typedef struct file_ops {
    uint64_t (*read)(void* file, uint64_t offset, void* buf, uint64_t size);
    uint64_t (*write)(void* file, uint64_t offset, const void* buf, uint64_t size);
    uint64_t (*open)(const char* path, uint64_t flags);
    uint64_t (*close)(void* file);
    uint64_t (*ioctl)(void* file, uint64_t req, void* arg);
    uint64_t (*lseek)(void* file, uint64_t offset, uint64_t whence);
} file_ops_t;

/* File descriptor structure */
typedef struct file {
    uint64_t    inode;
    uint64_t    size;
    uint64_t    offset;
    file_ops_t* ops;
    void*       private_data;
    uint64_t    ref_count;
    char        name[64];
    /* W7: open 时记录的标志（O_NONBLOCK 等，fcntl F_SETFL 读写）与文件类型 */
    uint64_t    flags;
    uint64_t    f_type;   /* EXT2_S_IFDIR / EXT2_S_IFREG / 0 = 未知 */
} file_t;

/* File open flags */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_NONBLOCK  0x0800

/* File descriptor table */
#define MAX_FDS 256

void     vfs_init(void);
file_t*  vfs_open(const char* path, uint64_t flags);
file_t*  vfs_open_mode(const char* path, uint64_t flags, uint64_t mode);
uint64_t vfs_read(file_t* f, void* buf, uint64_t size);
uint64_t vfs_write(file_t* f, const void* buf, uint64_t size);
uint64_t vfs_close(file_t* f);
uint64_t vfs_ioctl(file_t* f, uint64_t req, void* arg);
uint64_t vfs_lseek(file_t* f, uint64_t offset, uint64_t whence);
int      vfs_fd_alloc(file_t* f);
file_t*  vfs_fd_get(int fd);
void     vfs_fd_free(int fd);
void     vfs_fd_close_all(int pid);   /* 进程退出时关闭其自开的 fd */

/* 精确槽位注册（spawn 时把 /dev/tty 挂到 std fd 0/1/2；普通 open 用
 * vfs_fd_alloc，其保留 0/1/2）。 */
int      vfs_fd_set(file_t* f, int fd);

/* W7: 父目录写权限检查（O_CREAT/mkdir 用）。0 放行 / -13 拒绝。 */
int      vfs_dir_write_perm(uint32_t parent_ino);

/* 最近一次 vfs_open_mode 失败 errno（-13 = EACCES）。 */
extern int vfs_open_errno;

/* Allocate the lowest free fd >= `min` (fcntl F_DUPFD). */
int      vfs_fd_alloc_min(file_t* f, int min);

/* dup2: point newfd at the same file as oldfd (closing newfd's old target). */
int      vfs_fd_dup2(int oldfd, int newfd);

/* 目录列表：将 path 目录下的条目名格式化到 buf（"name/ name "），返回写入字节数 */
int      vfs_list_dir(const char* path, char* buf, int max);

#endif