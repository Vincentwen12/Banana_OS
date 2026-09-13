#ifndef LOOPBACK_H
#define LOOPBACK_H

/*
 * loopback.h — W6 内存回环 TCP 的原语导出。
 *
 * W8 起，socket 系统调用由 net/socket.c 统一分发：
 *   127.0.0.1 / SOCK_STREAM → 本文件的 loop_* 实现（逻辑与 W6 完全一致）
 *   其他地址 / SOCK_DGRAM    → 真实网卡协议栈（ARP/IPv4/ICMP/UDP）
 */

#include "axion.h"

uint64_t loop_socket(uint64_t domain, uint64_t type, uint64_t proto,
                     uint64_t a4, uint64_t a5, uint64_t a6);
uint64_t loop_bind(uint64_t fd, uint64_t addr, uint64_t len,
                   uint64_t a4, uint64_t a5, uint64_t a6);
uint64_t loop_listen(uint64_t fd, uint64_t backlog,
                     uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6);
uint64_t loop_connect(uint64_t fd, uint64_t addr, uint64_t len,
                      uint64_t a4, uint64_t a5, uint64_t a6);
uint64_t loop_accept(uint64_t fd, uint64_t addr_out, uint64_t addrlen,
                     uint64_t a4, uint64_t a5, uint64_t a6);
uint64_t loop_sendto(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                     uint64_t addr, uint64_t addrlen);
uint64_t loop_recvfrom(uint64_t fd, uint64_t buf, uint64_t len, uint64_t flags,
                       uint64_t addr, uint64_t addrlen);
uint64_t loop_shutdown(uint64_t fd, uint64_t how,
                       uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6);

#endif /* LOOPBACK_H */
