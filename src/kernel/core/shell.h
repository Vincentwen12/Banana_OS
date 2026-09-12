#ifndef SHELL_H
#define SHELL_H

#include "axion.h"

void shell_init(void);
void shell_execute(const char* line);
void shell_prompt(void);

/* W7: 按路径启动用户程序（前台），返回 pid；kmain 自动运行 bash 用。 */
uint64_t shell_launch_program(const char* path, int extra_argc,
                              const char** extra_argv);

/* W7: 前台任务 pid（0 = 无）。cmd_run 设置，kmain 主循环在任务退出后
 * 恢复内核 Shell 输入。 */
extern uint64_t g_fg_pid;

/* 内核 shell 行编辑辅助（kmain 主循环调用）：
 * 命令历史（环形）+ Tab 命令名补全。 */
void shell_hist_add(const char* line);
const char* shell_hist_get(int offset);   /* 0 = 最近一条，越界返回 NULL */
int  shell_tab_complete(const char* prefix, char* out, int max);

#endif