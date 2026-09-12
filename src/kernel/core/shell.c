#include "shell.h"
#include "vga.h"
#include "printk.h"
#include "mm.h"
#include "mm/vm.h"
#include "panic.h"
#include "port.h"
#include "boomerang.h"
#include "sched.h"
#include "doorbell.h"
#include "ipc.h"
#include "compress.h"
#include "power/freq.h"
#include "power/policy.h"
#include "power/cstate.h"
#include "ap.h"
#include "fs/vfs.h"
#include "fs/devfs.h"
#include "fs/tmpfs.h"
#include "fs/ext2.h"
#include "elf/loader.h"
#include "gdt.h"

#define MAX_CMDS    32
#define ALLOC_TRACK 1024

typedef struct {
    const char* name;
    const char* desc;
    void (*func)(int argc, char** argv);
} shell_cmd_t;

static shell_cmd_t cmd_table[MAX_CMDS];
static int cmd_count = 0;

/* Track allocations for free command */
static void* alloc_track[ALLOC_TRACK];
static int alloc_track_count = 0;

static void cmd_help(int argc, char** argv);
static void cmd_hello(int argc, char** argv);
static void cmd_mem(int argc, char** argv);
static void cmd_alloc(int argc, char** argv);
static void cmd_free(int argc, char** argv);
static void cmd_clear(int argc, char** argv);
static void cmd_reboot(int argc, char** argv);
static void cmd_panic(int argc, char** argv);
static void cmd_echo(int argc, char** argv);
static void cmd_stats(int argc, char** argv);
static void cmd_compress(int argc, char** argv);
static void cmd_ps(int argc, char** argv);
static void cmd_kill(int argc, char** argv);
static void cmd_ipc(int argc, char** argv);
static void cmd_power(int argc, char** argv);
static void cmd_run(int argc, char** argv);
static void cmd_jobs(int argc, char** argv);
static void cmd_fg(int argc, char** argv);
static void cmd_bg(int argc, char** argv);
static void cmd_ls(int argc, char** argv);
static void cmd_cat(int argc, char** argv);
static void cmd_dmesg(int argc, char** argv);
static void cmd_fsalloc(int argc, char** argv);
static void cmd_fsfree(int argc, char** argv);
static void cmd_wfile(int argc, char** argv);

/* Helper to register a single command */
static void shell_register(const char* name, const char* desc, void (*func)(int, char**)) {
    if (cmd_count >= MAX_CMDS) return;
    cmd_table[cmd_count].name = name;
    cmd_table[cmd_count].desc = desc;
    cmd_table[cmd_count].func = func;
    cmd_count++;
}

/* ---- 内核 shell 行编辑：命令历史（环形）+ Tab 命令名补全 ---- */
#define SHELL_HIST_MAX 16
static char shell_hist[SHELL_HIST_MAX][CMD_MAX_LEN];
static int  shell_hist_count = 0;
static int  shell_hist_next = 0;

void shell_hist_add(const char* line)
{
    if (!line || !line[0]) return;
    int last = (shell_hist_next + SHELL_HIST_MAX - 1) % SHELL_HIST_MAX;
    if (shell_hist_count > 0) {
        int same = 1, k = 0;
        while (shell_hist[last][k] && line[k]) {
            if (shell_hist[last][k] != line[k]) { same = 0; break; }
            k++;
        }
        if (same && shell_hist[last][k] == line[k]) return;  /* 去重 */
    }
    int i = 0;
    while (line[i] && i < CMD_MAX_LEN - 1) {
        shell_hist[shell_hist_next][i] = line[i];
        i++;
    }
    shell_hist[shell_hist_next][i] = '\0';
    shell_hist_next = (shell_hist_next + 1) % SHELL_HIST_MAX;
    if (shell_hist_count < SHELL_HIST_MAX) shell_hist_count++;
}

const char* shell_hist_get(int offset)
{
    if (offset < 0 || offset >= shell_hist_count) return NULL;
    int idx = (shell_hist_next + SHELL_HIST_MAX - 1 - offset) % SHELL_HIST_MAX;
    return shell_hist[idx];
}

