/*
 * pci.c — W8 PCI 总线枚举。
 *
 * 扫描 bus 0-255 / dev 0-31 / func 0-7（多功能设备由 header_type bit7 指示），
 * 记录 vendor/device/class/BAR/IRQ。空槽（vendor==0xFFFF）跳过。
 *
 * 端口访问用 core/port.h 的 outd/ind（32 位；本内核的 32 位原语叫 ind/outd）。
 * 无中断相关代码：ESP/IRQ 只做记录，本内核不启用 PCI 中断。
 */
#include "pci.h"
#include "port.h"
#include "printk.h"

static pci_device_t g_devs[PCI_MAX_DEVS];
static int          g_count = 0;

static uint32_t pci_addr(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    return 0x80000000U
         | ((uint32_t)bus  << 16)
         | ((uint32_t)dev  << 11)
         | ((uint32_t)func << 8)
         | ((uint32_t)off & 0xFCU);
}

uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    outd(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, off));
    return ind(PCI_CONFIG_DATA);
}

uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    uint32_t v = pci_read32(bus, dev, func, (uint8_t)(off & 0xFC));
    return (uint16_t)((v >> ((off & 2) * 8)) & 0xFFFF);
}

uint8_t pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
    uint32_t v = pci_read32(bus, dev, func, (uint8_t)(off & 0xFC));
    return (uint8_t)((v >> ((off & 3) * 8)) & 0xFF);
}

void pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off,
                 uint32_t val)
{
    outd(PCI_CONFIG_ADDR, pci_addr(bus, dev, func, off));
    outd(PCI_CONFIG_DATA, val);
}

void pci_init(void)
{
    g_count = 0;

    for (int bus = 0; bus < 256 && g_count < PCI_MAX_DEVS; bus++) {
        for (int dev = 0; dev < 32 && g_count < PCI_MAX_DEVS; dev++) {
            if (pci_read16((uint8_t)bus, (uint8_t)dev, 0, 0x00) == 0xFFFF)
                continue;                       /* 空槽 */

            uint8_t header = pci_read8((uint8_t)bus, (uint8_t)dev, 0, 0x0E);
            int nfunc = (header & 0x80) ? 8 : 1;   /* 多功能设备 */

            for (int func = 0; func < nfunc && g_count < PCI_MAX_DEVS; func++) {
                uint16_t vendor = pci_read16((uint8_t)bus, (uint8_t)dev,
                                             (uint8_t)func, 0x00);
                if (vendor == 0xFFFF) continue;

                pci_device_t* d = &g_devs[g_count++];
                d->bus      = (uint8_t)bus;
                d->dev      = (uint8_t)dev;
                d->func     = (uint8_t)func;
                d->vendor   = vendor;
                d->device   = pci_read16((uint8_t)bus, (uint8_t)dev,
                                         (uint8_t)func, 0x02);
                d->progif   = pci_read8((uint8_t)bus, (uint8_t)dev,
                                        (uint8_t)func, 0x09);
                d->subclass = pci_read8((uint8_t)bus, (uint8_t)dev,
                                        (uint8_t)func, 0x0A);
                d->class    = pci_read8((uint8_t)bus, (uint8_t)dev,
                                        (uint8_t)func, 0x0B);
                d->header   = header;
                d->irq      = pci_read8((uint8_t)bus, (uint8_t)dev,
                                        (uint8_t)func, 0x3C);

                for (int b = 0; b < 6; b++) {
                    uint32_t bar = pci_read32((uint8_t)bus, (uint8_t)dev,
                                              (uint8_t)func,
                                              (uint8_t)(0x10 + b * 4));
                    d->bar[b] = bar;
                    d->bar_is_io[b] = (uint8_t)(bar & 1U);
                }
            }
        }
    }

    printk(KERN_INFO, "[PCI] %u device(s) found\n", (unsigned)g_count);
}

int pci_count(void) { return g_count; }

pci_device_t* pci_at(int i)
{
    if (i < 0 || i >= g_count) return NULL;
    return &g_devs[i];
}

pci_device_t* pci_find(uint16_t vendor, uint16_t device)
{
    for (int i = 0; i < g_count; i++)
        if (g_devs[i].vendor == vendor && g_devs[i].device == device)
            return &g_devs[i];
    return NULL;
}

pci_device_t* pci_find_class(uint8_t cls, uint8_t sub)
{
    for (int i = 0; i < g_count; i++)
        if (g_devs[i].class == cls && g_devs[i].subclass == sub)
            return &g_devs[i];
    return NULL;
}
