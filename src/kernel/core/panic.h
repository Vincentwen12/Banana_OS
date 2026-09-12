#ifndef PANIC_H
#define PANIC_H

#include "axion.h"

/* Panic 记录保留区（固定物理地址，软重启后 RAM 内容保留）。
 * 位置选择：必须在内核镜像之后（当前 kernel_end ≈ 0x71f000，含 BSS）、
 * Ω 堆 (HEAP_BASE 0x800000) 之前；避开 DOORBELL(0x9000)、
 * IPC_SHM(0x70000000)、AP_TRAMPOLINE(0x8000)、VGA(0xB8000)。 */
#define PANIC_RECORD_ADDR      0x740000
#define PANIC_RECORD_MAGIC     0x50414E31  /* 'PAN1' */
#define PANIC_RECORD_MSG_LEN   128
#define PANIC_SAFE_MODE_THRESHOLD 3

typedef struct {
    uint32_t magic;                    /* PANIC_RECORD_MAGIC */
    uint32_t count;                    /* 累计 panic 次数（跨重启保留） */
    char     msg[PANIC_RECORD_MSG_LEN];
    uint64_t rip;
    uint64_t rsp;
    uint64_t cr2;
} panic_record_t;

void kernel_panic(const char* msg);

/* 启动早期调用：读取保留区，magic 匹配则打印 "Previous panic: ..."；
 * 连续 panic 次数达到阈值时置安全模式标志。不清零记录。 */
void panic_check_previous(void);

/* 返回是否处于安全模式（连续 panic >= 阈值，跳过 auto-run bash）。 */
int panic_safe_mode(void);

#endif
