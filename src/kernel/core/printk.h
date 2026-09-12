#ifndef PRINTK_H
#define PRINTK_H

#include "axion.h"

/* 日志级别（与 Linux 一致，0=最高紧急） */
#define KERN_EMERG   0
#define KERN_ALERT   1
#define KERN_CRIT    2
#define KERN_ERR     3
#define KERN_WARNING 4
#define KERN_NOTICE  5
#define KERN_INFO    6
#define KERN_DEBUG   7

/* 控制台过滤级别：仅 level <= 此值 输出到 VGA/串口。
 * DEBUG(7) 只进环形缓冲，避免刷屏。默认 INFO；`quiet` 启动参数
 * 可将其提升为 WARNING（跳过全部 INFO 启动日志，见 printk_set_console_level）。 */
#define PRINTK_CONSOLE_LEVEL  KERN_INFO

void printk_init(void);

/* 运行时调整控制台过滤级别（quiet 启动参数用）。 */
void printk_set_console_level(int level);

/* 格式化日志：写入环形缓冲（总是），并按过滤级别输出到控制台。
 * 支持 %s %d %u %x %p %c %%。 */
void printk(int level, const char* fmt, ...);

/* 遍历环形缓冲，每条按 "<level>text\n" 交给 emit（dmesg 命令用）。 */
void printk_dump(void (*emit)(const char* s));

#endif
