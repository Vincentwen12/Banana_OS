/* loopback.c — W6 loopback 网络最小实现（Task 3.3）。
 *
 * 实现：socket(41) / bind(49) / listen(50) / connect(42) / accept(43) /
 *       sendto(44) / recvfrom(45) / shutdown(48)，以及 file_ops
 *       （read=loop_recv, write=loop_send, close=loop_close）。
 * 仅支持 AF_INET(2) + SOCK_STREAM(1)，仅 127.0.0.1，16 端口表。
 *
 * 同步串行模型说明：本内核用户进程由 shell `run` 与 bash wait4 同步串行
 * 执行（user_run），没有并行用户任务。因此 connect() 同步立即完成：找到
 * 目标监听端口后立刻创建连接对（server/client 两个端点 + 各自接收缓冲），
 * 把 server 侧端点压入监听 socket 的待接受队列，connect 即返回 0。
 * accept() 从队列弹出即返回新 fd。send(fd) 写入对端接收缓冲；recv(fd)
 * 从本端接收缓冲读。
 *
 * 缓冲满/空：用 pause 自旋+重试做有界轮询。sys_sched_yield 是 table.c 的
 * static 函数无法跨文件复用；sched_block/sched_wake 面向多任务阻塞，同步
 * 单任务模型下没有其它进程会填满/排空缓冲来唤醒我们，故用有界自旋最简。
 * 测试场景中 connect/send 总是先于 accept/recv 发生，轮询分支通常不命中。
 *
 * 内存访问：内核可直接读写用户内存（本内核 identity-map 全部物理内存，
 * 与 sys_uname/sys_gettimeofday 的直写模式一致）；测试程序的 sockaddr/
 * 缓冲区均在自身 .data/.bss 段，保证可映射。
 */
#include "vfs.h"
#include "loopback.h"

#define AF_INET     2
#define SOCK_STREAM 1

#define LOOP_MAX_SOCKS   32   /* socket 私有数据池 */
#define LOOP_MAX_FILES   32   /* socket file_t 池 */
#define LOOP_MAX_LISTEN  16   /* 16 端口表 */
#define LOOP_MAX_CONNS   64   /* 连接端点池（每连接 2 端点） */
#define LOOP_BACKLOG     8    /* 每监听 socket 待接受队列深度 */
#define LOOP_BUF_SIZE    256  /* 每端点接收缓冲 */
#define LOOP_SPIN_MAX    1000000  /* 有界轮询上限（pause 指令次数） */

/* ---- 数据结构 ---- */
typedef struct loop_conn loop_conn_t;
typedef struct loop_sock loop_sock_t;

/* 环形接收缓冲 */
typedef struct {
    uint8_t  data[LOOP_BUF_SIZE];
    uint16_t head;      /* 读位置 */
    uint16_t tail;      /* 写位置 */
    uint16_t count;     /* 已用字节 */
} loop_rbuf_t;

/* 连接端点（连接对的一侧；send 写入 peer 的 rbuf，recv 读自己的 rbuf） */
struct loop_conn {
    uint16_t used;
    uint16_t closed;       /* shutdown/close 置位 */
    uint16_t local_port;   /* 主机字节序；0 = 未绑定（client 侧） */
    loop_conn_t* peer;     /* 对端端点；被关闭/释放后为 NULL */
    loop_rbuf_t rbuf;      /* 对端发来的数据，由本端 recv 读取 */
};

/* 监听 socket 私有数据 */
typedef struct {
    uint16_t used;
    uint16_t listening;    /* listen() 已调用 */
    uint16_t port;         /* 主机字节序 */
    loop_conn_t* pending[LOOP_BACKLOG];  /* 待接受队列（server 侧端点） */
    uint16_t pending_head, pending_tail, pending_count;
} loop_listen_t;

/* socket() 私有数据：监听态或连接态 */
struct loop_sock {
    uint16_t used;
    uint16_t kind;         /* 0=未绑定, 1=监听, 2=已连接 */
    loop_listen_t* lst;    /* kind==1 */
    loop_conn_t* conn;     /* kind==2 */
};

/* ---- 静态池（BSS 清零，无需 init） ---- */
static loop_sock_t   g_socks[LOOP_MAX_SOCKS];
static loop_listen_t g_listens[LOOP_MAX_LISTEN];
static loop_conn_t   g_conns[LOOP_MAX_CONNS];
static file_t        g_files[LOOP_MAX_FILES];   /* ops==NULL 表示空槽 */

