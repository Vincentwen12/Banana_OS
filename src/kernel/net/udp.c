/*
 * udp.c — W8 UDP。
 *
 * 收包路径：rtl8139_poll → net_rx → ip_input → udp_input → 查绑定表入队
 *          → sched_wake_cond(WAIT_NET, port) 唤醒阻塞在 recvfrom 的任务。
 * 因此用户态 recvfrom 必须"让出 CPU"（sched_block_and_switch），主循环
 * 才有机会收包；这是本内核无中断架构下的必要配合（见 net/socket.c）。
 */
#include "udp.h"
#include "ip.h"
#include "sched.h"
#include "printk.h"

typedef struct {
    int      used;
    uint16_t port;
    uint64_t owner;

    uint8_t  data[UDP_QUEUE_DEPTH][UDP_DGRAM_MAX];
    uint16_t len[UDP_QUEUE_DEPTH];
    uint32_t from_ip[UDP_QUEUE_DEPTH];
    uint16_t from_port[UDP_QUEUE_DEPTH];
    int      head, tail, count;

    uint64_t rx, tx, dropped;
} udp_bind_t;

static udp_bind_t g_udp[UDP_MAX_BINDS];

static uint64_t g_rx = 0, g_tx = 0, g_dropped = 0;

static udp_bind_t* udp_find(uint16_t port)
{
    for (int i = 0; i < UDP_MAX_BINDS; i++)
        if (g_udp[i].used && g_udp[i].port == port)
            return &g_udp[i];
    return NULL;
}

int udp_bind(uint16_t port, uint64_t pid)
{
    if (port == 0) return -22;                  /* EINVAL */
    if (udp_find(port)) return -98;             /* EADDRINUSE */

    for (int i = 0; i < UDP_MAX_BINDS; i++) {
        if (!g_udp[i].used) {
            memset(&g_udp[i], 0, sizeof(g_udp[i]));
            g_udp[i].used = 1;
            g_udp[i].port = port;
            g_udp[i].owner = pid;
            return 0;
        }
    }
    return -24;                                 /* EMFILE */
}

void udp_unbind(uint16_t port)
{
    udp_bind_t* b = udp_find(port);
    if (b) memset(b, 0, sizeof(*b));
}

int udp_is_bound(uint16_t port) { return udp_find(port) ? 1 : 0; }

int udp_send(uint32_t dst_ip, uint16_t dst_port, const void* data, uint32_t len)
{
    if (len > MTU - IP_HDR_LEN - UDP_HDR_LEN) return -1;

    uint8_t buf[MTU - IP_HDR_LEN];
    udp_hdr_t* uh = (udp_hdr_t*)buf;
    uh->src_port = net_htons(dst_port);         /* 简化：不维护本地随机端口 */
    uh->dst_port = net_htons(dst_port);
    uh->len      = net_htons((uint16_t)(UDP_HDR_LEN + len));
    uh->checksum = 0;                           /* IPv4 允许不校验 */
    if (len) memcpy(buf + UDP_HDR_LEN, data, len);

    if (ip_send(dst_ip, IP_PROTO_UDP, buf, UDP_HDR_LEN + len) != 0) {
        g_dropped++;
        return -1;
    }
    g_tx++;
    return 0;
}

void udp_input(const uint8_t* pkt, uint32_t len, uint32_t src_ip)
{
    g_rx++;

    if (len < UDP_HDR_LEN) { g_dropped++; return; }

    const udp_hdr_t* uh = (const udp_hdr_t*)pkt;
    uint16_t dport = net_ntohs(uh->dst_port);
    uint16_t ulen  = net_ntohs(uh->len);

    if (ulen < UDP_HDR_LEN || ulen > len) { g_dropped++; return; }

    uint32_t payload_len = (uint32_t)ulen - UDP_HDR_LEN;
    if (payload_len > UDP_DGRAM_MAX) payload_len = UDP_DGRAM_MAX;

    udp_bind_t* b = udp_find(dport);
    if (!b) { g_dropped++; return; }            /* 未绑定端口：丢弃 */

    if (b->count >= UDP_QUEUE_DEPTH) {          /* 队列满：丢最旧 */
        b->head = (b->head + 1) % UDP_QUEUE_DEPTH;
        b->count--;
        b->dropped++;
    }

    int slot = b->tail;
    memcpy(b->data[slot], pkt + UDP_HDR_LEN, payload_len);
    b->len[slot]       = (uint16_t)payload_len;
    b->from_ip[slot]   = src_ip;
    b->from_port[slot] = net_ntohs(uh->src_port);
    b->tail = (b->tail + 1) % UDP_QUEUE_DEPTH;
    b->count++;
    b->rx++;

    /* 唤醒阻塞在该端口上 recvfrom 的任务，让它在主循环下次调度时取数据 */
    sched_wake_cond(WAIT_NET, dport);
}

int udp_recv(uint16_t port, void* buf, int max,
             uint32_t* from_ip, uint16_t* from_port)
{
    udp_bind_t* b = udp_find(port);
    if (!b) return -1;
    if (b->count == 0) return 0;

    int slot = b->head;
    int n = (int)b->len[slot];
    if (n > max) n = max;
    memcpy(buf, b->data[slot], (uint32_t)n);
    if (from_ip)   *from_ip = b->from_ip[slot];
    if (from_port) *from_port = b->from_port[slot];

    b->head = (b->head + 1) % UDP_QUEUE_DEPTH;
    b->count--;
    return n;
}

void udp_stats(uint64_t* rx, uint64_t* tx, uint64_t* dropped)
{
    uint64_t brx = 0, btx = 0, bdrop = 0;
    for (int i = 0; i < UDP_MAX_BINDS; i++) {
        if (!g_udp[i].used) continue;
        brx += g_udp[i].rx;
        btx += g_udp[i].tx;
        bdrop += g_udp[i].dropped;
    }
    if (rx)      *rx = g_rx;
    if (tx)      *tx = g_tx;
    if (dropped) *dropped = g_dropped + bdrop + btx * 0;
}
