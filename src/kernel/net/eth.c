/*
 * eth.c — W8 Ethernet II 帧封装/解封 + ARP。
 *
 * 单 pending 槽设计：ARP 未命中时把待发整帧暂存（1 帧），发出 ARP 请求；
 * 收到应答后立刻补发。这样 ping/UDP 的首个包不会因为 ARP 未解析而丢失，
 * 又不必引入发送队列。
 */
#include "eth.h"
#include "ip.h"
#include "printk.h"
#include "timer.h"

/* ---- ARP 缓存表 ---- */
typedef struct {
    uint32_t ip;
    uint8_t  mac[ETH_ALEN];
    uint64_t ts;
    int      valid;
} arp_entry_t;

static arp_entry_t g_arp[ARP_TABLE_SIZE];

/* ---- 单 pending 槽 ---- */
static uint8_t  g_pending_frame[ETH_FRAME_MAX];
static uint32_t g_pending_len = 0;
static uint32_t g_pending_ip  = 0;

static const uint8_t BCAST_MAC[ETH_ALEN] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

void eth_send_raw(const uint8_t dst_mac[ETH_ALEN], uint16_t ethertype_be,
                  const void* payload, uint32_t len)
{
    net_device_t* dev = net_default();
    if (!dev) return;
    if (ETH_HDR_LEN + len > ETH_FRAME_MAX) return;

    uint8_t frame[ETH_FRAME_MAX];
    eth_hdr_t* h = (eth_hdr_t*)frame;
    memcpy(h->dst, dst_mac, ETH_ALEN);
    memcpy(h->src, dev->mac, ETH_ALEN);
    h->type = ethertype_be;
    memcpy(frame + ETH_HDR_LEN, payload, len);
    net_tx(frame, ETH_HDR_LEN + len);
}

void eth_output(uint32_t dst_ip, uint8_t* frame, uint32_t len)
{
    if (len < ETH_HDR_LEN || len > ETH_FRAME_MAX) return;

    uint8_t mac[ETH_ALEN];
    if (arp_lookup(dst_ip, mac)) {
        memcpy(frame, mac, ETH_ALEN);      /* 填目的 MAC */
        net_tx(frame, len);
        return;
    }

    /* ARP 未命中：暂存整帧，发起解析 */
    memcpy(g_pending_frame, frame, len);
    g_pending_len = len;
    g_pending_ip  = dst_ip;
    arp_request(dst_ip);
}

/* ---- ARP ---- */

static void arp_update(uint32_t ip, const uint8_t mac[ETH_ALEN])
{
    int free_idx = -1;
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            memcpy(g_arp[i].mac, mac, ETH_ALEN);
            g_arp[i].ts = timer_ms();
            return;
        }
        if (!g_arp[i].valid && free_idx < 0) free_idx = i;
    }
    if (free_idx < 0) {
        /* 表满：替换最旧的一项 */
        uint64_t oldest = ~0ULL;
        for (int i = 0; i < ARP_TABLE_SIZE; i++) {
            if (g_arp[i].ts < oldest) { oldest = g_arp[i].ts; free_idx = i; }
        }
    }
    if (free_idx < 0) return;
    g_arp[free_idx].ip = ip;
    memcpy(g_arp[free_idx].mac, mac, ETH_ALEN);
    g_arp[free_idx].ts = timer_ms();
    g_arp[free_idx].valid = 1;
}

void arp_flush_stale(void)
{
    uint64_t now = timer_ms();
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (g_arp[i].valid && (now - g_arp[i].ts) > ARP_TIMEOUT_MS)
            g_arp[i].valid = 0;
    }
}

int arp_lookup(uint32_t ip, uint8_t out_mac[ETH_ALEN])
{
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (g_arp[i].valid && g_arp[i].ip == ip) {
            memcpy(out_mac, g_arp[i].mac, ETH_ALEN);
            return 1;
        }
    }
    return 0;
}

int arp_entry_at(int i, uint32_t* ip, uint8_t mac[ETH_ALEN], uint64_t* age_ms)
{
    if (i < 0 || i >= ARP_TABLE_SIZE || !g_arp[i].valid) return 0;
    if (ip)     *ip = g_arp[i].ip;
    if (mac)    memcpy(mac, g_arp[i].mac, ETH_ALEN);
    if (age_ms) *age_ms = timer_ms() - g_arp[i].ts;
    return 1;
}

void arp_request(uint32_t ip)
{
    net_device_t* dev = net_default();
    if (!dev) return;

    arp_hdr_t req;
    req.hw_type    = net_htons(1);
    req.proto_type = net_htons(ETH_TYPE_IP);
    req.hw_len     = ETH_ALEN;
    req.proto_len  = 4;
    req.opcode     = net_htons(ETH_TYPE_ARP_REQ);
    memcpy(req.sender_mac, dev->mac, ETH_ALEN);
    req.sender_ip  = net_htonl(dev->ip);
    memset(req.target_mac, 0, ETH_ALEN);
    req.target_ip  = net_htonl(ip);

    eth_send_raw(BCAST_MAC, net_htons(ETH_TYPE_ARP), &req, sizeof(req));
}

static void arp_send_reply(const uint8_t dst_mac[ETH_ALEN], uint32_t to_ip)
{
    net_device_t* dev = net_default();
    if (!dev) return;

    arp_hdr_t rep;
    rep.hw_type    = net_htons(1);
    rep.proto_type = net_htons(ETH_TYPE_IP);
    rep.hw_len     = ETH_ALEN;
    rep.proto_len  = 4;
    rep.opcode     = net_htons(ETH_TYPE_ARP_REP);
    memcpy(rep.sender_mac, dev->mac, ETH_ALEN);
    rep.sender_ip  = net_htonl(dev->ip);
    memcpy(rep.target_mac, dst_mac, ETH_ALEN);
    rep.target_ip  = net_htonl(to_ip);

    eth_send_raw(dst_mac, net_htons(ETH_TYPE_ARP), &rep, sizeof(rep));
}

static void arp_try_pending(void)
{
    if (!g_pending_len) return;

    uint8_t mac[ETH_ALEN];
    if (!arp_lookup(g_pending_ip, mac)) return;

    memcpy(g_pending_frame, mac, ETH_ALEN);     /* 填目的 MAC */
    net_tx(g_pending_frame, g_pending_len);
    g_pending_len = 0;
}

void arp_input(const uint8_t* pkt, uint32_t len)
{
    if (len < sizeof(arp_hdr_t)) return;

    const arp_hdr_t* a = (const arp_hdr_t*)pkt;
    if (net_ntohs(a->hw_type) != 1) return;
    if (net_ntohs(a->proto_type) != ETH_TYPE_IP) return;
    if (a->hw_len != ETH_ALEN || a->proto_len != 4) return;

    net_device_t* dev = net_default();
    if (!dev) return;

    uint32_t sip = net_ntohl(a->sender_ip);
    uint32_t tip = net_ntohl(a->target_ip);
    uint16_t op  = net_ntohs(a->opcode);

    if (op == ETH_TYPE_ARP_REQ) {
        arp_update(sip, a->sender_mac);
        if (tip == dev->ip)
            arp_send_reply(a->sender_mac, sip);
    } else if (op == ETH_TYPE_ARP_REP) {
        arp_update(sip, a->sender_mac);
        arp_try_pending();
    }
}
