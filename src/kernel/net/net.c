/*
 * net.c — W8 网卡设备表、主循环轮询入口与地址工具。
 *
 * 设计：无中断，收包完全由主循环调用 net_poll() 驱动；驱动在 poll() 内
 * 提取以太帧后回调 net_rx()，这里按 EtherType 分发到 ARP / IPv4。
 * 无设备时所有入口都是安全的空操作，保证未挂网卡的启动路径与从前一致。
 */
#include "net.h"
#include "pci.h"
#include "rtl8139.h"
#include "eth.h"
#include "ip.h"
#include "printk.h"

#define NET_MAX_DEVS 4

static net_device_t* g_devs[NET_MAX_DEVS];
static int           g_dev_count = 0;
static int           g_ready     = 0;

#define IP4(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | \
     ((uint32_t)(c) << 8)  |  (uint32_t)(d))

/* ---- 工具 ---- */

uint32_t net_parse_ip(const char* s, int* ok)
{
    uint32_t parts[4] = { 0, 0, 0, 0 };
    int idx = 0, digits = 0;

    if (!s) { if (ok) *ok = 0; return 0; }

    while (*s && idx < 4) {
        if (*s >= '0' && *s <= '9') {
            parts[idx] = parts[idx] * 10u + (uint32_t)(*s - '0');
            if (parts[idx] > 255u) { if (ok) *ok = 0; return 0; }
            digits++;
        } else if (*s == '.') {
            if (!digits) { if (ok) *ok = 0; return 0; }
            idx++;
            digits = 0;
        } else {
            break;
        }
        s++;
    }
    if (idx != 3 || !digits) { if (ok) *ok = 0; return 0; }
    if (ok) *ok = 1;
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

void net_format_ip(uint32_t ip, char* out)
{
    char* p = out;
    for (int i = 3; i >= 0; i--) {
        uint32_t v = (ip >> (i * 8)) & 0xFFu;
        char tmp[3];
        int n = 0;
        if (v == 0) tmp[n++] = '0';
        while (v) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
        while (n) *p++ = tmp[--n];
        if (i) *p++ = '.';
    }
    *p = '\0';
}

void net_format_mac(const uint8_t mac[ETH_ALEN], char* out)
{
    static const char hex[] = "0123456789abcdef";
    char* p = out;
    for (int i = 0; i < ETH_ALEN; i++) {
        *p++ = hex[(mac[i] >> 4) & 0xF];
        *p++ = hex[mac[i] & 0xF];
        if (i != ETH_ALEN - 1) *p++ = ':';
    }
    *p = '\0';
}

/* ---- 设备表 ---- */

int net_register_device(net_device_t* dev)
{
    if (!dev || g_dev_count >= NET_MAX_DEVS) return -1;
    g_devs[g_dev_count++] = dev;
    return 0;
}

net_device_t* net_default(void)
{
    return g_dev_count ? g_devs[0] : NULL;
}

int net_device_count(void) { return g_dev_count; }

net_device_t* net_device_at(int i)
{
    if (i < 0 || i >= g_dev_count) return NULL;
    return g_devs[i];
}

void net_set_addr(uint32_t ip, uint32_t mask, uint32_t gw)
{
    net_device_t* d = net_default();
    if (!d) return;
    d->ip = ip; d->mask = mask; d->gw = gw;
}

/* ---- 初始化 ---- */

void net_init(void)
{
    /* PCI 枚举由 kmain 在 pci_init() 中完成 */
    pci_device_t* d = pci_find(0x10EC, 0x8139);
    if (!d) {
        printk(KERN_INFO, "[NET] no supported NIC found; network disabled\n");
        return;
    }

    net_device_t* dev = rtl8139_probe(d);
    if (!dev) {
        printk(KERN_WARNING, "[NET] RTL8139 probe failed\n");
        return;
    }
    if (dev->init(dev) != 0) {
        printk(KERN_WARNING, "[NET] %s init failed\n", dev->name);
        return;
    }
    if (net_register_device(dev) != 0) return;

    /* QEMU user-mode 网络的固定约定：guest 10.0.2.15/24，网关 10.0.2.2 */
    dev->ip   = IP4(10, 0, 2, 15);
    dev->mask = IP4(255, 255, 255, 0);
    dev->gw   = IP4(10, 0, 2, 2);

    char ips[16], gws[16], macs[18];
    net_format_ip(dev->ip, ips);
    net_format_ip(dev->gw, gws);
    net_format_mac(dev->mac, macs);
    printk(KERN_INFO, "[NET] %s mac=%s ip=%s/24 gw=%s\n",
           dev->name, macs, ips, gws);
    g_ready = 1;
}

/* ---- 收发 ---- */

void net_poll(void)
{
    for (int i = 0; i < g_dev_count; i++) {
        net_device_t* d = g_devs[i];
        if (d->poll) d->poll(d);
    }
}

int net_tx(const void* frame, uint32_t len)
{
    net_device_t* d = net_default();
    if (!d || !d->send) return -1;
    return d->send(d, frame, len);
}

void net_rx(const uint8_t* frame, uint32_t len)
{
    net_device_t* d = net_default();

    if (len < ETH_HDR_LEN) {
        if (d) d->rx_dropped++;
        return;
    }
    if (d) d->rx_packets++;

    /* EtherType 在帧首部以大端存放 */
    uint16_t type = (uint16_t)(((uint16_t)frame[12] << 8) | frame[13]);

    if (type == ETH_TYPE_ARP) {
        arp_input(frame + ETH_HDR_LEN, len - ETH_HDR_LEN);
    } else if (type == ETH_TYPE_IP) {
        ip_input(frame + ETH_HDR_LEN, len - ETH_HDR_LEN);
    } else if (d) {
        d->rx_dropped++;
    }
}

int net_is_ready(void) { return g_ready; }