int shell_tab_complete(const char* prefix, char* out, int max)
{
    if (!prefix || !out || max <= 0) return 0;
    int plen = 0;
    while (prefix[plen]) plen++;
    int n = 0;
    out[0] = '\0';
    for (int i = 0; i < cmd_count; i++) {
        const char* name = cmd_table[i].name;
        int k = 0;
        while (k < plen && name[k] && name[k] == prefix[k]) k++;
        if (k != plen) continue;
        if (n == 0) {
            int m = 0;
            while (name[m] && m < max - 1) { out[m] = name[m]; m++; }
            out[m] = '\0';
        } else {
            int m = 0;
            while (out[m] && name[m] && out[m] == name[m]) m++;
            out[m] = '\0';
        }
        n++;
    }
    return n;
}

void shell_init(void) {
    cmd_count = 0;
    alloc_track_count = 0;

    shell_register("help",     "Show available commands",              cmd_help);
    shell_register("hello",    "Print 'Hello, world!'",                cmd_hello);
    shell_register("mem",      "Show memory statistics",               cmd_mem);
    shell_register("alloc",    "Allocate N pages",                     cmd_alloc);
    shell_register("free",     "Free N pages",                         cmd_free);
    shell_register("clear",    "Clear the screen",                     cmd_clear);
    shell_register("reboot",   "Reboot the system",                    cmd_reboot);
    shell_register("panic",    "Trigger kernel panic",                 cmd_panic);
    shell_register("echo",     "Echo arguments",                       cmd_echo);
    shell_register("stats",    "Show zone usage & compression ratio",  cmd_stats);
    shell_register("compress", "Manually compress a page",             cmd_compress);
    shell_register("ps",       "Show task list",                       cmd_ps);
    shell_register("kill",     "Kill task <id>",                       cmd_kill);
    shell_register("ipc",      "Send/recv IPC <ch> [msg]",             cmd_ipc);
    shell_register("power",    "Power status/strategy/freq",           cmd_power);
    shell_register("run",      "Load and execute ELF file",            cmd_run);
    shell_register("jobs",     "List background jobs",                 cmd_jobs);
    shell_register("fg",       "Run job <pid> in foreground",          cmd_fg);
    shell_register("bg",       "Resume job <pid> in background",       cmd_bg);
    shell_register("ls",       "List directory contents",              cmd_ls);
    shell_register("cat",      "Show file contents",                   cmd_cat);
    shell_register("dmesg",    "Show kernel log ring buffer",          cmd_dmesg);
    shell_register("fsalloc",  "EXT2: alloc 1 inode + 1 block",        cmd_fsalloc);
    shell_register("fsfree",   "EXT2: free <ino> <blk>",               cmd_fsfree);
    shell_register("wfile",    "EXT2: wfile <path> <offset> <data>",   cmd_wfile);
}

void shell_prompt(void) {
    vga_puts("> ");
}

static int strcmp_s(const char* a, const char* b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return *a - *b;
}

static int atoi_s(const char* s) {
    int n = 0;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
    return n;
}

/* Print unsigned integer */
static void print_uint(uint64_t n) {
    if (n == 0) { vga_putc('0'); return; }
    char rev[32]; int rp = 0;
    while (n) { rev[rp++] = '0' + (n % 10); n /= 10; }
    while (rp) vga_putc(rev[--rp]);
}

void shell_execute(const char* line) {
    char  cmd_line[CMD_MAX_LEN];
    char* argv[CMD_MAX_ARGS];
    int   argc = 0;
    int   i;

    for (i = 0; line[i] && i < CMD_MAX_LEN - 1; i++) cmd_line[i] = line[i];
    cmd_line[i] = '\0';

    char* p = cmd_line;
    while (*p && argc < CMD_MAX_ARGS) {
        while (*p == ' ') p++;
        if (*p == '\0') break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p == ' ') { *p = '\0'; p++; }
    }

    if (argc == 0) return;

    for (i = 0; i < cmd_count; i++) {
        if (strcmp_s(cmd_table[i].name, argv[0]) == 0) {
            cmd_table[i].func(argc, argv);
            return;
        }
    }

    vga_puts("Unknown command: ");
    vga_puts(argv[0]);
    vga_puts("\n");
}

