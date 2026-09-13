/*
 * socket.c — W8 socket 系统调用分流层。
 *
 * 分流规则（用户确认的方案）：
 *   AF_INET + SOCK_STREAM → loop_*（W6 内存回环，仅 127.0.0.1）
 *   AF_INET + SOCK_DGRAM  → 真实网卡协议栈（UDP）
 * syscall 编号与函数名保持与 W6 完全一致（41-45/48-50），用户态无感。
 *
 * 阻塞语义（无中断架构的关键）：UDP 收不到数据时不能自旋等待，必须
 * sched_block_and_switch(WAIT_NET, port) 让出 CPU，主循环的 net_poll()
 * 才会继续收包，收到后由 udp_input() → sched_wake_cond() 唤醒本任务。
 */
#include "net.h"
#include "udp.h"
#include "loopback.h"
#include "vfs.h"
#include "sched.h"
#include "printk.h"

#define AF_INET     2
#define SOCK_STREAM 1
#define SOCK_DGRAM  2

#define UDP_SOCK_MAX 16

typedef struct {
    int      used;
    uint16_t port;        /* 本端端口（bind 后有效，主机序） */
    uint32_t peer_ip;     /* connect 记录的对端（可选） */
    uint16_t peer_port;
} udp_sock_t;

static udp_sock_t g_usocks[UDP_SOCK_MAX];
static file_t     g_ufiles[UDP_SOCK_MAX];

static uint64_t udp_file_read(void* file, uint64_t offset, void* buf, uint64_t size);
static uint64_t udp_file_write(void* file, uint64_t offset, const void* buf, uint64_t size);
static uint64_t udp_file_close(void* file);

static file_ops_t udp_ops = { udp_file_read, udp_file_write, 0, udp_file_close, 0, 0 };

static int fd_is_udp(file_t* f) { return f && f->ops == &udp_ops; }

/* ---- sockaddr_in 解析（与 loopback.c 同布局） ---- */
static uint16_t sa_family(uint64_t addr)
{
    const uint8_t* a = (const uint8_t*)addr;
    return (uint16_t)(a[0] | (a[1] << 8));
}
static uint16_t sa_port(uint64_t addr)
{
    const uint8_t* a = (const uint8_t*)addr;
    return (uint16_t)((a[2] << 8) | a[3]);      /* 大端 → 主机序 */
}
static uint32_t sa_ip(uint64_t addr)
{
    const uint8_t* a = (const uint8_t*)addr;
    return ((uint32_t)a[4] << 24) | ((uint32_t)a[5] << 16) |
           ((uint32_t)a[6] << 8)  |  (uint32_t)a[7];
}
static void sa_store(uint64_t addr, uint32_t ip, uint16_t port)
{
    if (!addr) return;
    uint8_t* a = (uint8_t*)addr;
    a[0] = 2; a[1] = 0;                          /* AF_INET */
    a[2] = (uint8_t)(port >> 8);
    a[3] = (uint8_t)(port & 0xFF);
    a[4] = (uint8_t)(ip >> 24);
    a[5] = (uint8_t)(ip >> 16);
    a[6] = (uint8_t)(ip >> 8);
    a[7] = (uint8_t)ip;
    for (int i = 8; i < 16; i++) a[i] = 0;
}

/* ---- UDP file_ops ---- */

static uint64_t udp_file_read(void* file, uint64_t offset, void* buf, uint64_t size)
{
    (void)offset;
    file_t* f = (file_t*)file;
    udp_sock_t* s = (udp_sock_t*)f->private_data;
    if (!s || !s->used || !s->port) return (uint64_t)(-9);      /* EBADF */

    for (;;) {
        int n = udp_recv(s->port, buf, (int)size, NULL, NULL);
        if (n > 0) return (uint64_t)n;
        if (n < 0) return (uint64_t)(-9);
        if (f->flags & O_NONBLOCK) return (uint64_t)(-11);       /* EAGAIN */
        sched_block_and_switch(WAIT_NET, s->port);
    }
}

static uint64_t udp_file_write(void* file, uint64_t offset, const void* buf, uint64_t size)
{
    (void)offset;
    file_t* f = (file_t*)file;
    udp_sock_t* s = (udp_sock_t*)f->private_data;
    if (!s || !s->used) return (uint64_t)(-9);
    if (!s->peer_ip) return (uint64_t)(-107);                    /* ENOTCONN */
    if (udp_send(s->peer_ip, s->peer_port, buf, (uint32_t)size) != 0)
        return (uint64_t)(-5);                                   /* EIO */
    return size;
}

static uint64_t udp_file_close(void* file)
{
    file_t* f = (file_t*)file;
    udp_sock_t* s = (udp_sock_t*)f->private_data;
    if (s) {
        if (s->used && s->port) udp_unbind(s->port);
        s->used = 0;
        s->port = 0;
        s->peer_ip = 0;
        s->peer_port = 0;
    }
    f->private_data = NULL;
    f->ops = NULL;                                               /* 归还文件池 */
    return 0;
}

/* ---- 系统调用 ---- */

