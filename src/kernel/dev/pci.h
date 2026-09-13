#ifndef PCI_H
#define PCI_H

/*
 * pci.h — W8 PCI 总线枚举（配置空间 0xCF8/0xCFC）。
 *
 * 只做枚举与 BAR 解析；不涉及中断/MSI（本内核无中断子系统）。
 */

#include "axion.h"

#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC
#define PCI_MAX_DEVS     32

typedef struct {
    uint8_t  bus, dev, func;
    uint16_t vendor, device;
    uint8_t  class, subclass, progif, header;
    uint32_t bar[6];
    uint8_t  bar_is_io[6];    /* 1 = I/O 空间，0 = MMIO */
    uint8_t  irq;
} pci_device_t;

/* 配置空间读写（off 低 2 位被忽略，按 32 位对齐访问） */
uint32_t pci_read32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
uint16_t pci_read16(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
uint8_t  pci_read8(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off);
void     pci_write32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off,
                     uint32_t val);

void          pci_init(void);
int           pci_count(void);
pci_device_t* pci_at(int i);
pci_device_t* pci_find(uint16_t vendor, uint16_t device);
pci_device_t* pci_find_class(uint8_t cls, uint8_t sub);

#endif /* PCI_H */