static void cmd_help(int argc, char** argv) {
    (void)argc; (void)argv;
    vga_puts("Axion-Ban Kernel v0.1\nAvailable commands:\n");
    for (int i = 0; i < cmd_count; i++) {
        vga_puts("  ");
        vga_puts(cmd_table[i].name);
        int len = 0; const char* s = cmd_table[i].name; while (*s++) len++;
        for (int j = len; j < 12; j++) vga_putc(' ');
        vga_puts(cmd_table[i].desc);
        vga_puts("\n");
    }
}

static void cmd_hello(int argc, char** argv) {
    (void)argc; (void)argv;
    vga_puts("Hello, world!\n");
}

static void cmd_mem(int argc, char** argv) {
    (void)argc; (void)argv;
    uint64_t total_phys, equiv_total, hot_used, warm_used, boom_used, warm_equiv, boom_equiv;
    mm_stats_full(&total_phys, &equiv_total, &hot_used, &warm_used, &boom_used, &warm_equiv, &boom_equiv);

    uint64_t phys_mb = total_phys * PAGE_SIZE / (1024 * 1024);
    uint64_t hot_mb  = HOT_ZONE_PAGES * PAGE_SIZE / (1024 * 1024);
    uint64_t warm_mb = WARM_ZONE_PAGES * PAGE_SIZE / (1024 * 1024);
    uint64_t boom_mb = BOOMERANG_PAGES * PAGE_SIZE / (1024 * 1024);

    vga_puts("Phys Memory: "); print_uint(phys_mb); vga_puts("MB\n");
    vga_puts("Hot Zone:  "); print_uint(hot_mb); vga_puts("MB (used ");
    print_uint(hot_used * PAGE_SIZE / (1024 * 1024)); vga_puts("MB)\n");
    vga_puts("Warm Zone: "); print_uint(warm_mb); vga_puts("MB (comp equiv ");
    print_uint(warm_equiv * PAGE_SIZE / (1024 * 1024)); vga_puts("MB)\n");
    vga_puts("Boomerang: "); print_uint(boom_mb); vga_puts("MB (comp equiv ");
    print_uint(boom_equiv * PAGE_SIZE / (1024 * 1024)); vga_puts("MB)\n");
    vga_puts("Equiv Total: ~"); print_uint(equiv_total * PAGE_SIZE / (1024 * 1024)); vga_puts("MB\n");
}

static void cmd_alloc(int argc, char** argv) {
    if (argc < 2) { vga_puts("Usage: alloc <N>\n"); return; }
    int n = atoi_s(argv[1]);
    if (n <= 0) { vga_puts("Invalid number of pages\n"); return; }
    int ok = 0;
    for (int i = 0; i < n; i++) {
        void* p = pmalloc();
        if (!p) {
            /* 热区不足, 尝试驱逐冷页 */
            int evicted = boomerang_evict_cold();
            if (evicted > 0) {
                p = pmalloc();
            }
        }
        if (!p) {
            vga_puts("OOM: hot zone exhausted. Allocated ");
            print_uint(ok); vga_puts(" of "); print_uint(n); vga_puts(" pages\n");
            return;
        }
        if (alloc_track_count < ALLOC_TRACK) {
            alloc_track[alloc_track_count++] = p;
        }
        ok++;
    }
    vga_puts("Allocated "); print_uint(ok); vga_puts(" pages\n");
}

static void cmd_free(int argc, char** argv) {
    if (argc < 2) { vga_puts("Usage: free <N>\n"); return; }
    int n = atoi_s(argv[1]);
    if (n <= 0) { vga_puts("Invalid number of pages\n"); return; }
    if (n > alloc_track_count) n = alloc_track_count;
    int freed = 0;
    for (int i = 0; i < n; i++) {
        int idx = alloc_track_count - 1 - i;
        if (alloc_track[idx] != NULL) {
            pfree(alloc_track[idx]);
            alloc_track[idx] = NULL;
            freed++;
        }
    }
    alloc_track_count -= n;
    if (alloc_track_count < 0) alloc_track_count = 0;
    vga_puts("Freed "); print_uint(freed); vga_puts(" pages\n");
}

static void cmd_clear(int argc, char** argv) {
    (void)argc; (void)argv;
    vga_clear();
}

static void cmd_reboot(int argc, char** argv) {
    (void)argc; (void)argv;
    vga_puts("Rebooting...\n");
    while (inb(0x64) & 0x02) {}
    outb(0x64, 0xFE);
    __asm__ volatile("lidt 0; int3");
    while (1) { __asm__ volatile("hlt"); }
}