static uint64_t loop_recv(void* file, uint64_t offset, void* buf, uint64_t size);
static uint64_t loop_send(void* file, uint64_t offset, const void* buf, uint64_t size);
static uint64_t loop_close(void* file);
static file_ops_t loop_ops = { loop_recv, loop_send, 0, loop_close, 0, 0 };

/* ---- 池分配 ---- */
static loop_sock_t* loop_sock_alloc(void)
{
    for (int i = 0; i < LOOP_MAX_SOCKS; i++) {
        if (!g_socks[i].used) {
            g_socks[i].used = 1;
            g_socks[i].kind = 0;
            g_socks[i].lst = NULL;
            g_socks[i].conn = NULL;
            return &g_socks[i];
        }
    }
    return NULL;
}

static loop_listen_t* loop_listen_alloc(void)
{
    for (int i = 0; i < LOOP_MAX_LISTEN; i++) {
        if (!g_listens[i].used) {
            g_listens[i].used = 1;
            g_listens[i].listening = 0;
            g_listens[i].port = 0;
            g_listens[i].pending_head = g_listens[i].pending_tail = 0;
            g_listens[i].pending_count = 0;
            for (int j = 0; j < LOOP_BACKLOG; j++)
                g_listens[i].pending[j] = NULL;
            return &g_listens[i];
        }
    }
    return NULL;
}

static loop_conn_t* loop_conn_alloc(void)
{
    for (int i = 0; i < LOOP_MAX_CONNS; i++) {
        if (!g_conns[i].used) {
            g_conns[i].used = 1;
            g_conns[i].closed = 0;
            g_conns[i].local_port = 0;
            g_conns[i].peer = NULL;
            g_conns[i].rbuf.head = g_conns[i].rbuf.tail = 0;
            g_conns[i].rbuf.count = 0;
            return &g_conns[i];
        }
    }
    return NULL;
}

static file_t* loop_file_alloc(void)
{
    for (int i = 0; i < LOOP_MAX_FILES; i++) {
        if (!g_files[i].ops) {
            file_t* f = &g_files[i];
            f->inode = 0;
            f->size = 0;
            f->offset = 0;
            f->ops = &loop_ops;
            f->private_data = NULL;
            f->ref_count = 1;
            f->name[0] = 0;
            return f;
        }
    }
    return NULL;
}

/* ---- 环形缓冲 ---- */
static int rbuf_empty(const loop_rbuf_t* r) { return r->count == 0; }
static int rbuf_full(const loop_rbuf_t* r)  { return r->count >= LOOP_BUF_SIZE; }

static uint16_t rbuf_put(loop_rbuf_t* r, const uint8_t* src, uint16_t n)
{
    uint16_t space = (uint16_t)(LOOP_BUF_SIZE - r->count);
    if (n > space) n = space;
    for (uint16_t i = 0; i < n; i++) {
        r->data[r->tail] = src[i];
        r->tail = (uint16_t)((r->tail + 1) % LOOP_BUF_SIZE);
    }
    r->count = (uint16_t)(r->count + n);
    return n;
}

static uint16_t rbuf_get(loop_rbuf_t* r, uint8_t* dst, uint16_t n)
{
    if (n > r->count) n = r->count;
    for (uint16_t i = 0; i < n; i++) {
        dst[i] = r->data[r->head];
        r->head = (uint16_t)((r->head + 1) % LOOP_BUF_SIZE);
    }
    r->count = (uint16_t)(r->count - n);
    return n;
}

/* ---- 地址解析 / 回写（直接访问用户内存） ----
 * sockaddr_in 布局：sin_family(u16) sin_port(u16 BE) sin_addr(u32 BE)
 *                    sin_zero[8]；仅接受 AF_INET + 127.0.0.1。 */
static void loop_parse_addr(uint64_t addr, uint16_t* family, uint16_t* port,
                            int* is_loopback)
{
    const uint8_t* a = (const uint8_t*)addr;
    *family      = (uint16_t)(a[0] | (a[1] << 8));
    *port        = (uint16_t)((a[2] << 8) | a[3]);   /* 大端 → 主机序 */
    *is_loopback = (a[4] == 127 && a[5] == 0 && a[6] == 0 && a[7] == 1);
}

