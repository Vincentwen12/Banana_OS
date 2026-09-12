#ifndef PROCFS_H
#define PROCFS_H

#include "vfs.h"

/* 生成式伪文件系统：/proc/uptime、/proc/self/status、/proc（进程表） */
file_t* procfs_open(const char* path, uint64_t flags);

/* 目录列表（vfs_list_dir 用）："/proc" → "uptime self " */
int procfs_list(const char* path, char* buf, int max);

/* 路径是否以 /proc 开头（vfs 分发用） */
int procfs_is_path(const char* path);

#endif