static void cmd_panic(int argc, char** argv) {
    (void)argc; (void)argv;
    kernel_panic("Manual panic");
}

static void cmd_echo(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) vga_putc(' ');
        vga_puts(argv[i]);
    }
    vga_puts("\n");
}

static void cmd_stats(int argc, char** argv) {
    (void)argc; (void)argv;
    uint64_t total_phys, equiv_total, hot_used, warm_used, boom_used, warm_equiv, boom_equiv;
    mm_stats_full(&total_phys, &equiv_total, &hot_used, &warm_used, &boom_used, &warm_equiv, &boom_equiv);

    uint64_t hot_total = HOT_ZONE_PAGES;
    uint64_t warm_total = WARM_ZONE_PAGES;
    uint64_t boom_total = BOOMERANG_PAGES;

    float ratio = compress_ratio();

    vga_puts("=== Memory Zone Statistics ===\n");
    
    vga_puts("Hot Zone:    "); print_uint(hot_total * PAGE_SIZE / (1024*1024));
    vga_puts("MB total,  "); print_uint(hot_used * PAGE_SIZE / (1024*1024));
    vga_puts("MB used,  ");
    if (hot_total > 0) {
        print_uint(hot_used * 100 / hot_total);
    } else { vga_putc('0'); }
    vga_puts("% usage\n");

    vga_puts("Warm Zone:   "); print_uint(warm_total * PAGE_SIZE / (1024*1024));
    vga_puts("MB total,  "); print_uint(warm_used);
    vga_puts(" pages,  compressed equiv: ");
    print_uint(warm_equiv * PAGE_SIZE / (1024*1024));
    vga_puts("MB,  ratio: ");
    if (ratio > 0.0f) {
        print_uint((uint64_t)(ratio * 10));
        vga_putc('.');
        print_uint((uint64_t)(ratio * 10) % 10);
        vga_putc('x');
    } else {
        vga_puts("2.5x");
    }
    vga_puts("\n");

    vga_puts("Boomerang:   "); print_uint(boom_total * PAGE_SIZE / (1024*1024));
    vga_puts("MB total,  "); print_uint(boom_used);
    vga_puts(" pages,  compressed equiv: ");
    print_uint(boom_equiv * PAGE_SIZE / (1024*1024));
    vga_puts("MB,  ratio: ");
    if (ratio > 0.0f) {
        print_uint((uint64_t)(ratio * 10));
        vga_putc('.');
        print_uint((uint64_t)(ratio * 10) % 10);
        vga_putc('x');
    } else {
        vga_puts("2.5x");
    }
    vga_puts("\n");

    vga_puts("----------------------------------------\n");
    vga_puts("Equiv Total: ~"); print_uint(equiv_total * PAGE_SIZE / (1024*1024));
    vga_puts("MB  (Phys "); print_uint(total_phys * PAGE_SIZE / (1024*1024));
    vga_puts("MB)\n");
}

static void cmd_compress(int argc, char** argv) {
    if (argc < 2) { vga_puts("Usage: compress <addr>\n"); return; }
    
    /* Parse hex address */
    uint64_t addr = 0;
    const char* s = argv[1];
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) s += 2;
    while (*s) {
        addr <<= 4;
        if (*s >= '0' && *s <= '9') addr += *s - '0';
        else if (*s >= 'a' && *s <= 'f') addr += *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'F') addr += *s - 'A' + 10;
        else break;
        s++;
    }

    boomerang_manual_compress(addr);
}

static void cmd_ps(int argc, char** argv) {
    (void)argc; (void)argv;
    /* ps 基于 /proc 数据源（与用户态 cat /proc 一致） */
    file_t* f = vfs_open("/proc", 0);
    if (!f) {
        vga_puts("ps: /proc unavailable\n");
        return;
    }
    char buf[512];
    uint64_t bytes;
    while ((bytes = vfs_read(f, buf, sizeof(buf) - 1)) > 0) {
        buf[bytes] = '\0';
        vga_puts(buf);
        if (bytes < sizeof(buf) - 1) break;
    }
    vfs_close(f);
}

static void cmd_dmesg(int argc, char** argv) {
    (void)argc; (void)argv;
    printk_dump(vga_puts);
}