static void loop_store_addr(uint64_t addr, uint16_t port)
{
    uint8_t* a = (uint8_t*)addr;
    a[0] = 2; a[1] = 0;                          /* sin_family = AF_INET */
    a[2] = (uint8_t)(port >> 8);                 /* sin_port 大端 */
    a[3] = (uint8_t)(port & 0xFF);
    a[4] = 127; a[5] = 0; a[6] = 0; a[7] = 1;    /* 127.0.0.1 */
    for (int i = 8; i < 16; i++) a[i] = 0;       /* sin_zero */
}

/* ---- file_ops：recv 从本端接收缓冲读；send 写入对端接收缓冲 ---- */
static uint64_t loop_recv_io(file_t* f, void* buf, uint64_t size)
{
    loop_sock_t* s = (loop_sock_t*)f->private_data;
    if (!s || s->kind != 2 || !s->conn) return 0;   /* 未连接：EOF */
    loop_conn_t* self = s->conn;
    loop_conn_t* peer = self->peer;
    uint8_t* dst = (uint8_t*)buf;
    uint64_t total = 0;
    while (total < size) {
        if (rbuf_empty(&self->rbuf)) {
            /* 对端关闭/已断开或本端关闭 → EOF */
            if (self->closed || !peer || peer->closed) return total;
            /* 空：有界轮询（同步模型下无并发进程会来填充，安全网而已） */
            int spins = 0;
            while (rbuf_empty(&self->rbuf) && spins < LOOP_SPIN_MAX) {
                __asm__ volatile("pause");
                spins++;
            }
            if (rbuf_empty(&self->rbuf)) return total;   /* 仍空：返回已读 */
        }
        uint16_t n = rbuf_get(&self->rbuf, dst + total,
            (uint16_t)((size - total > LOOP_BUF_SIZE) ? LOOP_BUF_SIZE
                                                      : (uint16_t)(size - total)));
        if (n == 0) break;
        total += n;
    }
    return total;
}

static uint64_t loop_send_io(file_t* f, const void* buf, uint64_t size)
{
    loop_sock_t* s = (loop_sock_t*)f->private_data;
    if (!s || s->kind != 2 || !s->conn) return 0;   /* 未连接 */
    loop_conn_t* self = s->conn;
    if (self->closed) return 0;
    loop_conn_t* peer = self->peer;
    if (!peer || peer->closed) return 0;            /* 对端已关闭 */
    const uint8_t* src = (const uint8_t*)buf;
    uint64_t total = 0;
    while (total < size) {
        if (rbuf_full(&peer->rbuf)) {
            /* 满：有界轮询（同步模型下对端会先 recv，安全网而已） */
            int spins = 0;
            while (rbuf_full(&peer->rbuf) && spins < LOOP_SPIN_MAX) {
                __asm__ volatile("pause");
                spins++;
            }
            if (rbuf_full(&peer->rbuf)) break;       /* 仍满：返回已写 */
        }
        uint16_t n = rbuf_put(&peer->rbuf, src + total,
            (uint16_t)((size - total > LOOP_BUF_SIZE) ? LOOP_BUF_SIZE
                                                      : (uint16_t)(size - total)));
        if (n == 0) break;
        total += n;
    }
    return total;
}

static uint64_t loop_recv(void* file, uint64_t offset, void* buf, uint64_t size)
{
    (void)offset;
    return loop_recv_io((file_t*)file, buf, size);
}

static uint64_t loop_send(void* file, uint64_t offset, const void* buf, uint64_t size)
{
    (void)offset;
    return loop_send_io((file_t*)file, buf, size);
}

static uint64_t loop_close(void* file)
{
    file_t* f = (file_t*)file;
    loop_sock_t* s = (loop_sock_t*)f->private_data;
    if (s) {
        if (s->kind == 2 && s->conn) {
            loop_conn_t* c = s->conn;
            if (c->peer) {
                c->peer->peer = NULL;   /* 对端 recv 看到 peer==NULL → EOF */
                c->peer->closed = 1;
            }
            c->used = 0;                /* 释放连接端点 */
            s->conn = NULL;
        } else if (s->kind == 1 && s->lst) {
            /* 关闭监听：丢弃待接受队列中的 server 端点，client 侧看到 EOF */
            for (int i = 0; i < LOOP_BACKLOG; i++) {
                loop_conn_t* sc = s->lst->pending[i];
                if (sc) {
                    if (sc->peer) {
                        sc->peer->peer = NULL;
                        sc->peer->closed = 1;
                    }
                    sc->used = 0;
                    s->lst->pending[i] = NULL;
                }
            }
            s->lst->used = 0;
            s->lst = NULL;
        }
        s->used = 0;                    /* 归还 sock 池 */
    }
    f->private_data = NULL;
    f->ops = NULL;                      /* 归还 file 池 */
    return 0;
}

