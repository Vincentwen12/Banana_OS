#include "axion.h"
#include "port.h"
#include "vga.h"
#include "printk.h"
#include "keyboard.h"
#include "timer.h"
#include "mm.h"
#include "mm/vm.h"
#include "hotness.h"
#include "sched.h"
#include "doorbell.h"
#include "boomerang.h"
#include "compress.h"
#include "shell.h"
#include "panic.h"
#include "ap.h"
#include "power/freq.h"
#include "power/policy.h"
#include "power/cstate.h"
#include "syscall/syscall.h"
#include "syscall/table.h"
#include "fs/vfs.h"
#include "fs/devfs.h"
#include "fs/tmpfs.h"
#include "fs/procfs.h"
#include "fs/sysfs.h"
#include "elf/loader.h"
#include "dev/ata.h"
#include "fs/ext2.h"
#include "gdt.h"

void hello_elf_init(void);
void idt_init(void);   /* core/idt.c */

/* W7 (Task 2.5 / 2.4): 信号 restorer 桩初始化（syscall/signal.c）与
 * 用户数据库解析（syscall/table.c users_init）。 */
void signal_init(void);
void users_init(void);

/* 禁用 8259 PIC */
static void pic_disable(void) {
    outb(0x20, 0x11);
    outb(0xA0, 0x11);
    outb(0x21, 0x20);
    outb(0xA1, 0x28);
    outb(0x21, 0x04);
    outb(0xA1, 0x02);
    outb(0x21, 0x01);
    outb(0xA1, 0x01);
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

static uint64_t last_age_ms = 0;

/* W7: 协作式调度主循环的内核 Shell 状态。 */
static char     shell_buf[CMD_MAX_LEN];
static int      shell_pos = 0;
static int      shell_active = 0;   /* 内核 Shell 正在收集输入 */
/* 内核 Shell 行编辑：ESC 序列状态机 + 历史导航（见 shell.h/shell.c） */
#define SHELL_HIST_MAX 16
static int      shell_esc_state = 0;   /* 0=无 1=收到ESC 2=收到ESC[ */
static int      shell_hist_pos = -1;   /* -1=编辑新输入，否则浏览历史 */
static char     shell_hist_save[CMD_MAX_LEN];

/* bash 登录循环：g_bash_pid 记录当前 bash（退出后自动重启），
 * g_bash_fails 统计连续重启失败次数（>=3 回落内核 Shell）。 */
static uint64_t g_bash_pid = 0;
static int      g_bash_fails = 0;

/* sched.c：把无父进程回收的 ZOMBIE（如退出的 bash）置为 REAPED 释放槽位 */
void sched_force_reap(uint64_t pid);

/* Demo task A: idle heartbeat (no output) */
static void demo_a_func(void) {
}

/* Demo task B: IPC relay */
static void demo_b_func(void) {
    /* Poll doorbell channel 1 for messages */
    uint64_t msg = doorbell_poll(1);
    if (msg != 0) {
        /* Echo back on channel 2 */
        doorbell_write(2, msg, 0);
    }
}

/* AP 核心入口: 由 trampoline 调用，跳过所有初始化，进入空闲循环
 *
 * 唤醒路径说明:
 *   1. AP 通过 MONITOR 监听专属 doorbell 通道的 data 字段
 *   2. MWAIT 使核心进入低功耗 C-state
 *   3. 当其他核心向该通道 doorbell_write() 时，写入 data 字段
 *   4. 缓存一致性协议检测到写入，自动唤醒 AP
 *   5. 执行从 MWAIT 下一条指令继续 (内联返回，非中断)
 *   6. AP 轮询 doorbell_poll() 获取消息并处理
 */
void ap_entry(int apic_id) {
    /* 通知 BSP 该核心已上线 */
    ap_core_online(apic_id);

    /* 串口输出 */
    outb(SERIAL_PORT, 'A');
    outb(SERIAL_PORT, 'P');
    outb(SERIAL_PORT, '0' + apic_id);
    outb(SERIAL_PORT, '\n');

    /* AP core has no parallel work yet — halt until an interrupt arrives. */
    while (1) {
        __asm__ volatile("hlt");
    }
}

/* Multiboot2 信息解析：扫描 tag，找 type==1 (boot command line)。
 * 命令行为 null 结尾字符串；解析出 "quiet" 则提升 printk 控制台级别，
 * 跳过全部 INFO 启动日志（加速启动）。无 cmdline/无 quiet 时行为不变。 */
static void kmain_parse_cmdline(void* multiboot_info)
{
    if (!multiboot_info) return;
    uint64_t base = (uint64_t)(uintptr_t)multiboot_info;
    uint32_t total = *(uint32_t*)base;
    uint64_t p = base + 8;   /* 跳过 total_size + reserved */

    while (p + 8 <= base + total) {
        uint32_t type = *(uint32_t*)p;
        uint32_t size = *(uint32_t*)(p + 4);
        if (type == 0) break;                       /* end tag */
        if (type == 1) {                            /* boot command line */
            const char* cmd = (const char*)(p + 8);
            const char* s = cmd;
            while (*s) {
                /* 匹配子串 "quiet"（按空格分隔的参数） */
                if ((s[0] == 'q' && s[1] == 'u' && s[2] == 'i' && s[3] == 'e' && s[4] == 't') &&
                    (s == cmd || s[-1] == ' ' || s[-1] == '\t') &&
                    (s[5] == '\0' || s[5] == ' ' || s[5] == '\t')) {
                    printk_set_console_level(KERN_WARNING);
                    return;
                }
                s++;
            }
            return;
        }
        /* 8 字节对齐步进 */
        p += (size + 7) & ~7ULL;
    }
}

void kmain(uint32_t magic, void* multiboot_info) {
    (void)magic;

    outb(SERIAL_PORT, 'K');  /* Kernel entry reached */
    outb(SERIAL_PORT, 'M');

    vga_init();
    printk_init();

    /* `quiet` 启动参数：解析 multiboot2 cmdline，提升控制台级别，
     * 跳过 INFO 启动日志（须在 banner/任何 [INIT] 输出之前）。 */
    kmain_parse_cmdline(multiboot_info);

    /* 重启早期检查上一次 panic 记录（vga_init 之后最早处） */
    panic_check_previous();

    /* 先初始化内存管理器，再安装 IDT：idt_init() 通过 pmalloc 从 Ω hot
     * zone 分配 IDT 页。若在 mm_init() 之前调用，mm_init() 会清零位图，
     * 导致后续 gdt_init() 的 TSS 页重新分配到同一物理页并覆盖 IDT 表。 */
    vga_puts("============================================\n");
    vga_puts("  Axion-Ban Kernel v0.1\n");
    vga_puts("  Architecture: x86-64 (Multiboot2)\n");
    vga_puts("============================================\n\n");

    /* 记录启动时间 */
    uint64_t tsc_start = rdtsc();

    printk(KERN_INFO, "[INIT] Disabling 8259 PIC...\n");
    pic_disable();

    printk(KERN_INFO, "[INIT] Initializing timer...\n");
    timer_init();

    printk(KERN_INFO, "[INIT] Initializing memory manager...\n");
    mm_init();

    /* Install the IDT after mm_init: previously ANY exception
     * triple-faulted into a silent reboot. Now faults print and halt. */
    idt_init();

    printk(KERN_INFO, "[INIT] Initializing hotness tracker...\n");
    hotness_init();

    printk(KERN_INFO, "[INIT] Initializing scheduler...\n");
    sched_init();
    
    printk(KERN_INFO, "[INIT] Initializing doorbell IPC...\n");
    doorbell_init();

    printk(KERN_INFO, "[INIT] Initializing XOR compression engine...\n");
    compress_init();

    printk(KERN_INFO, "[INIT] Initializing boomerang pool...\n");
    boomerang_init();

    printk(KERN_INFO, "[INIT] Initializing ATA block device...\n");
    ata_init();
    {
        static uint8_t ata_buf[ATA_SECTOR_SIZE];
        if (ata_read_sector(0, ata_buf) == 0) {
            if (ata_buf[510] == 0x55 && ata_buf[511] == 0xAA)
                printk(KERN_INFO, "[ATA] Sector 0 read OK (boot signature 0x55AA)\n");
            else
                printk(KERN_INFO, "[ATA] Sector 0 signature mismatch\n");
        } else {
            printk(KERN_INFO, "[ATA] Sector 0 read failed\n");
        }
    }

    printk(KERN_INFO, "[INIT] Mounting EXT2 filesystem...\n");
    ext2_mount();

    printk(KERN_INFO, "[INIT] Initializing keyboard...\n");
    kbd_init();

    printk(KERN_INFO, "[INIT] Initializing VFS...\n");
    vfs_init();

    printk(KERN_INFO, "[INIT] Initializing devfs...\n");
    devfs_init();

    printk(KERN_INFO, "[INIT] Initializing tmpfs...\n");
    tmpfs_init();
    sysfs_init();
    hello_elf_init();

    printk(KERN_INFO, "[INIT] Initializing GDT (Ring 3 segments)...\n");
    gdt_init();

    printk(KERN_INFO, "[INIT] Initializing syscall subsystem...\n");
    syscall_init();
    syscall_table_init();

    /* W7: 信号 restorer 桩（vm_create 需映射到用户地址空间）与用户
     * 数据库（解析 /etc/passwd，供 uid/gid 使用）。 */
    signal_init();
    users_init();

    printk(KERN_INFO, "[INIT] Initializing frequency control...\n");
    freq_init();

    printk(KERN_INFO, "[INIT] Initializing power policy engine...\n");
    policy_init();

    printk(KERN_INFO, "[INIT] Initializing C-state management...\n");
    cstate_init();

    printk(KERN_INFO, "[INIT] Initializing shell...\n");
    shell_init();

    /* Register Shell as a task for scheduling */
    task_t shell_task;
    shell_task.name = "Shell";
    shell_task.func = NULL;  /* Shell runs in the main loop, not as a task func */
    shell_task.core_id = 0;
    sched_add(&shell_task);

    /* Register demo tasks for scheduler testing */
    task_t demo_a;
    demo_a.name = "DemoA";
    demo_a.func = demo_a_func;
    demo_a.core_id = 0;
    sched_add(&demo_a);

    task_t demo_b;
    demo_b.name = "DemoB";
    demo_b.func = demo_b_func;
    demo_b.core_id = 0;
    sched_add(&demo_b);

    /* Set up doorbell ACL for IPC testing */
    doorbell_acl_set(0, 0, 1);  /* Allow Shell (task 0) to write channel 0 */
    doorbell_acl_set(1, 0, 1);  /* Allow Shell (task 0) to write channel 1 */
    doorbell_acl_set(2, 0, 1);  /* Allow Shell (task 0) to write channel 2 */

    /* 启动多核 (AP initialization) */
    printk(KERN_INFO, "[INIT] Initializing AP subsystem...\n");
    ap_init();
    ap_start_all();

    uint64_t tsc_end = rdtsc();
    uint64_t boot_ms = (tsc_end - tsc_start) / 2000000;
    sysfs_set_boot_ms(boot_ms);

    printk(KERN_INFO, "\n[BOOT] Kernel loaded OK\n");
    printk(KERN_INFO, "[BOOT] Boot time: ~%u ms\n\n", (unsigned)boot_ms);

    /* 启动 bash：作为用户任务入队，由协作调度器运行。
     * 安全模式（连续 panic >= 3 次）跳过 auto-run bash，仅保留内核 Shell。
     * bash 退出后由主循环自动重启（登录循环），保持始终有 shell 可用。 */
    if (panic_safe_mode()) {
        vga_puts("[SAFE MODE] skipping auto-run bash\n");
        shell_prompt();
        shell_active = 1;
    } else {
        g_bash_pid = shell_launch_program("/bin/bash", 0, NULL);
        g_fg_pid = g_bash_pid;
        if (!g_bash_pid) {
            vga_puts("[BOOT] bash launch failed, kernel shell\n");
            shell_prompt();
            shell_active = 1;
        }
    }

    /* ============ 主循环：协作式调度 + 内核 Shell 状态机 ============ */
    while (1) {
        int ran = 0;

        /* 1) 调度：运行下一个就绪任务 */
        int next_tid = sched_next();
        task_t* t = (next_tid >= 0) ? sched_get_task(next_tid) : NULL;
        if (t) {
            if (t->is_user) {
                ran = 1;
                current_task = t;
                syscall_kernel_top = t->kstack_top;
                vm_switch(t->mm_context);
                /* W7 3.2: 任务切换（非 syscall 路径）后恢复 FS/GS base MSR，
                   否则被上一任务 arch_prctl 覆盖的 TLS 基址会错位。 */
                syscall_restore_msrs();
                if (t->kctx.valid) {
                    /* 恢复该任务的退出返回上下文（阻塞期间被其它任务启动覆盖） */
                    user_ret_rsp = t->exit_ctx.rsp;
                    user_ret_rbx = t->exit_ctx.rbx;
                    user_ret_rbp = t->exit_ctx.rbp;
                    user_ret_r12 = t->exit_ctx.r12;
                    user_ret_r13 = t->exit_ctx.r13;
                    user_ret_r14 = t->exit_ctx.r14;
                    user_ret_r15 = t->exit_ctx.r15;
                    switch_context(&g_sched_kctx, &t->kctx);
                    /* 返回点 A：任务再次阻塞 */
                } else if (t->is_fork) {
                    fork_resume(&t->fork_ctx);
                    /* 返回点 B：任务退出 */
                } else {
                    sched_enter_user(t->entry, t->user_rsp);
                    /* 返回点 C：任务退出 */
                }
                if (t->state == TASK_STATE_BLOCKED) {
                    /* 阻塞：保存退出返回上下文 */
                    t->exit_ctx.rsp  = user_ret_rsp;
                    t->exit_ctx.rbx  = user_ret_rbx;
                    t->exit_ctx.rbp  = user_ret_rbp;
                    t->exit_ctx.r12  = user_ret_r12;
                    t->exit_ctx.r13  = user_ret_r13;
                    t->exit_ctx.r14  = user_ret_r14;
                    t->exit_ctx.r15  = user_ret_r15;
                    t->exit_ctx.valid = 1;
                } else if (t->state == TASK_STATE_ZOMBIE) {
                    /* 退出：唤醒等待它的父进程 */
                    sched_wake_waiting_parent(t);
                    current_task = NULL;
                }
            } else if (t->func) {
                ran = 1;
                t->func();
                if (t->state == TASK_STATE_RUNNING) sched_requeue(t);
            } else {
                /* 空闲任务（func=NULL）：不调度，保持就绪 */
                if (t->state == TASK_STATE_RUNNING) sched_requeue(t);
            }
        }

        /* 2) 唤醒条件轮询：键盘 / 定时器 / 信号 */
        if (kbd_has_key()) {
            sched_wake_kbd_all();
            sched_wake_select_all();
        }
        sched_poll_timeouts();
        sched_poll_signals();

        /* 3) 前台任务结束：bash 退出自动重启（登录循环）；其它程序退出回内核 Shell */
        if (g_fg_pid && !sched_task_alive(g_fg_pid)) {
            uint64_t exited_pid = g_fg_pid;
            g_fg_pid = 0;
            if (g_bash_pid && exited_pid == g_bash_pid) {
                /* 回收 bash 任务槽（无父进程 wait4，ZOMBIE 不会自行被 reap） */
                sched_force_reap(exited_pid);
                g_bash_pid = shell_launch_program("/bin/bash", 0, NULL);
                if (!g_bash_pid) {
                    g_bash_fails++;
                    if (g_bash_fails >= 3) {
                        vga_puts("[BOOT] bash restart failed, kernel shell\n");
                        shell_prompt();
                        shell_active = 1;
                    }
                } else {
                    g_bash_fails = 0;   /* shell_launch_program 已设 g_fg_pid */
                }
            } else {
                shell_prompt();
                shell_active = 1;
            }
        }

        /* 4) 内核 Shell 输入收集（仅当无前台任务时，非阻塞逐字符） */
        if (shell_active && !g_fg_pid) {
            char c;
            if (kbd_try_get(&c)) {
                if (shell_esc_state == 1) {
                    /* ESC [ 序列：方向键 */
                    if (c == '[') { shell_esc_state = 2; }
                    else { shell_esc_state = 0; }
                } else if (shell_esc_state == 2) {
                    shell_esc_state = 0;
                    if (c == 'A') {           /* ↑：历史向前 */
                        if (shell_hist_pos < 0) {
                            for (int i = 0; i < shell_pos && i < CMD_MAX_LEN - 1; i++)
                                shell_hist_save[i] = shell_buf[i];
                            shell_hist_save[shell_pos] = '\0';
                            shell_hist_pos = 0;
                        } else if (shell_hist_pos < SHELL_HIST_MAX - 1) {
                            shell_hist_pos++;
                        }
                        const char* h = shell_hist_get(shell_hist_pos);
                        if (!h) shell_hist_pos--;      /* 无更多历史 */
                        else {
                            while (shell_pos > 0) { shell_pos--; vga_puts("\b \b"); }
                            int i = 0;
                            while (h[i] && i < CMD_MAX_LEN - 1) {
                                shell_buf[i] = h[i]; vga_putc(h[i]); i++;
                            }
                            shell_pos = i; shell_buf[shell_pos] = '\0';
                        }
                    } else if (c == 'B') {    /* ↓：历史向后 */
                        if (shell_hist_pos > 0) shell_hist_pos--;
                        else if (shell_hist_pos == 0) shell_hist_pos = -1;
                        const char* h = (shell_hist_pos >= 0)
                            ? shell_hist_get(shell_hist_pos) : shell_hist_save;
                        while (shell_pos > 0) { shell_pos--; vga_puts("\b \b"); }
                        int i = 0;
                        while (h[i] && i < CMD_MAX_LEN - 1) {
                            shell_buf[i] = h[i]; vga_putc(h[i]); i++;
                        }
                        shell_pos = i; shell_buf[shell_pos] = '\0';
                    }
                } else if (c == 0x1B) {
                    shell_esc_state = 1;
                } else if (c == '\t') {       /* Tab：命令名补全 */
                    shell_buf[shell_pos] = '\0';
                    char comp[CMD_MAX_LEN];
                    if (shell_tab_complete(shell_buf, comp, sizeof(comp)) >= 1) {
                        int i = 0;
                        while (comp[i] && i < CMD_MAX_LEN - 1) {
                            shell_buf[i] = comp[i]; vga_putc(comp[i]); i++;
                        }
                        shell_pos = i; shell_buf[shell_pos] = '\0';
                    }
                } else if (c == '\n' || c == '\r') {
                    shell_buf[shell_pos] = '\0';
                    vga_puts("\n");
                    shell_pos = 0;
                    shell_hist_pos = -1;
                    shell_hist_add(shell_buf);
                    shell_active = 0;
                    if (shell_buf[0]) {
                        shell_execute(shell_buf);
                        /* shell_execute 可能通过 run 启动前台任务（g_fg_pid） */
                    }
                    if (!g_fg_pid) {
                        shell_prompt();
                        shell_active = 1;
                    }
                } else if (c == '\b' || c == 0x7F) {
                    if (shell_pos > 0) { shell_pos--; vga_puts("\b \b"); }
                } else if (c >= ' ' && c <= '~') {
                    if (shell_pos < CMD_MAX_LEN - 1) {
                        shell_buf[shell_pos++] = c;
                        vga_putc(c);
                    }
                }
            }
        }

        /* 5) 定时任务与功耗管理 */
        uint64_t now_ms = timer_ms();
        if (now_ms - last_age_ms >= 10) {
            hotness_age();
            last_age_ms = now_ms;
        }
        float load = sched_load_sample();
        policy_apply(load);
        if (doorbell_poll(0)) {
            /* Process IPC on channel 0 */
        }
        sched_tick();

        /* 6) 空载省电 */
        if (!ran) {
            for (int idle_i = 0; idle_i < 8; idle_i++)
                __asm__ volatile("pause");
        } else {
            __asm__ volatile("pause");
        }
    }
}