#include "printk.h"
#include "vga.h"

/* 使用 GCC 内建 va_list（不依赖 libc 的 stdarg.h） */
typedef __builtin_va_list va_list_k;
#define va_start_k(ap, last) __builtin_va_start(ap, last)
#define va_arg_k(ap, type)   __builtin_va_arg(ap, type)
#define va_end_k(ap)         __builtin_va_end(ap)

#define LOG_ENTRIES   64
#define LOG_ENTRY_LEN 128

static char    log_text[LOG_ENTRIES][LOG_ENTRY_LEN];
static uint8_t log_level[LOG_ENTRIES];
static int     log_head  = 0;   /* 下一个写入位置 */
static int     log_count = 0;   /* 有效条目数（<= LOG_ENTRIES） */

/* 控制台过滤级别（默认 KERN_INFO；`quiet` 启动参数提升为 KERN_WARNING）。 */
static int g_console_level = KERN_INFO;

void printk_set_console_level(int level)
{
    g_console_level = level;
}

/* 将无符号数按 base 写入 out（从低位到高位反转），n 为当前游标 */
static void fmt_uint(char* out, int cap, int* n, uint64_t v, int base, int upper)
{
    const char* digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char tmp[24];
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = digits[v % base]; v /= base; }
    while (i && *n < cap - 1) out[(*n)++] = tmp[--i];
}

/* 内核版 vsnprintf：支持 %s %c %d %u %x %p %%。 */
static int vsnprintf_k(char* out, int cap, const char* fmt, va_list_k ap)
{
    int n = 0;
    for (const char* p = fmt; *p && n < cap - 1; p++) {
        if (*p != '%') { out[n++] = *p; continue; }
        p++;
        switch (*p) {
        case 's': {
            const char* s = va_arg_k(ap, const char*);
            if (!s) s = "(null)";
            while (*s && n < cap - 1) out[n++] = *s++;
            break;
        }
        case 'c': {
            char c = (char)va_arg_k(ap, int);
            if (n < cap - 1) out[n++] = c;
            break;
        }
        case 'd': {
            int v = va_arg_k(ap, int);
            if (v < 0) { if (n < cap - 1) out[n++] = '-'; v = -v; }
            fmt_uint(out, cap, &n, (uint64_t)v, 10, 0);
            break;
        }
        case 'u':
            fmt_uint(out, cap, &n, va_arg_k(ap, uint64_t), 10, 0);
            break;
        case 'x':
            fmt_uint(out, cap, &n, va_arg_k(ap, uint64_t), 16, 0);
            break;
        case 'p':
            if (n + 2 < cap - 1) { out[n++] = '0'; out[n++] = 'x'; }
            fmt_uint(out, cap, &n, va_arg_k(ap, uint64_t), 16, 0);
            break;
        case '%':
            if (n < cap - 1) out[n++] = '%';
            break;
        default:
            if (n < cap - 1) out[n++] = '%';
            if (*p && n < cap - 1) out[n++] = *p;
            break;
        }
    }
    out[n] = '\0';
    return n;
}

void printk_init(void)
{
    for (int i = 0; i < LOG_ENTRIES; i++) {
        log_text[i][0] = '\0';
        log_level[i] = KERN_DEBUG;
    }
    log_head  = 0;
    log_count = 0;
}

void printk(int level, const char* fmt, ...)
{
    char line[LOG_ENTRY_LEN];
    va_list_k ap;
    va_start_k(ap, fmt);
    vsnprintf_k(line, sizeof(line), fmt, ap);
    va_end_k(ap);

    /* 写入环形缓冲（总是记录） */
    int idx = log_head;
    int i = 0;
    while (line[i] && i < LOG_ENTRY_LEN - 1) { log_text[idx][i] = line[i]; i++; }
    log_text[idx][i] = '\0';
    log_level[idx] = (uint8_t)level;
    log_head = (log_head + 1) % LOG_ENTRIES;
    if (log_count < LOG_ENTRIES) log_count++;

    /* 控制台输出（按过滤级别） */
    if (level <= g_console_level) vga_puts(line);
}

void printk_dump(void (*emit)(const char* s))
{
    if (!emit) return;
    int start = log_head - log_count;
    if (start < 0) start += LOG_ENTRIES;

    for (int i = 0; i < log_count; i++) {
        int idx = (start + i) % LOG_ENTRIES;
        char prefix[8];
        int n = 0;
        prefix[n++] = '<';
        int lv = log_level[idx];
        if (lv >= 10) prefix[n++] = (char)('0' + lv / 10);
        prefix[n++] = (char)('0' + lv % 10);
        prefix[n++] = '>';
        prefix[n] = '\0';
        emit(prefix);
        emit(log_text[idx]);
        emit("\n");
    }
}