static void cmd_kill(int argc, char** argv) {
    if (argc < 2) { vga_puts("Usage: kill <task_id>\n"); return; }
    int tid = atoi_s(argv[1]);
    task_t* t = sched_get_task(tid);
    if (!t) {
        vga_puts("Task not found: ");
        print_uint((uint64_t)tid);
        vga_puts("\n");
        return;
    }
    sched_kill(tid);
    vga_puts("Killed task ");
    print_uint((uint64_t)tid);
    vga_puts("\n");
}

static void cmd_ipc(int argc, char** argv) {
    if (argc < 2) { vga_puts("Usage: ipc <channel> [msg]\n"); return; }
    int ch = atoi_s(argv[1]);
    
    if (argc >= 3) {
        /* Send mode */
        uint64_t val = (uint64_t)atoi_s(argv[2]);
        int result = doorbell_write(ch, val, 0);
        if (result == 0) {
            vga_puts("IPC sent to channel ");
            print_uint((uint64_t)ch);
            vga_puts("\n");
        } else {
            vga_puts("IPC failed (ACL denied or full)\n");
        }
    } else {
        /* Read mode */
        uint64_t val = doorbell_poll(ch);
        if (val != 0) {
            vga_puts("IPC recv ch"); print_uint((uint64_t)ch);
            vga_puts(": "); print_uint(val); vga_puts("\n");
        } else {
            vga_puts("No message on channel ");
            print_uint((uint64_t)ch);
            vga_puts("\n");
        }
    }
}

static void cmd_power(int argc, char** argv) {
    if (argc < 2) {
        vga_puts("Usage: power [status|strategy|freq]\n");
        return;
    }

    /* Simple string compare */
    const char* sub = argv[1];

    /* Check subcommand */
    if (sub[0] == 's' && sub[1] == 't' && sub[2] == 'a' && sub[3] == 't' && sub[4] == 'u' && sub[5] == 's' && sub[6] == '\0') {
        /* power status */
        vga_puts("=== Power Status ===\n");
        vga_puts("Frequency: "); print_uint(freq_get() / 1000); vga_puts(" MHz\n");
        vga_puts("Policy:    "); vga_puts(policy_name(policy_get()));
        if (policy_is_manual()) vga_puts(" (manual)");
        vga_puts("\n");
        vga_puts("Load:      "); print_uint((uint64_t)(policy_get_load() * 100)); vga_puts("%\n");
        vga_puts("\nCore States:\n");
        for (int i = 0; i < MAX_CORES; i++) {
            vga_puts("  Core "); vga_putc('0' + i); vga_puts(": ");
            int state = ap_get_state(i);
            vga_puts(cstate_state_name(state));
            vga_puts("\n");
        }
    } else if (sub[0] == 's' && sub[1] == 't' && sub[2] == 'r' && sub[3] == 'a') {
        /* power strategy */
        if (argc < 3) {
            vga_puts("Usage: power strategy <idle|light|balanced|performance>\n");
            return;
        }
        const char* name = argv[2];
        policy_t p;
        if (name[0] == 'i') p = POLICY_IDLE;
        else if (name[0] == 'l') p = POLICY_LIGHT;
        else if (name[0] == 'b') p = POLICY_BALANCED;
        else if (name[0] == 'p') p = POLICY_PERFORMANCE;
        else {
            vga_puts("Unknown strategy: "); vga_puts(name); vga_puts("\n");
            return;
        }
        policy_set_manual(p);
        vga_puts("Strategy set to "); vga_puts(policy_name(p)); vga_puts("\n");
    } else if (sub[0] == 'f' && sub[1] == 'r' && sub[2] == 'e' && sub[3] == 'q') {
        /* power freq */
        if (argc < 3) {
            vga_puts("Usage: power freq <khz>\n");
            vga_puts("Current: "); print_uint(freq_get()); vga_puts(" kHz\n");
            return;
        }
        uint32_t khz = (uint32_t)atoi_s(argv[2]);
        freq_set(khz);
        vga_puts("Frequency set to "); print_uint(freq_get() / 1000); vga_puts(" MHz\n");
    } else {
        vga_puts("Unknown subcommand: "); vga_puts(sub); vga_puts("\n");
        vga_puts("Usage: power [status|strategy|freq]\n");
    }
}

