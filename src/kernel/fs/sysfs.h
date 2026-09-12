#ifndef SYSFS_H
#define SYSFS_H

#include "vfs.h"

/* 只读伪文件系统：/sys/kernel/version、/sys/kernel/boottime */
void    sysfs_init(void);
void    sysfs_set_boot_ms(uint64_t ms);
file_t* sysfs_open(const char* path, uint64_t flags);

/* 目录列表（vfs_list_dir 用）："/sys" → "kernel "，"/sys/kernel" → "version boottime " */
int sysfs_list(const char* path, char* buf, int max);

/* 路径是否以 /sys 开头（vfs 分发用） */
int sysfs_is_path(const char* path);

#endif
