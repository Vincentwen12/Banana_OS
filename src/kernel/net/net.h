#ifndef NET_H
#define NET_H

/*
 * net.h — W8 网络核心抽象。
 *
 * 保持架构不变：无中断、无 DMA 映射层、恒等映射直接寻址。
 * 收包只在主循环的 net_poll() 里发生（见 kmain.c 主循环第 5 步）。
 */

#include "axion.h"

/* ---- 协议常量 ---- */
#define ETH_ALEN        6
#define ETH_HDR_LEN     14
#define MTU             1500
#define ETH_FRAME_MAX   (ETH_HDR_LEN + MTU)     /* 1514 */
#define IP_HDR_LEN      20
#define UDP_HDR_LEN     8
#define ICMP_HDR_LEN    8

#define ETH_TYPE_IP     0x0800
#define ETH_TYPE_ARP    0x0806

#define IP_PROTO_ICMP   1
#define IP_PROTO_UDP    17

/* ---- 字节序（x86-64 小端） ---- */
static inline uint16_t net_htons(uint16_t v) { return __builtin_bswap16(v); }
static inline uint16_t net_ntohs(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t net_htonl(uint32_t v) { return __builtin_bswap32(v); }
static inline uint32_t net_ntohl(uint32_t v) { return __builtin_bswap32(v); }

/* ---- 网卡抽象 ---- */
/* 驱动负责填充 mac/io_base/priv 与三个回调；协议栈只认这个结构。
 * 返回 0 表示成功，非 0 表示失败（-1）。 */
typedef struct net_device {
    char     name[16];        /* "eth0" */
    uint8_t  mac[ETH_ALEN];
    uint32_t ip;              /* 主机字节序 */
    uint32_t mask;
    uint32_t gw;
    uint64_t io_base;         /* I/O 端口或 MMIO 基址 */
    int      is_mmio;

    /* 统计 */
    uint64_t rx_packets;
    uint64_t tx_packets;
    uint64_t rx_errors;
    uint64_t tx_errors;
    uint64_t rx_dropped;

    void* priv;               /* 驱动私有数据 */
    int (*init)(struct net_device* dev);
    int (*send)(struct net_device* dev, const void* data, uint32_t len);
    int (*poll)(struct net_device* dev);   /* 返回本轮处理的包数 */
} net_device_t;

/* ---- 工具（net.c） ---- */
/* 解析 "a.b.c.d" 为主机字节序；成功置 *ok=1 */
uint32_t net_parse_ip(const char* s, int* ok);
/* 格式化为主机字节序 IP；out 至少 16 字节 */
void     net_format_ip(uint32_t ip, char* out);
/* 格式化 MAC；out 至少 18 字节 */
void     net_format_mac(const uint8_t mac[ETH_ALEN], char* out);

/* ---- 生命周期与收发（net.c） ---- */
void net_init(void);                 /* PCI probe + 驱动初始化 + 默认地址 */
void net_poll(void);                 /* 主循环调用：驱动轮询收包 */
int  net_tx(const void* frame, uint32_t len);
void net_rx(const uint8_t* frame, uint32_t len);   /* 驱动收到完整以太帧后回调 */

int           net_register_device(net_device_t* dev);
net_device_t* net_default(void);
int           net_device_count(void);
net_device_t* net_device_at(int i);
void          net_set_addr(uint32_t ip, uint32_t mask, uint32_t gw);
int           net_is_ready(void);   /* 有可用网卡且初始化成功 */

#endif /* NET_H */
