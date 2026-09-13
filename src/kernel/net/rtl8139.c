/*
 * rtl8139.c — W8 RTL8139 网卡驱动（轮询，无中断）。
 *
 * 硬件要点：
 *  - I/O BAR0 访问全部寄存器（QEMU 的 rtl8139 默认给 I/O BAR）。
 *  - RX 环 8KB（CR 的 RBLEN 默认 00 = 8KB），另加 16B 头 + 1500B 回绕余量，
 *    故分配 3 页（12KB）。环中每个包格式：[status(2)][length(2)][payload][crc(4)]，
 *    length 含 4 字节 CRC。
 *  - 包可能跨越 8KB 边界：因为缓冲实际分配了 12KB 且物理连续，我们直接从
 *    rx_buf + capr 读取，仅在指针推进时对 8KB 取模。
 *  - 不启用中断（IMR=0）：靠主循环 net_poll() → rtl8139_poll() 收包。
 *
 * DMA 缓冲来自 pmalloc_contig()：物理连续、4KB 对齐；本内核恒等映射，
 * 因此线性地址即可直接写入 RBSTART/TSAD 寄存器。
 */
#include "rtl8139.h"
#include "net.h"
#include "pci.h"
#include "port.h"
#include "mm.h"
#include "printk.h"
#include "timer.h"

/* ---- 寄存器偏移（I/O BAR0 基址） ---- */
#define RTL_IDR0      0x00   /* MAC[0..5] */
#define RTL_TSD0      0x10   /* 四个 32 位发送状态（每 4 字节一个） */
#define RTL_TSAD0     0x20   /* 四个 32 位发送缓冲物理地址 */
#define RTL_RBSTART   0x30   /* RX 环物理地址 */
#define RTL_CMD       0x37   /* 命令寄存器（8 位） */
#define RTL_CAPR      0x38   /* 当前读指针（16 位） */
#define RTL_CBR       0x3A   /* 当前写指针（16 位） */
#define RTL_IMR       0x3C   /* 中断掩码（16 位） */
#define RTL_ISR       0x3E   /* 中断状态（16 位） */
#define RTL_TCR       0x40   /* 发送配置（32 位） */
#define RTL_RCR       0x44   /* 接收配置（32 位） */

/* ---- 命令/状态位 ---- */
#define RTL_CMD_RESET 0x10
#define RTL_CMD_RE    0x08   /* 接收使能 */
#define RTL_CMD_TE    0x04   /* 发送使能 */

#define RTL_ISR_ROK   0x0001
#define RTL_ISR_RER   0x0002
#define RTL_ISR_TOK   0x0004
#define RTL_ISR_TER   0x0008
#define RTL_ISR_RXOVW 0x0010

#define RTL_TSD_TOK   0x8000 /* 发送成功 */
#define RTL_TSD_TABT  0x4000 /* 发送中止 */

#define RTL_TX_TIMEOUT_MS 10

/* ---- 缓冲配置 ---- */
#define RTL_RX_RING_SIZE  8192      /* 硬件环大小（RBLEN=00） */
#define RTL_RX_EXTRA      (16 + MTU + 4)
#define RTL_RX_BYTES      (RTL_RX_RING_SIZE + RTL_RX_EXTRA)
#define RTL_RX_PAGES      3         /* 12KB ≥ 8192 + 1520 */
#define RTL_TX_COUNT      4
#define RTL_TX_BUF_SIZE   2048
#define RTL_TX_PAGES      2         /* 4 × 2048 = 8KB */
#define RTL_POLL_MAX_PKTS 16        /* 单次 poll 处理上限，避免霸占主循环 */

typedef struct {
    net_device_t dev;
    uint16_t     io;
    uint8_t*     rx_buf;
    uint32_t     rx_capr;              /* 驱动维护的读指针（0..8191） */
    uint8_t*     tx_buf[RTL_TX_COUNT];
    int          tx_idx;
} rtl8139_priv_t;

static rtl8139_priv_t g_rtl;