/* ---- syscall 实现 ---- */

/* 41: socket(domain=AF_INET(2), type=SOCK_STREAM(1), proto) */
uint64_t loop_socket(uint64_t domain, uint64_t type, uint64_t proto,
                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)proto; (void)a4; (void)a5; (void)a6;
    if (domain != AF_INET) return (uint64_t)(-97);    /* EAFNOSUPPORT */
    if (type != SOCK_STREAM) return (uint64_t)(-93);  /* EPROTONOSUPPORT */

    loop_sock_t* s = loop_sock_alloc();
    if (!s) return (uint64_t)(-24);   /* EMFILE */
    file_t* f = loop_file_alloc();
    if (!f) { s->used = 0; return (uint64_t)(-24); }

    f->private_data = s;
    int fd = vfs_fd_alloc(f);
    if (fd < 0) { s->used = 0; f->ops = NULL; return (uint64_t)(-24); }
    return (uint64_t)fd;
}

/* 49: bind(fd, &sockaddr_in, len) — 仅 127.0.0.1，端口写入监听结构 */
uint64_t loop_bind(uint64_t fd, uint64_t addr, uint64_t len,
                   uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)len; (void)a4; (void)a5; (void)a6;
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);   /* EBADF */
    loop_sock_t* s = (loop_sock_t*)f->private_data;
    if (!s) return (uint64_t)(-9);
    if (s->kind != 0) return (uint64_t)(-22);   /* EINVAL：已绑定/已连接 */

    uint16_t family, port;
    int is_lo;
    loop_parse_addr(addr, &family, &port, &is_lo);
    if (family != AF_INET) return (uint64_t)(-97);   /* EAFNOSUPPORT */
    if (!is_lo) return (uint64_t)(-99);              /* EADDRNOTAVAIL */
    if (port == 0) return (uint64_t)(-22);           /* EINVAL */

    /* 端口冲突检查（16 端口表） */
    for (int i = 0; i < LOOP_MAX_LISTEN; i++) {
        if (g_listens[i].used && g_listens[i].port == port)
            return (uint64_t)(-98);   /* EADDRINUSE */
    }
    loop_listen_t* l = loop_listen_alloc();
    if (!l) return (uint64_t)(-98);   /* 端口表满：视为 EADDRINUSE */
    l->port = port;
    s->kind = 1;
    s->lst = l;
    return 0;
}

/* 50: listen(fd, backlog) — 置监听态（bind 已初始化待接受队列） */
uint64_t loop_listen(uint64_t fd, uint64_t backlog,
                     uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)backlog; (void)a3; (void)a4; (void)a5; (void)a6;
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);
    loop_sock_t* s = (loop_sock_t*)f->private_data;
    if (!s || s->kind != 1 || !s->lst) return (uint64_t)(-22);  /* EINVAL */
    s->lst->listening = 1;
    return 0;
}

/* 42: connect(fd, &sockaddr_in, len) — 同步立即完成：
 * 找到监听 socket，创建连接对，server 端点入队，返回 0 */
