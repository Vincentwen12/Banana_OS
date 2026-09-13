#ifndef RTL8139_H
#define RTL8139_H

/*
 * rtl8139.h — W8 RTL8139 网卡驱动（纯轮询，无中断）。
 */

#include "net.h"
#include "pci.h"

/* 匹配 PCI 上的 10EC:8139 并填充一个静态 net_device_t；不匹配返回 NULL。
 * 仅做结构填充与 PCI 命令位（I/O + bus master）使能，硬件初始化在
 * dev->init() 里完成。 */
net_device_t* rtl8139_probe(pci_device_t* d);

#endif /* RTL8139_H */