/* ---- 驱动回调 ---- */

static int rtl8139_init_dev(net_device_t* dev)
{
    rtl8139_priv_t* p = (rtl8139_priv_t*)dev->priv;
    uint16_t io = p->io;

    /* 软复位 */
    outb((uint16_t)(io + RTL_CMD), RTL_CMD_RESET);
    uint64_t t0 = timer_ms();
    while ((inb((uint16_t)(io + RTL_CMD)) & RTL_CMD_RESET) &&
           timer_ms() - t0 < 100) {
        /* 等待复位完成 */
    }

    /* MAC 地址：QEMU 复位后已填入 IDR0..IDR5，无需读 EEPROM */
    for (int i = 0; i < ETH_ALEN; i++)
        dev->mac[i] = inb((uint16_t)(io + RTL_IDR0 + i));

    /* RX 环 */
    p->rx_buf = (uint8_t*)pmalloc_contig(RTL_RX_PAGES);
    if (!p->rx_buf) return -1;
    memset(p->rx_buf, 0, RTL_RX_PAGES * PAGE_SIZE);
    p->rx_capr = 0;
    outd((uint16_t)(io + RTL_RBSTART), (uint32_t)(uintptr_t)p->rx_buf);

    /* TX 缓冲（4 个连续槽） */
    uint8_t* tx = (uint8_t*)pmalloc_contig(RTL_TX_PAGES);
    if (!tx) return -1;
    memset(tx, 0, RTL_TX_PAGES * PAGE_SIZE);
    for (int i = 0; i < RTL_TX_COUNT; i++)
        p->tx_buf[i] = tx + i * RTL_TX_BUF_SIZE;
    p->tx_idx = 0;

    /* 配置：TCR 默认 IFG；RCR = AB|AM|APM + WRAP(bit7) */
    outd((uint16_t)(io + RTL_TCR), 0x00000300u);
    outd((uint16_t)(io + RTL_RCR), 0x0000000Fu | (1u << 7));
    outw((uint16_t)(io + RTL_IMR), 0x0000);       /* 轮询模式：屏蔽中断 */
    outw((uint16_t)(io + RTL_ISR), 0xFFFF);       /* 清全部中断状态 */
    /* CAPR 语义为“已读指针 - 16”（硬件预取 16 字节）：初值 0 必须写成
     * -16（0xFFF0）。写成 0 会让网卡认为还有 16 字节未读，后续收包因
     * “缓冲区空间不足”被整体丢弃（TX 正常但 RX 恒为 0）。 */
    outw((uint16_t)(io + RTL_CAPR), (uint16_t)(p->rx_capr - 16));

    /* 使能收发 */
    outb((uint16_t)(io + RTL_CMD), (uint8_t)(RTL_CMD_RE | RTL_CMD_TE));
    return 0;
}

static int rtl8139_send(net_device_t* dev, const void* data, uint32_t len)
{
    rtl8139_priv_t* p = (rtl8139_priv_t*)dev->priv;
    uint16_t io = p->io;

    if (len < ETH_HDR_LEN || len > RTL_TX_BUF_SIZE) {
        dev->tx_errors++;
        return -1;
    }

    int i = p->tx_idx;
    p->tx_idx = (p->tx_idx + 1) % RTL_TX_COUNT;

    uint16_t tsd_off = (uint16_t)(RTL_TSD0 + i * 4);
    uint16_t tsad_off = (uint16_t)(RTL_TSAD0 + i * 4);

    memcpy(p->tx_buf[i], data, len);
    outd((uint16_t)(io + tsad_off), (uint32_t)(uintptr_t)p->tx_buf[i]);
    /* 低 13 位为长度，写入时自动清除 TOK/TABT */
    outd((uint16_t)(io + tsd_off), len & 0x1FFFu);

    uint64_t t0 = timer_ms();
    while (timer_ms() - t0 < RTL_TX_TIMEOUT_MS) {
        uint32_t tsd = ind((uint16_t)(io + tsd_off));
        if (tsd & RTL_TSD_TOK) {
            dev->tx_packets++;
            return 0;
        }
        if (tsd & RTL_TSD_TABT) {
            dev->tx_errors++;
            return -1;
        }
    }
    dev->tx_errors++;
    return -1;
}