/* 41: socket(domain, type, proto) */
uint64_t sys_socket(uint64_t domain, uint64_t type, uint64_t proto,
                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    if (domain != AF_INET) return (uint64_t)(-97);               /* EAFNOSUPPORT */

    if (type == SOCK_STREAM)
        return loop_socket(domain, type, proto, a4, a5, a6);

    if (type != SOCK_DGRAM) return (uint64_t)(-93);              /* EPROTONOSUPPORT */

    udp_sock_t* s = NULL;
    for (int i = 0; i < UDP_SOCK_MAX; i++)
        if (!g_usocks[i].used) { s = &g_usocks[i]; break; }
    if (!s) return (uint64_t)(-24);                              /* EMFILE */

    file_t* f = NULL;
    for (int i = 0; i < UDP_SOCK_MAX; i++)
        if (!g_ufiles[i].ops) { f = &g_ufiles[i]; break; }
    if (!f) return (uint64_t)(-24);

    memset(s, 0, sizeof(*s));
    s->used = 1;

    f->inode = 0; f->size = 0; f->offset = 0;
    f->ops = &udp_ops;
    f->private_data = s;
    f->ref_count = 1;
    f->name[0] = 0;
    f->flags = 0;
    f->f_type = 0;

    int fd = vfs_fd_alloc(f);
    if (fd < 0) { s->used = 0; f->ops = NULL; return (uint64_t)(-24); }
    return (uint64_t)fd;
}

/* 49: bind(fd, &sockaddr_in, len) */
uint64_t sys_bind(uint64_t fd, uint64_t addr, uint64_t len,
                  uint64_t a4, uint64_t a5, uint64_t a6)
{
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);
    if (!fd_is_udp(f))
        return loop_bind(fd, addr, len, a4, a5, a6);

    udp_sock_t* s = (udp_sock_t*)f->private_data;
    if (!s || s->port) return (uint64_t)(-22);                   /* EINVAL */
    if (sa_family(addr) != AF_INET) return (uint64_t)(-97);

    uint16_t port = sa_port(addr);
    int r = udp_bind(port, current_task ? current_task->pid : 0);
    if (r != 0) return (uint64_t)r;
    s->port = port;
    return 0;
}

/* 50: listen — UDP 无此语义 */
uint64_t sys_listen(uint64_t fd, uint64_t backlog,
                    uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);
    if (fd_is_udp(f)) return (uint64_t)(-95);                    /* EOPNOTSUPP */
    return loop_listen(fd, backlog, a3, a4, a5, a6);
}

/* 42: connect — UDP 只记录对端地址（Linux 语义） */
uint64_t sys_connect(uint64_t fd, uint64_t addr, uint64_t len,
                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);
    if (!fd_is_udp(f))
        return loop_connect(fd, addr, len, a4, a5, a6);

    udp_sock_t* s = (udp_sock_t*)f->private_data;
    if (!s) return (uint64_t)(-9);
    if (sa_family(addr) != AF_INET) return (uint64_t)(-97);

    s->peer_ip   = sa_ip(addr);
    s->peer_port = sa_port(addr);
    return 0;
}

/* 43: accept — UDP 无此语义 */
uint64_t sys_accept(uint64_t fd, uint64_t addr_out, uint64_t addrlen,
                    uint64_t a4, uint64_t a5, uint64_t a6)
{
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);
    if (fd_is_udp(f)) return (uint64_t)(-95);                    /* EOPNOTSUPP */
    return loop_accept(fd, addr_out, addrlen, a4, a5, a6);
}

/* 44: sendto(fd, buf, len, flags, addr, addrlen) */
uint64_t sys_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                    uint64_t addr, uint64_t addrlen)
{
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);
    if (!fd_is_udp(f))
        return loop_sendto(fd, buf, len, flags, addr, addrlen);

    udp_sock_t* s = (udp_sock_t*)f->private_data;
    if (!s) return (uint64_t)(-9);

    uint32_t dst_ip   = s->peer_ip;
    uint16_t dst_port = s->peer_port;
    if (addr) {
        dst_ip   = sa_ip(addr);
        dst_port = sa_port(addr);
    }
    if (!dst_ip) return (uint64_t)(-89);                         /* EDESTADDRREQ */

    if (udp_send(dst_ip, dst_port, (const void*)buf, (uint32_t)len) != 0)
        return (uint64_t)(-5);                                   /* EIO */
    return len;
}

/* 45: recvfrom(fd, buf, len, flags, addr, addrlen) */
uint64_t sys_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                      uint64_t addr, uint64_t addrlen)
{
    (void)addrlen;
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);
    if (!fd_is_udp(f))
        return loop_recvfrom(fd, buf, len, flags, addr, addrlen);

    udp_sock_t* s = (udp_sock_t*)f->private_data;
    if (!s || !s->used || !s->port) return (uint64_t)(-9);

    for (;;) {
        uint32_t from_ip = 0;
        uint16_t from_port = 0;
        int n = udp_recv(s->port, (void*)buf, (int)len, &from_ip, &from_port);
        if (n > 0) {
            if (addr) sa_store(addr, from_ip, from_port);
            return (uint64_t)n;
        }
        if (n < 0) return (uint64_t)(-9);
        if (f->flags & O_NONBLOCK) return (uint64_t)(-11);       /* EAGAIN */
        /* 让出 CPU，等主循环收到包后唤醒（无中断架构下的必要配合） */
        sched_block_and_switch(WAIT_NET, s->port);
    }
}

/* 48: shutdown — UDP 无此语义 */
uint64_t sys_shutdown(uint64_t fd, uint64_t how,
                      uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);
    if (fd_is_udp(f)) {
        udp_sock_t* s = (udp_sock_t*)f->private_data;
        if (s) { s->peer_ip = 0; s->peer_port = 0; }
        return 0;
    }
    return loop_shutdown(fd, how, a3, a4, a5, a6);
}
