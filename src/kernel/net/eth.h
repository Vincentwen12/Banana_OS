#ifndef ETH_H
#define ETH_H

/*
 * eth.h — W8 Ethernet II 帧与 ARP。
 */

#include "net.h"

typedef struct __attribute__((packed)) {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t type;              /* 大端 EtherType */
} eth_hdr_t;

typedef struct __attribute__((packed)) {
    uint16_t hw_type;           /* 大端：1 = Ethernet */
    uint16_t proto_type;        /* 大端：0x0800 = IPv4 */
    uint8_t  hw_len;            /* 6 */
    uint8_t  proto_len;         /* 4 */
    uint16_t opcode;            /* 大端：1 = request, 2 = reply */
    uint8_t  sender_mac[ETH_ALEN];
    uint32_t sender_ip;         /* 网络序 */
    uint8_t  target_mac[ETH_ALEN];
    uint32_t target_ip;         /* 网络序 */
} arp_hdr_t;

#define ETH_TYPE_ARP_REQ  1
#define ETH_TYPE_ARP_REP  2

#define ARP_TABLE_SIZE    16
#define ARP_TIMEOUT_MS    300000ULL   /* 5 分钟老化 */

/* 发送一个完整以太帧（payload 为上层内容）。len 为整帧长度。
 * frame 首 6 字节（目的 MAC）由本函数填写：命中 ARP 缓存则直接发出；
 * 未命中则把整帧存入 pending 槽并发起 ARP 请求，收到应答后自动补发。 */
void eth_output(uint32_t dst_ip, uint8_t* frame, uint32_t len);

/* 直接以指定目的 MAC 发送（广播 ARP 请求/应答用）。 */
void eth_send_raw(const uint8_t dst_mac[ETH_ALEN], uint16_t ethertype_be,
                  const void* payload, uint32_t len);

/* ARP 报文入口（net_rx 分发，参数为去掉 14 字节以太头之后的内容）。 */
void arp_input(const uint8_t* pkt, uint32_t len);

int  arp_lookup(uint32_t ip, uint8_t out_mac[ETH_ALEN]);
void arp_request(uint32_t ip);
void arp_flush_stale(void);

/* shell `arp` 命令枚举：命中返回 1，否则 0。age_ms 为表项年龄（ms）。 */
int  arp_entry_at(int i, uint32_t* ip, uint8_t mac[ETH_ALEN], uint64_t* age_ms);

#endif /* ETH_H */
