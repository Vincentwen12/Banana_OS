#include "devfs.h"
#include "vga.h"
#include "keyboard.h"
#include "port.h"
#include "sched.h"

/* /dev/null — read returns EOF, write discards */
static uint64_t null_read(void* f, uint64_t off, void* buf, uint64_t size)
{
    (void)f; (void)off; (void)buf; (void)size;
    return 0;  /* EOF */
}

static uint64_t null_write(void* f, uint64_t off, const void* buf, uint64_t size)
{
    (void)f; (void)off; (void)buf;
    return size;  /* Discard all data */
}

static uint64_t null_close(void* f) { (void)f; return 0; }

/* /dev/zero — read returns zeros, write discards */
static uint64_t zero_read(void* f, uint64_t off, void* buf, uint64_t size)
{
    (void)f; (void)off;
    for (uint64_t i = 0; i < size; i++) {
        ((uint8_t*)buf)[i] = 0;
    }
    return size;
}

static uint64_t zero_write(void* f, uint64_t off, const void* buf, uint64_t size)
{
    (void)f; (void)off; (void)buf;
    return size;
}

static uint64_t zero_close(void* f) { (void)f; return 0; }

/* /dev/tty — read from keyboard, write to VGA */

/* Terminal line-discipline state (glibc termios c_lflag/c_oflag bits).
 * Default: ISIG|ICANON|ECHO + OPOST|ONLCR, matching a real interactive tty.
 * bash/readline switches to raw mode (ECHO cleared) via TCSETS; when ECHO is
 * off the reader (readline) does its own echoing, so tty_read must not echo. */
#define TTY_ECHO    0x0008
#define TTY_ICANON  0x0002
static uint32_t tty_c_lflag = 0x000B;   /* ISIG|ICANON|ECHO */
static uint32_t tty_c_oflag = 0x0005;   /* OPOST|ONLCR */

/* ---- Task 3.1: per-tty job-control & line-discipline 状态 ----
 * tty_pgrp    : 前台进程组（TIOCSPGRP 写入，TIOCGPGRP 读回）。
 * tty_vmin/vtime : termios c_cc[VMIN]/c_cc[VTIME]（TCSETS 写入）。本内核
 *               无时钟超时中断，VTIME 仅存储不生效。
 * tty_ibuf    : 行缓冲。ICANON 模式攒到 '\n' 才返回整行；raw 模式按
 *               VMIN 攒够字节。回显始终逐字符即时执行（见 tty_echo）。 */
static uint32_t tty_pgrp = 0;
static uint8_t  tty_vmin = 1;
static uint8_t  tty_vtime = 0;
static char     tty_ibuf[256];
static int      tty_ibuf_len = 0;
static int      tty_ibuf_pos = 0;

/* forward decl：tty_ops 在文件后半定义（devfs_is_tty 需要比较 ops 指针） */
static file_ops_t tty_ops;

void devfs_tty_get_flags(uint32_t* lflag, uint32_t* oflag)
{
    if (lflag) *lflag = tty_c_lflag;
    if (oflag) *oflag = tty_c_oflag;
}

void devfs_tty_set_flags(uint32_t lflag, uint32_t oflag)
{
    tty_c_lflag = lflag;
    tty_c_oflag = oflag;
}

uint32_t devfs_tty_get_pgrp(void) { return tty_pgrp; }

void devfs_tty_set_pgrp(uint32_t pgrp) { tty_pgrp = pgrp; }

void devfs_tty_get_vmin_vtime(uint8_t* vmin, uint8_t* vtime)
{
    if (vmin)  *vmin  = tty_vmin;
    if (vtime) *vtime = tty_vtime;
}

/* W7: tty 是否可读（poll/select 用）：行缓冲有残留数据或键盘有键。 */
int devfs_tty_readable(void)
{
    if (tty_ibuf_pos < tty_ibuf_len) return 1;
    return kbd_has_key() ? 1 : 0;
}