/* ---- Task 3.1: 最小 job control（jobs / fg / bg） ----
 * 同步执行模型下 user_run 返回即进程退出，因此"后台"仅体现在 job 表
 * 的状态记录上：run ... & 登记 job 并打印作业号，跑完后置 Done；
 * fg 重新同步运行该 job 的路径；bg 把 Done 的 job 标回 Running。 */
#define MAX_JOBS   16
#define JOB_RUNNING 0
#define JOB_DONE    1

typedef struct {
    int      in_use;
    int      job_no;        /* 作业号（jobs 列表显示） */
    uint64_t pid;
    char     path[64];
    int      state;         /* JOB_RUNNING / JOB_DONE */
} job_t;

static job_t jobs[MAX_JOBS];
static int   next_job_no = 1;

/* 查找 job 表空槽，返回下标或 -1（表满）。 */
static int job_alloc_slot(void)
{
    for (int i = 0; i < MAX_JOBS; i++)
        if (!jobs[i].in_use) return i;
    return -1;
}

/* 按 pid 查找 job，返回下标或 -1。 */
static int job_find_by_pid(uint64_t pid)
{
    for (int i = 0; i < MAX_JOBS; i++)
        if (jobs[i].in_use && jobs[i].pid == pid) return i;
    return -1;
}

/* 登记一个 job；返回作业号，-1 = 表满。 */
static int job_add(uint64_t pid, const char* path)
{
    int slot = job_alloc_slot();
    if (slot < 0) return -1;
    jobs[slot].in_use = 1;
    jobs[slot].job_no = next_job_no++;
    jobs[slot].pid = pid;
    jobs[slot].state = JOB_RUNNING;
    int i = 0;
    while (path[i] && i < (int)sizeof(jobs[slot].path) - 1) {
        jobs[slot].path[i] = path[i];
        i++;
    }
    jobs[slot].path[i] = '\0';
    return jobs[slot].job_no;
}

/* 前台任务 pid（0 = 无）。由 cmd_run 设置，kmain 主循环在任务退出后
 * 恢复内核 Shell 输入。 */
uint64_t g_fg_pid = 0;

/* 启动一个用户程序（入队到协作调度器，立即返回，不同步运行）。
 * background=1 时登记 job。返回 pid（0 = 启动失败）。 */
static uint64_t run_program(int argc, char** argv, int background)
{
    const char* path = argv[1];

    /* Check if it's a device file (not executable) */
    if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' && path[3] == 'v') {
        vga_puts("Error: Not a valid executable\n");
        return 0;
    }

    /* Load ELF */
    elf_context_t ctx;
    if (elf_load(path, &ctx) < 0) {
        return 0;  /* Error already printed by elf_load */
    }

    /* Build the initial user stack: argv[0] = program path, extra args passed
     * through (e.g. `run /bin/bash -c /bin/hello`). */
    const char* uargv[CMD_MAX_ARGS];
    int uargc = 0;
    uargv[uargc++] = path;
    for (int a = 2; a < argc && uargc < CMD_MAX_ARGS - 1; a++)
        uargv[uargc++] = argv[a];
    uargv[uargc] = NULL;
    uint64_t rsp = 0;
    /* W7 (Phase 3.2): 注入基础环境。LD_LIBRARY_PATH 让 ld.so 能找到 multiarch
     * 库目录（ld.so.cache 尚未被内核 mmap 完整支持时的重要兜底）。 */
    const char* uenv[7];
    uenv[0] = "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    uenv[1] = "HOME=/home/banana";
    uenv[2] = "TERM=xterm";
    uenv[3] = "SHELL=/bin/bash";
    uenv[4] = "LD_LIBRARY_PATH=/lib/x86_64-linux-gnu:/usr/lib/x86_64-linux-gnu";
    uenv[5] = NULL;
    if (elf_build_user_stack(&ctx, uargc, uargv, uenv, &rsp) < 0) {
        vga_puts("Error: Failed to build user stack\n");
        return 0;
    }

    /* 创建用户进程并入队，由协作调度器运行（不再同步 user_run）。 */
    task_t* t = sched_spawn_process(ctx.vmctx, ctx.entry, rsp);
    if (!t) {
        vga_puts("Error: Failed to create user process\n");
        return 0;
    }

    vga_puts("Running ");
    vga_puts(path);
    vga_puts(" (pid ");
    print_uint((uint64_t)t->pid);
    vga_puts(")\n");

    /* W7 (Task 4.5): 前台程序运行时，把 tty 前台进程组设为它的 pgrp。
     * bash 5.2 的 initialize_job_control 先比较 tcgetpgrp(tty) 与自身
     * pgrp，不一致就打印 "no job control in background" 并放弃 job
     * control——启动时必须由内核（代替 login/init 角色）设置。 */
    if (!background)
        devfs_tty_set_pgrp((uint32_t)t->pid);

    if (background) {
        int job_no = job_add(t->pid, path);
        if (job_no > 0) {
            vga_puts("Started job ");
            print_uint((uint64_t)job_no);
            vga_puts(" (pid ");
            print_uint((uint64_t)t->pid);
            vga_puts(") in background\n");
        }
    }
    return t->pid;
}

