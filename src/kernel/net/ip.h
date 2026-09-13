#ifndef IP_H
#define IP_H

/*
 * ip.h — W8 IPv4 与 ICMP。
 *
 * 不实现分片重组：收到 MF=1 或 frag_off≠0 的包直接丢弃并计入 rx_dropped。
 */

#include "net.h"

typedef struct __attribute__((packed)) {
    uint8_t  version_ihl;   /* 高 4 位版本，低 4 位首部长度（4 字节为单位） */
    uint8_t  tos;
    uint16_t total_len;     /* 大端 */
    uint16_t id;            /* 大端 */
    uint16_t frag_off;      /* 大端：bit15 RF / bit14 DF / bit13 MF，低 13 位偏移 */
    uint8_t  ttl;
    uint8_t  proto;
    uint16_t checksum;      /* 大端 */
    uint32_t src;           /* 大端 */
    uint32_t dst;           /* 大端 */
} ip_hdr_t;

typedef struct __attribute__((packed)) {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t id;
    uint16_t seq;
} icmp_hdr_t;

#define ICMP_TYPE_ECHO_REPLY   0
#define ICMP_TYPE_ECHO_REQUEST 8
#define ICMP_TYPE_UNREACHABLE  3

/* 互联网校验和（IP 首部 / ICMP 通用）。校验通过时对首部计算结果为 0。 */
uint16_t ip_checksum(const void* data, uint32_t len);

/* 发送一个 IPv4 包；dest 为主机字节序。返回 0 表示已发出或已排入 ARP
 * pending 槽（等待解析后自动补发），-1 表示失败。 */
int ip_send(uint32_t dst, uint8_t proto, const void* payload, uint32_t len);

/* IPv4 入口（net_rx 分发，参数为去掉以太头之后的内容）。 */
void ip_input(const uint8_t* pkt, uint32_t len);

/* ---- ICMP / ping ---- */
void icmp_send_ping(uint32_t dst);
void icmp_reset_stats(void);
void icmp_stats(uint64_t* sent, uint64_t* recv, uint64_t* unreach,
                uint64_t* last_rtt_ms, uint64_t* total_rtt_ms);
/* 本机收到的 echo request 次数（供 while 回显调试） */
uint64_t icmp_echo_requests(void);

#endif /* IP_H */
