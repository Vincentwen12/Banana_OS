/*
 * ip.c — W8 IPv4 + ICMP（含 ping 收发与统计）。
 *
 * 分片：不重组。MF=1 或分片偏移非 0 的包直接丢弃并计 rx_dropped。
 * ARP 未命中时 ip_send() 把整帧交给 eth_output() 的 pending 槽，返回 0
 * （视为"已排队"），因此上层（ICMP/UDP）无需关心解析过程。
 */
#include "ip.h"
#include "eth.h"
#include "udp.h"
#include "printk.h"
#include "timer.h"

static uint16_t g_ip_id = 1;

/* ---- ping 统计 ---- */
static uint64_t g_ping_sent = 0;
static uint64_t g_ping_recv = 0;
static uint64_t g_ping_unreach = 0;
static uint64_t g_ping_echo_req = 0;
static uint64_t g_ping_last_send_ms = 0;
static uint64_t g_ping_last_rtt = 0;
static uint64_t g_ping_total_rtt = 0;
static uint16_t g_ping_seq = 0;
static uint16_t g_ping_id  = 0x1234;

/* 返回值为网络字节序（可直接写入报文头部的 uint16_t 字段）。
 * 校验已有报文时：校验和正确 → 返回 0。 */
uint16_t ip_checksum(const void* data, uint32_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    uint32_t sum = 0;

    for (uint32_t i = 0; i + 1 < len; i += 2)
        sum += (uint32_t)(((uint16_t)p[i] << 8) | p[i + 1]);
    if (len & 1)
        sum += (uint32_t)((uint16_t)p[len - 1] << 8);

    while (sum >> 16) sum = (sum & 0xFFFFu) + (sum >> 16);
    return net_htons((uint16_t)(~sum));
}

int ip_send(uint32_t dst, uint8_t proto, const void* payload, uint32_t len)
{
    net_device_t* dev = net_default();
    if (!dev) return -1;
    if (IP_HDR_LEN + len > MTU) return -1;

    uint8_t frame[ETH_FRAME_MAX];
    eth_hdr_t* eh = (eth_hdr_t*)frame;
    memset(eh->dst, 0, ETH_ALEN);            /* 由 eth_output 填 */
    memcpy(eh->src, dev->mac, ETH_ALEN);
    eh->type = net_htons(ETH_TYPE_IP);

    ip_hdr_t* ih = (ip_hdr_t*)(frame + ETH_HDR_LEN);
    memset(ih, 0, sizeof(*ih));
    ih->version_ihl = 0x45;                  /* IPv4, 20 字节首部 */
    ih->tos         = 0;
    ih->total_len   = net_htons((uint16_t)(IP_HDR_LEN + len));
    ih->id          = net_htons(g_ip_id++);
    ih->frag_off    = 0;                     /* 一律不分片 */
    ih->ttl         = 64;
    ih->proto       = proto;
    ih->src         = net_htonl(dev->ip);
    ih->dst         = net_htonl(dst);
    ih->checksum    = 0;
    ih->checksum    = ip_checksum(ih, IP_HDR_LEN);

    memcpy(frame + ETH_HDR_LEN + IP_HDR_LEN, payload, len);
    eth_output(dst, frame, ETH_HDR_LEN + IP_HDR_LEN + len);
    return 0;
}

/* ---- ICMP ---- */

static void icmp_input(const uint8_t* pkt, uint32_t len, uint32_t src_ip)
{
    if (len < ICMP_HDR_LEN) return;

    const icmp_hdr_t* ic = (const icmp_hdr_t*)pkt;

    if (ic->type == ICMP_TYPE_ECHO_REPLY) {
        g_ping_recv++;
        if (g_ping_last_send_ms) {
            g_ping_last_rtt = timer_ms() - g_ping_last_send_ms;
            g_ping_total_rtt += g_ping_last_rtt;
        }
        return;
    }

    if (ic->type == ICMP_TYPE_ECHO_REQUEST) {
        g_ping_echo_req++;
        /* 原样回送，只改 type 与校验和 */
        uint8_t buf[MTU];
        if (len > MTU) return;
        memcpy(buf, pkt, len);
        icmp_hdr_t* out = (icmp_hdr_t*)buf;
        out->type = ICMP_TYPE_ECHO_REPLY;
        out->checksum = 0;
        out->checksum = ip_checksum(buf, len);
        ip_send(src_ip, IP_PROTO_ICMP, buf, len);
        return;
    }

    if (ic->type == ICMP_TYPE_UNREACHABLE) {
        g_ping_unreach++;       /* UDP 打到未监听端口时网关会回这个 */
        return;
    }
}

