#ifndef UDP_H
#define UDP_H

/*
 * udp.h — W8 UDP（端口绑定表 + 固定深度接收队列 + 阻塞唤醒）。
 *
 * 队列静态分配（无动态内存）：8 个绑定端口 × 4 个 512B 槽 ≈ 16KB BSS。
 */

#include "net.h"

typedef struct __attribute__((packed)) {
    uint16_t src_port;   /* 大端 */
    uint16_t dst_port;   /* 大端 */
    uint16_t len;        /* 大端：UDP 首部 + 数据 */
    uint16_t checksum;   /* 大端：IPv4 下 0 = 不校验 */
} udp_hdr_t;

#define UDP_MAX_BINDS   8
#define UDP_QUEUE_DEPTH 4
#define UDP_DGRAM_MAX   512

/* 绑定/解绑端口（冲突返回 -98 EADDRINUSE，表满返回 -24 EMFILE） */
int  udp_bind(uint16_t port, uint64_t pid);
void udp_unbind(uint16_t port);
int  udp_is_bound(uint16_t port);

int  udp_send(uint32_t dst_ip, uint16_t dst_port, const void* data, uint32_t len);
void udp_input(const uint8_t* pkt, uint32_t len, uint32_t src_ip);

/* 取一个数据报：取到返回字节数；队列空返回 0；参数错返回 -1。
 * from_ip / from_port 可为 NULL。 */
int  udp_recv(uint16_t port, void* buf, int max,
              uint32_t* from_ip, uint16_t* from_port);

void udp_stats(uint64_t* rx, uint64_t* tx, uint64_t* dropped);

#endif /* UDP_H */
