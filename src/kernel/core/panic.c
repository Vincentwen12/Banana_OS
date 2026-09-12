#include "panic.h"
#include "vga.h"
#include "printk.h"
#include "port.h"
#include "timer.h"
#include "sched.h"

/* 栈回溯边界保护：帧地址必须 8 对齐且在合理内核栈范围内，帧数上限 32 */
#define PANIC_STACK_MIN   0x100000
#define PANIC_STACK_MAX   0x1400000
#define PANIC_MAX_FRAMES  32

/* 安全模式标志：由 panic_check_previous() 在连续 panic 达到阈值时置位 */
static int safe_mode = 0;

int panic_safe_mode(void) {
    return safe_mode;
}

/* 打印十进制无符号数 */
static void print_u64(uint64_t n) {
    if (n == 0) { vga_putc('0'); return; }
    char rev[24]; int rp = 0;
    while (n) { rev[rp++] = '0' + (char)(n % 10); n /= 10; }
    while (rp) vga_putc(rev[--rp]);
}

/* 打印 64 位十六进制（无前导零） */
static void print_hex64(uint64_t val) {
    static const char hex[] = "0123456789ABCDEF";
    vga_puts("0x");
    int started = 0;
    for (int i = 60; i >= 0; i -= 4) {
        int d = (int)((val >> i) & 0xF);
        if (d || started || i == 0) {
            started = 1;
            vga_putc(hex[d]);
        }
    }
}

/* 受限字符串拷贝：保证 NUL 结尾，len 为缓冲区大小 */
static void str_copy(char* dst, const char* src, int len) {
    int i = 0;
    if (src) {
        while (i < len - 1 && src[i]) { dst[i] = src[i]; i++; }
    }
    dst[i] = '\0';
}

void panic_check_previous(void) {
    volatile panic_record_t* rec = (volatile panic_record_t*)PANIC_RECORD_ADDR;
    if (rec->magic != PANIC_RECORD_MAGIC) return;

    vga_set_color(0x0E, 0x00);  /* 黄色醒目 */
    vga_puts("Previous panic: ");
    vga_puts((const char*)rec->msg);
    vga_puts(" (count=");
    print_u64((uint64_t)rec->count);
    vga_puts(")\n");
    vga_set_color(VGA_COLOR, 0x00);

    /* 连续 panic 次数达到阈值 → 安全模式（不清零记录，count 继续累计） */
    if (rec->count >= PANIC_SAFE_MODE_THRESHOLD) {
        safe_mode = 1;
    }
}

void kernel_panic(const char* msg) {
    /* 快照 CPU 现场 */
    uint64_t rip = (uint64_t)__builtin_return_address(0);
    uint64_t rsp, rbp, cr2;
    __asm__ volatile("movq %%rsp, %0" : "=r"(rsp));
    __asm__ volatile("movq %%rbp, %0" : "=r"(rbp));
    __asm__ volatile("movq %%cr2, %0" : "=r"(cr2));

    /* 0. 记录到日志环形缓冲（dmesg 可见） */
    printk(KERN_ERR, "KERNEL PANIC: %s (pid=%u task=%s)\n",
           msg ? msg : "(null)",
           current_task ? (unsigned)current_task->pid : 0,
           current_task && current_task->name ? current_task->name : "(none)");

    /* 1. 红字 panic 消息 */
    vga_set_color(0x0C, 0x00);  /* 红字黑底 */
    vga_puts("\n[PANIC] ");
    vga_puts(msg ? msg : "(null)");
    vga_puts("\n");

    /* 2. 当前任务名 / PID */
    if (current_task) {
        vga_puts("Task: ");
        vga_puts(current_task->name ? current_task->name : "(unnamed)");
        vga_puts(" (pid=");
        print_u64(current_task->pid);
        vga_puts(")\n");
    } else {
        vga_puts("Task: (none)\n");
    }

    /* 3. RIP / RSP / CR2 快照 */
    vga_puts("RIP=");
    print_hex64(rip);
    vga_puts(" RSP=");
    print_hex64(rsp);
    vga_puts(" CR2=");
    print_hex64(cr2);
    vga_puts("\n");

    /* 4. 栈回溯：从当前 RBP 逐帧解引用，带边界保护 */
    vga_puts("Stack trace:\n");
    uint64_t frame = rbp;
    for (int i = 0; i < PANIC_MAX_FRAMES && frame != 0; i++) {
        /* 帧地址必须 8 对齐且在合理范围内，否则野指针终止回溯 */
        if ((frame & 0x7) != 0 || frame < PANIC_STACK_MIN || frame > PANIC_STACK_MAX) {
            vga_puts("  [unwind stopped: bad frame]\n");
            break;
        }
        uint64_t next = *(uint64_t*)frame;       /* frame[0] = 上一帧 rbp */
        uint64_t ret  = *(uint64_t*)(frame + 8); /* frame[1] = 返回地址 */
        vga_puts("  #");
        print_u64((uint64_t)i);
        vga_puts(" ");
        print_hex64(ret);
        vga_puts("\n");
        /* 帧地址必须单调递增，防止循环 */
        if (next <= frame) break;
        frame = next;
    }

    /* 5. 写 panic 记录到保留区（软重启后 RAM 保留） */
    {
        volatile panic_record_t* rec = (volatile panic_record_t*)PANIC_RECORD_ADDR;
        rec->magic = PANIC_RECORD_MAGIC;
        rec->count++;
        str_copy((char*)rec->msg, msg ? msg : "(null)", PANIC_RECORD_MSG_LEN);
        rec->rip = rip;
        rec->rsp = rsp;
        rec->cr2 = cr2;
        __asm__ volatile("mfence" : : : "memory");
    }

    /* 6. 5 秒倒计时后 0xCF9 软复位 */
    vga_puts("Rebooting in 5 seconds...\n");
    for (int i = 5; i > 0; i--) {
        vga_puts("  ");
        print_u64((uint64_t)i);
        vga_puts("...\n");
        uint64_t target = timer_ms() + 1000;
        while (timer_ms() < target) {
            __asm__ volatile("pause");
        }
    }
    vga_puts("  rebooting now\n");

    /* 0xCF9 软复位；若未生效依次尝试 QEMU ACPI / 标准复位端口 */
    outb(0xCF9, 0x0E);
    outw(0x604, 0x2000);
    outw(0xB004, 0x2000);

    /* 复位失败兜底：打印提示后 cli;hlt */
    vga_puts("[PANIC] Reset failed, halting.\n");
    __asm__ volatile("cli");
    while (1) {
        __asm__ volatile("hlt");
    }
}
