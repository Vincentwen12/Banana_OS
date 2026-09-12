#ifndef DEVFS_H
#define DEVFS_H

#include "vfs.h"

void    devfs_init(void);
file_t* devfs_open(const char* path, uint64_t flags);

/* /dev/tty termios state (glibc c_lflag / c_oflag bits). */
void devfs_tty_get_flags(uint32_t* lflag, uint32_t* oflag);
void devfs_tty_set_flags(uint32_t lflag, uint32_t oflag);

/* Task 3.1: /dev/tty 前台进程组（TIOCGPGRP/TIOCSPGRP 读写）。 */
uint32_t devfs_tty_get_pgrp(void);
void devfs_tty_set_pgrp(uint32_t pgrp);

/* W7: /dev/tty 是否可读（poll/select 用）。 */
int devfs_tty_readable(void);

/* W7: fd 是否指向 /dev/tty（poll 按类型分发用）。 */
int devfs_is_tty(file_t* f);

#endif