static int rtl8139_poll(net_device_t* dev)
{
    rtl8139_priv_t* p = (rtl8139_priv_t*)dev->priv;
    uint16_t io = p->io;
    int processed = 0;

    /* 清掉上一轮的 ROK/RXOVW 等状态位（无中断，仅维护状态） */
    uint16_t isr = inw((uint16_t)(io + RTL_ISR));
    if (isr & (RTL_ISR_RER | RTL_ISR_RXOVW)) {
        dev->rx_errors++;
        if (isr & RTL_ISR_RXOVW) dev->rx_dropped++;
    }
    if (isr) outw((uint16_t)(io + RTL_ISR), (uint16_t)(isr & 0x001Fu));

    while (processed < RTL_POLL_MAX_PKTS) {
        uint32_t cbr = inw((uint16_t)(io + RTL_CBR));
        if ((cbr & 0xFFFFu) == p->rx_capr) break;      /* 无新包 */

        uint8_t* hdr = p->rx_buf + p->rx_capr;
        uint16_t status = (uint16_t)(hdr[0] | ((uint16_t)hdr[1] << 8));
        uint16_t length = (uint16_t)(hdr[2] | ((uint16_t)hdr[3] << 8));

        /* 长度异常：重新同步到硬件写指针，避免死循环 */
        if (length < 8 || length > RTL_RX_RING_SIZE) {
            dev->rx_errors++;
            dev->rx_dropped++;
            p->rx_capr = cbr & 0xFFFFu;
            outw((uint16_t)(io + RTL_CAPR),
                 (uint16_t)(p->rx_capr - 16));
            break;
        }

        uint32_t pkt_len = (uint32_t)length - 4;       /* 去掉 4 字节 CRC */

        if (status & RTL_ISR_ROK) {
            net_rx(hdr + 4, pkt_len);
        } else {
            dev->rx_errors++;
            dev->rx_dropped++;
        }

        /* 前进：4 字节头 + length（含 CRC），16 字节对齐 */
        p->rx_capr = (p->rx_capr + 4u + length + 3u) & ~3u;
        if (p->rx_capr >= RTL_RX_RING_SIZE)
            p->rx_capr -= RTL_RX_RING_SIZE;

        /* CAPR 写 capr-16：给硬件留出余量，避免边写边读竞争 */
        outw((uint16_t)(io + RTL_CAPR),
             (uint16_t)(p->rx_capr - 16));
        processed++;
    }
    return processed;
}

/* ---- probe ---- */

net_device_t* rtl8139_probe(pci_device_t* d)
{
    if (!d) return NULL;
    if (d->vendor != 0x10EC || d->device != 0x8139) return NULL;

    /* 使能 I/O 空间（bit0）与 bus master（bit2） */
    uint32_t cmd = pci_read32(d->bus, d->dev, d->func, 0x04);
    pci_write32(d->bus, d->dev, d->func, 0x04, cmd | 0x0005u);

    if (!(d->bar[0] & 1u)) {
        printk(KERN_WARNING, "[RTL8139] BAR0 is not an I/O BAR (bar=%x)\n",
               d->bar[0]);
        return NULL;
    }

    rtl8139_priv_t* p = &g_rtl;
    memset(p, 0, sizeof(*p));
    p->io = (uint16_t)(d->bar[0] & ~0x3u);

    net_device_t* dev = &p->dev;
    const char* n = "eth0";
    int i = 0;
    while (n[i] && i < 15) { dev->name[i] = n[i]; i++; }
    dev->name[i] = '\0';

    dev->io_base = p->io;
    dev->is_mmio = 0;
    dev->priv    = p;
    dev->init    = rtl8139_init_dev;
    dev->send    = rtl8139_send;
    dev->poll    = rtl8139_poll;
    return dev;
}