uint64_t loop_connect(uint64_t fd, uint64_t addr, uint64_t len,
                      uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)len; (void)a4; (void)a5; (void)a6;
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);
    loop_sock_t* s = (loop_sock_t*)f->private_data;
    if (!s) return (uint64_t)(-9);
    if (s->kind != 0) return (uint64_t)(-22);   /* EINVAL：已连接/监听中 */

    uint16_t family, port;
    int is_lo;
    loop_parse_addr(addr, &family, &port, &is_lo);
    if (family != AF_INET) return (uint64_t)(-97);      /* EAFNOSUPPORT */
    if (!is_lo) return (uint64_t)(-101);                /* ENETUNREACH */
    if (port == 0) return (uint64_t)(-22);              /* EINVAL */

    loop_listen_t* l = NULL;
    for (int i = 0; i < LOOP_MAX_LISTEN; i++) {
        if (g_listens[i].used && g_listens[i].listening &&
            g_listens[i].port == port) {
            l = &g_listens[i];
            break;
        }
    }
    if (!l) return (uint64_t)(-111);   /* ECONNREFUSED：未监听 */
    if (l->pending_count >= LOOP_BACKLOG)
        return (uint64_t)(-111);       /* ECONNREFUSED：待接受队列满 */

    loop_conn_t* client = loop_conn_alloc();
    loop_conn_t* server = loop_conn_alloc();
    if (!client || !server) {
        if (client) client->used = 0;
        if (server) server->used = 0;
        return (uint64_t)(-12);        /* ENOMEM */
    }
    client->peer = server;
    server->peer = client;
    client->local_port = 0;            /* client 侧未绑定（临时端口） */
    server->local_port = port;
    client->closed = 0;
    server->closed = 0;

    l->pending[l->pending_tail] = server;
    l->pending_tail = (uint16_t)((l->pending_tail + 1) % LOOP_BACKLOG);
    l->pending_count++;

    s->kind = 2;
    s->conn = client;
    return 0;
}

/* 43: accept(fd, addr_out, addrlen) — 队列非空则弹出并新建 server 端点 fd；
 * 空则有界轮询（loopback 场景 connect 先发生，通常直接命中） */
uint64_t loop_accept(uint64_t fd, uint64_t addr_out, uint64_t addrlen,
                     uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)addrlen; (void)a4; (void)a5; (void)a6;
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);
    loop_sock_t* s = (loop_sock_t*)f->private_data;
    if (!s || s->kind != 1 || !s->lst || !s->lst->listening)
        return (uint64_t)(-22);        /* EINVAL */

    loop_listen_t* l = s->lst;
    int spins = 0;
    while (l->pending_count == 0) {
        if (spins >= LOOP_SPIN_MAX) return (uint64_t)(-11);   /* EAGAIN */
        __asm__ volatile("pause");
        spins++;
    }
    loop_conn_t* server = l->pending[l->pending_head];
    l->pending[l->pending_head] = NULL;
    l->pending_head = (uint16_t)((l->pending_head + 1) % LOOP_BACKLOG);
    l->pending_count--;

    loop_sock_t* ns = loop_sock_alloc();
    file_t* nf = loop_file_alloc();
    if (!ns || !nf) {
        if (ns) ns->used = 0;
        if (nf) nf->ops = NULL;
        return (uint64_t)(-24);        /* EMFILE */
    }
    ns->kind = 2;
    ns->conn = server;
    nf->private_data = ns;

    int nfd = vfs_fd_alloc(nf);
    if (nfd < 0) {
        ns->used = 0;
        nf->ops = NULL;
        return (uint64_t)(-24);
    }

    if (addr_out) loop_store_addr(addr_out, server->local_port);
    return (uint64_t)nfd;
}

/* 44: sendto(fd, buf, len, flags, addr, addrlen) — 退化为 send(=write) */
uint64_t loop_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                     uint64_t addr, uint64_t addrlen)
{
    (void)flags; (void)addr; (void)addrlen;
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);     /* EBADF */
    return vfs_write(f, (const void*)buf, len);
}

/* 45: recvfrom(fd, buf, len, flags, addr, addrlen) — 退化为 recv(=read) */
uint64_t loop_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                       uint64_t addr, uint64_t addrlen)
{
    (void)flags; (void)addr; (void)addrlen;
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);     /* EBADF */
    return vfs_read(f, (void*)buf, len);
}

/* 48: shutdown(fd, how) — 置断开标志，释放连接（fd 保留） */
uint64_t loop_shutdown(uint64_t fd, uint64_t how,
                       uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6)
{
    (void)how; (void)a3; (void)a4; (void)a5; (void)a6;
    file_t* f = vfs_fd_get((int)fd);
    if (!f) return (uint64_t)(-9);
    loop_sock_t* s = (loop_sock_t*)f->private_data;
    if (!s) return (uint64_t)(-9);
    if (s->kind == 2 && s->conn) {
        loop_conn_t* c = s->conn;
        if (c->peer) {
            c->peer->peer = NULL;      /* 对端 recv → EOF */
            c->peer->closed = 1;
        }
        c->closed = 1;
        c->used = 0;                   /* 释放本端连接端点 */
        s->conn = NULL;
    }
    return 0;
}