void icmp_send_ping(uint32_t dst)
{
    uint8_t buf[ICMP_HDR_LEN + 32];
    icmp_hdr_t* ic = (icmp_hdr_t*)buf;

    ic->type     = ICMP_TYPE_ECHO_REQUEST;
    ic->code     = 0;
    ic->checksum = 0;
    ic->id       = net_htons(g_ping_id);
    ic->seq      = net_htons(++g_ping_seq);
    memset(buf + ICMP_HDR_LEN, 0x42, 32);
    ic->checksum = ip_checksum(buf, sizeof(buf));

    if (ip_send(dst, IP_PROTO_ICMP, buf, sizeof(buf)) == 0) {
        g_ping_sent++;
        g_ping_last_send_ms = timer_ms();
    }
}

void icmp_reset_stats(void)
{
    g_ping_sent = 0;
    g_ping_recv = 0;
    g_ping_unreach = 0;
    g_ping_last_rtt = 0;
    g_ping_total_rtt = 0;
}

void icmp_stats(uint64_t* sent, uint64_t* recv, uint64_t* unreach,
                uint64_t* last_rtt_ms, uint64_t* total_rtt_ms)
{
    if (sent)         *sent = g_ping_sent;
    if (recv)         *recv = g_ping_recv;
    if (unreach)      *unreach = g_ping_unreach;
    if (last_rtt_ms)  *last_rtt_ms = g_ping_last_rtt;
    if (total_rtt_ms) *total_rtt_ms = g_ping_total_rtt;
}

uint64_t icmp_echo_requests(void) { return g_ping_echo_req; }

/* ---- IPv4 入口 ---- */

void ip_input(const uint8_t* pkt, uint32_t len)
{
    net_device_t* dev = net_default();

    if (len < IP_HDR_LEN) { if (dev) dev->rx_dropped++; return; }

    const ip_hdr_t* ip = (const ip_hdr_t*)pkt;

    if ((ip->version_ihl >> 4) != 4) { if (dev) dev->rx_dropped++; return; }

    uint32_t ihl = (uint32_t)(ip->version_ihl & 0x0F) * 4u;
    if (ihl < IP_HDR_LEN || ihl > len) { if (dev) dev->rx_dropped++; return; }

    uint16_t total = net_ntohs(ip->total_len);
    if (total < ihl || total > len) { if (dev) dev->rx_dropped++; return; }

    /* 分片：W8 不重组，见到即丢 */
    uint16_t frag = net_ntohs(ip->frag_off);
    if ((frag & 0x2000u) || (frag & 0x1FFFu)) {
        if (dev) dev->rx_dropped++;
        return;
    }

    if (ip_checksum(pkt, ihl) != 0) { if (dev) dev->rx_dropped++; return; }

    uint32_t dst = net_ntohl(ip->dst);
    if (dev && dst != dev->ip && dst != 0xFFFFFFFFu) {
        dev->rx_dropped++;
        return;
    }

    uint32_t src = net_ntohl(ip->src);
    const uint8_t* payload = pkt + ihl;
    uint32_t payload_len = (uint32_t)total - ihl;

    if (ip->proto == IP_PROTO_ICMP) {
        icmp_input(payload, payload_len, src);
    } else if (ip->proto == IP_PROTO_UDP) {
        udp_input(payload, payload_len, src);
    } else if (dev) {
        dev->rx_dropped++;
    }
}
