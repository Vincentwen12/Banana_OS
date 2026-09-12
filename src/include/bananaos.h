#ifndef BANANAOS_H
#define BANANAOS_H

/* 基础类型定义 - 不依赖标准库 */
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;
typedef signed int         int32_t;
typedef signed long long    int64_t;
typedef unsigned long long size_t;
typedef uint8_t            bool;

/* POSIX 兼容类型 */
typedef int32_t            pid_t;
typedef int64_t            ssize_t;
typedef int64_t            off_t;
typedef uint32_t           mode_t;
typedef uint32_t           uid_t;
typedef uint32_t           gid_t;

#define NULL    ((void*)0)
#define true    1
#define false   0

/* 物理地址常量 */
#define VGA_BASE         0xB8000
#define KERNEL_PHYS_BASE 0x100000
#define DOORBELL_PAGE    0x9000
#define HEARTBEAT_ADDR   0xA000
#define AP_TRAMPOLINE    0x8000

/* 内核栈大小 */
#define KERNEL_STACK_SIZE 0x4000

/* 最大 NUMA 节点数 */
#define MAX_NUMA_NODES    4
#define MAX_CPUS          64

/* 门铃数量 */
#define MAX_DOORBELLS     64

/* 页表常量 */
#define PAGE_SIZE_2MB     0x200000
#define PAGE_PRESENT      0x001
#define PAGE_RW           0x002
#define PAGE_HUGE         0x080

/* 端口 */
#define PS2_DATA_PORT     0x60
#define PS2_STATUS_PORT   0x64
#define PIC1_DATA         0x21
#define PIC2_DATA         0xA1
#define RESET_PORT        0xCF9

/* 错误码 */
#define EPERM      1
#define ENOENT     2
#define ESRCH      3
#define EINTR      4
#define EIO        5
#define ENXIO      6
#define E2BIG      7
#define ENOEXEC    8
#define EBADF      9
#define ECHILD     10
#define EAGAIN     11
#define ENOMEM     12
#define EACCES     13
#define EFAULT     14
#define ENOTBLK    15
#define EBUSY      16
#define EEXIST     17
#define EXDEV      18
#define ENODEV     19
#define ENOTDIR    20
#define EISDIR     21
#define EINVAL     22
#define ENFILE     23
#define EMFILE     24
#define ENOTTY     25
#define ETXTBSY    26
#define EFBIG      27
#define ENOSPC     28
#define ESPIPE     29
#define EROFS      30
#define EMLINK     31
#define EPIPE      32

/* 内存布局 */
#define KERNEL_HEAP_START   0x200000   /* 2MB - 内核堆起始 */
#define KERNEL_HEAP_SIZE    0x200000   /* 2MB - 内核堆大小 */
#define USER_SPACE_START    0x400000   /* 4MB - 用户空间起始 */
#define ZONE0_START         0x400000   /* 热区: 用户空间开始 */
#define ZONE1_START         0x800000   /* 温区: 压缩区 */
#define ZONE2_START         0xC00000   /* 回旋镖池: 深度压缩 */
#define PAGE_SIZE           0x1000     /* 4KB */

/* 门铃 IPC */
#define DOORBELL_BASE       0x9000     /* 门铃物理基址 */
#define DOORBELL_COUNT      64         /* 门铃通道数 */
#define DOORBELL_CHANNEL_SIZE 64       /* 每通道 64 字节 (缓存行对齐) */

/* 系统调用号 */
#define SYS_read     0
#define SYS_write    1
#define SYS_open     2
#define SYS_close    3
#define SYS_stat     4
#define SYS_fstat    5
#define SYS_lseek    8
#define SYS_mmap     9
#define SYS_mprotect 10
#define SYS_munmap   11
#define SYS_brk      12
#define SYS_sigaction 13
#define SYS_sigreturn 15
#define SYS_getpid   39
#define SYS_fork     57
#define SYS_execve   59
#define SYS_exit     60
#define SYS_wait4    61
#define SYS_kill     62
#define SYS_uname    63
#define SYS_getdents 78
#define SYS_getcwd   79
#define SYS_chdir    80
#define SYS_mkdir    83
#define SYS_rmdir    84
#define SYS_unlink   87
#define SYS_select   93
#define SYS_poll     94
#define SYS_nanosleep 35
#define SYS_gettimeofday 96
#define SYS_clock_gettime 228
#define SYS_pipe     22
#define SYS_dup      32
#define SYS_dup2     33
#define SYS_getuid   102
#define SYS_getgid   104
#define SYS_gethostname 170
#define SYS_sysinfo  99

/* 任务状态 */
#define TASK_RUNNING    0
#define TASK_READY      1
#define TASK_BLOCKED    2
#define TASK_ZOMBIE     3
#define TASK_DEAD       4

/* I/O 端口操作 */
static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint16_t inw(uint16_t port) {
    uint16_t val;
    __asm__ volatile("inw %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t ind(uint16_t port) {
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outd(uint16_t port, uint32_t val) {
    __asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

/* 端口延迟 (约 1μs) */
static inline void port_delay(void) {
    outb(0x80, 0);
}

/* 内存屏障 */
static inline void mfence(void) {
    __asm__ volatile("mfence" : : : "memory");
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

/* MONITOR/MWAIT */
static inline void monitor(void* addr, uint32_t ecx, uint32_t edx) {
    __asm__ volatile("monitor" : : "a"(addr), "c"(ecx), "d"(edx));
}

static inline void mwait(uint32_t eax, uint32_t ecx) {
    __asm__ volatile("mwait" : : "a"(eax), "c"(ecx));
}

/* CPUID */
static inline void cpuid(uint32_t leaf, uint32_t* eax, uint32_t* ebx,
                         uint32_t* ecx, uint32_t* edx) {
    __asm__ volatile("cpuid"
        : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
        : "a"(leaf));
}

/* Shell */
#define CMD_MAX_LEN     256
#define CMD_MAX_ARGS    16
#define SHELL_PROMPT    "epoch> "

#endif /* BANANAOS_H */