#ifndef AXION_H
#define AXION_H

/* 基础类型定义 */
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;
typedef unsigned long long size_t;
typedef unsigned long long uintptr_t;
typedef uint8_t            bool;

#define NULL    ((void*)0)
#define true    1
#define false   0

/* 内存操作（core/string.c 实现，≥64B 走 rep movsb/stosb） */
void* memcpy(void* dst, const void* src, size_t n);
void* memset(void* dst, int c, size_t n);

/* 物理地址常量 */
#define VGA_BASE         0xB8000
#define KERNEL_PHYS_BASE 0x100000
#define HEAP_BASE        0x800000   /* 8MB (after kernel BSS ~6MB) */

/* 内存常量 */
#define PAGE_SIZE           0x1000     /* 4KB */
#define TOTAL_MEMORY        0x80000000 /* 2GB */
#define HOT_ZONE_SIZE       0x40000000 /* 1024MB 热区 */
#define WARM_ZONE_SIZE      0x20000000 /* 512MB  温区 */
#define BOOMERANG_ZONE_SIZE 0x10000000 /* 256MB  回旋镖池 */

/* 热区: HEAP_BASE .. HEAP_BASE + HOT_ZONE_SIZE */
#define HOT_ZONE_PAGES      (HOT_ZONE_SIZE / PAGE_SIZE)       /* 262144 */
#define WARM_BASE           (HEAP_BASE + HOT_ZONE_SIZE)       /* 0x40800000 */
#define WARM_ZONE_PAGES     (WARM_ZONE_SIZE / PAGE_SIZE)      /* 131072 */
#define BOOMERANG_BASE      (WARM_BASE + WARM_ZONE_SIZE)      /* 0x60800000 */
#define BOOMERANG_PAGES     (BOOMERANG_ZONE_SIZE / PAGE_SIZE) /* 65536 */

#define NUM_PAGE_GROUPS     (HOT_ZONE_PAGES / 64)
#define HEAP_PAGES          HOT_ZONE_PAGES

/* 门铃 IPC */
#define DOORBELL_BASE       0x9000
#define DOORBELL_COUNT      64
#define DOORBELL_CHANNEL_SIZE 256

/* 调度器 */
#define MAX_TASKS           64
#define MLFQ_LEVELS         5
#define TIME_SLICE_BASE     10      /* 基础时间片 (ticks) */
#define MAX_CORES           4

/* 任务状态 */
#define TASK_STATE_RUNNING  0
#define TASK_STATE_READY    1
#define TASK_STATE_BLOCKED  2
#define TASK_STATE_SLEEPING 3
#define TASK_STATE_ZOMBIE   4
#define TASK_STATE_REAPED   5

/* AP 启动 */
#define AP_TRAMPOLINE_ADDR  0x8000
#define LAPIC_BASE          0xFEE00000
#define IOAPIC_BASE         0xFEC00000

/* IPC 共享内存 */
#define IPC_SHM_BASE        0x70000000  /* 共享内存基址 (1.75GB) */
#define IPC_SHM_SIZE        0x10000000  /* 256MB */
#define IPC_MAX_CHANNELS    16

/* Shell */
#define CMD_MAX_LEN         256
#define CMD_MAX_ARGS        16

/* I/O 端口 */
#define PS2_DATA_PORT       0x60
#define PS2_STATUS_PORT     0x64
#define PS2_CMD_PORT        0x64
#define PIC1_DATA           0x21
#define PIC2_DATA           0xA1
#define SERIAL_PORT         0x3F8

/* 内存屏障 */
static inline void mfence(void) {
    __asm__ volatile("mfence" : : : "memory");
}

static inline void sfence(void) {
    __asm__ volatile("sfence" : : : "memory");
}

/* 读 TSC */
static inline uint64_t rdtsc(void) {
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

/* 读 MSR */
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

/* 写 MSR */
static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val;
    uint32_t hi = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
}

/* CPUID */
static inline void cpuid(uint32_t leaf, uint32_t* eax, uint32_t* ebx,
                         uint32_t* ecx, uint32_t* edx) {
    __asm__ volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf));
}

#endif /* AXION_H */