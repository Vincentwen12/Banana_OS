#ifndef PIPE_H
#define PIPE_H

#include "axion.h"
#include "vfs.h"

/* 创建一对管道 fd（内核 Shell / sys_pipe 使用）。返回 0 成功；写端/读端
 * 分别为 fds[1]/fds[0]。fds 由调用者写入用户空间。 */
int pipe_create_fds(int fds[2], uint64_t flags);

/* 管道操作：用于 poll/select 的就绪判断。 */
int pipe_readable(file_t* f);
int pipe_writable(file_t* f);
int pipe_is_pipe(file_t* f);
int pipe_readers_gone(file_t* f);
int pipe_writers_gone(file_t* f);

#endif