static void cmd_run(int argc, char** argv) {
    if (argc < 2) {
        vga_puts("Usage: run <path> [args] [&]\n");
        return;
    }

    /* 解析尾随 '&'：剥离并标记后台执行 */
    int background = 0;
    if (strcmp_s(argv[argc - 1], "&") == 0) {
        background = 1;
        argv[argc - 1] = NULL;
        argc--;
    }

    uint64_t pid = run_program(argc, argv, background);
    if (pid && !background)
        g_fg_pid = pid;   /* 前台：主循环等它退出 */
}

/* W7: 按路径启动用户程序（前台），供 kmain 自动运行 bash。返回 pid。 */
uint64_t shell_launch_program(const char* path, int extra_argc,
                              const char** extra_argv)
{
    char* argv[CMD_MAX_ARGS];
    int argc = 0;
    argv[argc++] = (char*)"run";
    argv[argc++] = (char*)path;
    for (int a = 0; a < extra_argc && argc < CMD_MAX_ARGS - 1; a++)
        argv[argc++] = (char*)extra_argv[a];
    argv[argc] = NULL;
    uint64_t pid = run_program(argc, argv, 0);
    if (pid) g_fg_pid = pid;
    return pid;
}

static void cmd_jobs(int argc, char** argv) {
    (void)argc; (void)argv;
    int shown = 0;
    for (int i = 0; i < MAX_JOBS; i++) {
        if (!jobs[i].in_use) continue;
        vga_puts("[");
        print_uint((uint64_t)jobs[i].job_no);
        vga_puts("] ");
        print_uint(jobs[i].pid);
        vga_puts(" ");
        vga_puts(jobs[i].path);
        vga_puts(" ");
        vga_puts(jobs[i].state == JOB_RUNNING ? "Running" : "Done");
        vga_puts("\n");
        shown++;
    }
    if (shown == 0) vga_puts("No jobs\n");
}

static void cmd_fg(int argc, char** argv) {
    if (argc < 2) { vga_puts("Usage: fg <pid>\n"); return; }
    uint64_t pid = (uint64_t)atoi_s(argv[1]);
    int slot = job_find_by_pid(pid);
    if (slot < 0) {
        vga_puts("fg: job with pid ");
        print_uint(pid);
        vga_puts(" not found\n");
        return;
    }
    /* 重新运行该 job 的路径（原进程已退出，直接再跑一遍，前台）。 */
    char* fg_argv[2];
    fg_argv[0] = "fg";
    fg_argv[1] = jobs[slot].path;
    uint64_t npid = run_program(2, fg_argv, 0);
    if (npid) {
        g_fg_pid = npid;
        jobs[slot].pid = npid;
    }
}

static void cmd_bg(int argc, char** argv) {
    if (argc < 2) { vga_puts("Usage: bg <pid>\n"); return; }
    uint64_t pid = (uint64_t)atoi_s(argv[1]);
    int slot = job_find_by_pid(pid);
    if (slot < 0) {
        vga_puts("bg: job with pid ");
        print_uint(pid);
        vga_puts(" not found\n");
        return;
    }
    jobs[slot].state = JOB_RUNNING;
    vga_puts("Job ");
    print_uint((uint64_t)jobs[slot].job_no);
    vga_puts(" (pid ");
    print_uint(pid);
    vga_puts(") in background\n");
}