/* W7: fd 是否指向 /dev/tty（poll 按类型分发用）。 */
int devfs_is_tty(file_t* f)
{
    return f && f->ops == &tty_ops;
}

void devfs_tty_set_vmin_vtime(uint8_t vmin, uint8_t vtime)
{
    tty_vmin = vmin;
    tty_vtime = vtime;
}

/* 无条件即时回显（历史教训：readline dumb 模式清 ECHO 位后内核仍须回显，
 * 此函数保持原 tty_read 的回显行为一字不变）。 */
static void tty_echo(char c)
{
    if (c == '\n')      vga_putc('\n');
    else if (c == '\b') vga_puts("\b \b");
    else                vga_putc(c);
}

/* W7: 取一个键。无键时阻塞（WAIT_KBD）并切换调度器，让后台任务/其他
 * 子进程运行；主循环检测到键盘输入时唤醒。 */
static char tty_getc(void)
{
    char c;
    for (;;) {
        if (kbd_try_get(&c)) return c;
        sched_block_and_switch(WAIT_KBD, 0);
    }
}

static uint64_t tty_read(void* f, uint64_t off, void* buf, uint64_t size)
{
    (void)f; (void)off;
    if (size == 0) return 0;

    /* 1) 先消费上次攒下的行缓冲中尚未被用户取走的部分（canonical 模式下
     *    用户缓冲小于一行时剩余字节保留，下次 read 继续返回）。 */
    if (tty_ibuf_pos < tty_ibuf_len) {
        uint64_t n = (uint64_t)(tty_ibuf_len - tty_ibuf_pos);
        if (n > size) n = size;
        for (uint64_t i = 0; i < n; i++) ((char*)buf)[i] = tty_ibuf[tty_ibuf_pos + i];
        tty_ibuf_pos += (int)n;
        if (tty_ibuf_pos >= tty_ibuf_len) { tty_ibuf_len = 0; tty_ibuf_pos = 0; }
        return n;
    }

    /* 2) 攒数据。回显无条件逐字符即时执行（历史教训：readline 处于 dumb
     *    模式，从不即时回显单个字符；它的 raw TCSETS 还会清掉 ECHO 位，
     *    若内核也按 ECHO 位回显则双方都不回显。内核作为终端必须无条件
     *    回显。readline 的延迟整行重绘用 \r 原位覆盖，与内核回显内容
     *    一致，不会重复。） */
    if (tty_c_lflag & TTY_ICANON) {
        /* canonical：攒到 '\n' 返回整行；'\b' 回退缓冲前一字符。 */
        while (1) {
            char c = tty_getc();
            if (c == '\n') {
                tty_echo(c);
                if (tty_ibuf_len < (int)sizeof(tty_ibuf))
                    tty_ibuf[tty_ibuf_len++] = c;
                break;
            }
            if (c == '\b') {
                if (tty_ibuf_len > 0) tty_ibuf_len--;
                tty_echo(c);
                continue;
            }
            tty_echo(c);
            if (tty_ibuf_len < (int)sizeof(tty_ibuf))
                tty_ibuf[tty_ibuf_len++] = c;
        }
    } else {
        /* raw：按 VMIN 决定返回前的阻塞粒度（本内核无 VTIME 超时，
         * 未设置/VMIN=0 时按 1 字节处理，与历史逐字节行为一致）。 */
        int need = tty_vmin >= 1 ? (int)tty_vmin : 1;
        if (need > (int)sizeof(tty_ibuf)) need = (int)sizeof(tty_ibuf);
        if (need > (int)size) need = (int)size;
        while (tty_ibuf_len < need) {
            char c = tty_getc();
            tty_echo(c);
            tty_ibuf[tty_ibuf_len++] = c;
        }
    }

    /* 3) 返回攒好的数据（canonical 下超过用户缓冲的部分留在 tty_ibuf）。 */
    uint64_t n = (uint64_t)tty_ibuf_len;
    if (n > size) n = size;
    for (uint64_t i = 0; i < n; i++) ((char*)buf)[i] = tty_ibuf[i];
    tty_ibuf_pos = (int)n;
    if (tty_ibuf_pos >= tty_ibuf_len) { tty_ibuf_len = 0; tty_ibuf_pos = 0; }
    return n;
}

static uint64_t tty_write(void* f, uint64_t off, const void* buf, uint64_t size)
{
    (void)f; (void)off;
    const char* s = (const char*)buf;
    for (uint64_t i = 0; i < size; i++) {
        vga_putc(s[i]);
    }
    return size;
}

static uint64_t tty_close(void* f) { (void)f; return 0; }

/* Pre-allocated device file objects */
static file_ops_t null_ops = { null_read, null_write, 0, null_close, 0, 0 };
static file_ops_t zero_ops = { zero_read, zero_write, 0, zero_close, 0, 0 };
static file_ops_t tty_ops  = { tty_read,  tty_write,  0, tty_close,  0, 0 };

static file_t dev_null;
static file_t dev_zero;
static file_t dev_tty;

void devfs_init(void)
{
    /* /dev/null */
    dev_null.inode  = 1;
    dev_null.size   = 0;
    dev_null.offset = 0;
    dev_null.ops    = &null_ops;
    dev_null.private_data = (void*)0;
    dev_null.ref_count = 1;
    for (int i = 0; i < 64; i++) dev_null.name[i] = 0;
    dev_null.name[0] = '/'; dev_null.name[1] = 'd'; dev_null.name[2] = 'e';
    dev_null.name[3] = 'v'; dev_null.name[4] = '/'; dev_null.name[5] = 'n';
    dev_null.name[6] = 'u'; dev_null.name[7] = 'l'; dev_null.name[8] = 'l';

    /* /dev/zero */
    dev_zero.inode  = 2;
    dev_zero.size   = 0;
    dev_zero.offset = 0;
    dev_zero.ops    = &zero_ops;
    dev_zero.private_data = (void*)0;
    dev_zero.ref_count = 1;
    for (int i = 0; i < 64; i++) dev_zero.name[i] = 0;
    dev_zero.name[0] = '/'; dev_zero.name[1] = 'd'; dev_zero.name[2] = 'e';
    dev_zero.name[3] = 'v'; dev_zero.name[4] = '/'; dev_zero.name[5] = 'z';
    dev_zero.name[6] = 'e'; dev_zero.name[7] = 'r'; dev_zero.name[8] = 'o';

    /* /dev/tty */
    dev_tty.inode  = 3;
    dev_tty.size   = 0;
    dev_tty.offset = 0;
    dev_tty.ops    = &tty_ops;
    dev_tty.private_data = (void*)0;
    dev_tty.ref_count = 1;
    for (int i = 0; i < 64; i++) dev_tty.name[i] = 0;
    dev_tty.name[0] = '/'; dev_tty.name[1] = 'd'; dev_tty.name[2] = 'e';
    dev_tty.name[3] = 'v'; dev_tty.name[4] = '/'; dev_tty.name[5] = 't';
    dev_tty.name[6] = 't'; dev_tty.name[7] = 'y';
}

file_t* devfs_open(const char* path, uint64_t flags)
{
    (void)flags;

    /* Simple string comparison */
    if (path[0] == '/' && path[1] == 'd' && path[2] == 'e' &&
        path[3] == 'v' && path[4] == '/') {

        if (path[5] == 'n' && path[6] == 'u' && path[7] == 'l' &&
            path[8] == 'l' && path[9] == '\0') {
            return &dev_null;
        }
        if (path[5] == 'z' && path[6] == 'e' && path[7] == 'r' &&
            path[8] == 'o' && path[9] == '\0') {
            return &dev_zero;
        }
        if (path[5] == 't' && path[6] == 't' && path[7] == 'y' &&
            path[8] == '\0') {
            return &dev_tty;
        }
    }
    return (file_t*)0;
}