static void cmd_ls(int argc, char** argv) {
    const char* path = (argc >= 2) ? argv[1] : "/";

    char buf[1024];
    int len = vfs_list_dir(path, buf, sizeof(buf));
    if (len < 0) {
        vga_puts("ls: "); vga_puts(path); vga_puts(": No such directory\n");
        return;
    }
    if (len == 0) {
        vga_puts("(empty)\n");
        return;
    }
    for (int i = 0; i < len; i++) {
        vga_putc(buf[i]);
    }
    vga_puts("\n");
}

static void cmd_cat(int argc, char** argv) {
    if (argc < 2) {
        vga_puts("Usage: cat <path>\n");
        return;
    }

    const char* path = argv[1];

    file_t* f = vfs_open(path, 0);
    if (!f) {
        vga_puts("cat: "); vga_puts(path); vga_puts(": File not found\n");
        return;
    }

    /* Read and display file content */
    char buf[256];
    uint64_t bytes;
    while ((bytes = vfs_read(f, buf, sizeof(buf) - 1)) > 0) {
        buf[bytes] = '\0';
        vga_puts(buf);
        if (bytes < sizeof(buf) - 1) break;
    }
    vga_puts("\n");
}

/* ================= EXT2 写路径调试命令 ================= */

static void cmd_fsalloc(int argc, char** argv) {
    (void)argc; (void)argv;
    uint32_t fi, fb;
    if (ext2_free_counts(&fi, &fb) < 0) {
        vga_puts("fsalloc: ext2 not mounted\n");
        return;
    }
    vga_puts("free before: inodes="); print_uint(fi);
    vga_puts(" blocks="); print_uint(fb); vga_puts("\n");

    int ino = ext2_alloc_inode();
    uint32_t blk = ext2_alloc_block();

    vga_puts("alloc inode="); print_uint((uint64_t)(ino < 0 ? 0 : (uint32_t)ino));
    vga_puts(" block="); print_uint(blk); vga_puts("\n");

    ext2_free_counts(&fi, &fb);
    vga_puts("free after:  inodes="); print_uint(fi);
    vga_puts(" blocks="); print_uint(fb); vga_puts("\n");
}

static void cmd_fsfree(int argc, char** argv) {
    if (argc < 3) {
        vga_puts("Usage: fsfree <ino> <blk>\n");
        return;
    }
    uint32_t ino = (uint32_t)atoi_s(argv[1]);
    uint32_t blk = (uint32_t)atoi_s(argv[2]);
    int ri = ext2_free_inode(ino);
    int rb = ext2_free_block(blk);
    vga_puts("free inode ");
    print_uint(ino);
    vga_puts(": "); vga_puts(ri == 0 ? "ok" : "fail");
    vga_puts(", free block ");
    print_uint(blk);
    vga_puts(": "); vga_puts(rb == 0 ? "ok" : "fail");
    vga_puts("\n");

    uint32_t fi, fb;
    if (ext2_free_counts(&fi, &fb) == 0) {
        vga_puts("free now: inodes="); print_uint(fi);
        vga_puts(" blocks="); print_uint(fb); vga_puts("\n");
    }
}

static void cmd_wfile(int argc, char** argv) {
    if (argc < 4) {
        vga_puts("Usage: wfile <path> <offset> <data>\n");
        return;
    }
    const char* path = argv[1];
    uint32_t offset = (uint32_t)atoi_s(argv[2]);
    const char* data = argv[3];

    uint32_t ino;
    if (ext2_lookup_path(path, &ino) < 0) {
        vga_puts("wfile: "); vga_puts(path); vga_puts(": not found\n");
        return;
    }

    uint32_t len = 0;
    while (data[len]) len++;
    int r = ext2_write_file(ino, offset, data, len);
    if (r < 0) {
        vga_puts("wfile: write failed\n");
        return;
    }
    vga_puts("wfile: wrote ");
    print_uint(len);
    vga_puts(" bytes at offset ");
    print_uint(offset);
    vga_puts(" to ");
    vga_puts(path);
    vga_puts(" (ino ");
    print_uint(ino);
    vga_puts(")\n");

    uint32_t sz = 0;
    ext2_inode_size(ino, &sz);
    vga_puts("wfile: i_size now ");
    print_uint(sz);
    vga_puts("\n");
}