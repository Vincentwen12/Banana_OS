# Axion-Ban W8 — 驱动框架与网络栈（PCI + RTL8139 + ARP/IPv4/ICMP/UDP）

版本：v1.0（基于 v2.0-dev 草案裁剪）
范围决策（用户已确认）：
- **本轮交付到 UDP/ICMP**；简化 TCP 与 socket 流式对接另开 W8.1
- **分流**：目标 `127.0.0.1` 继续走现有 `loopback.c`，其余地址走真实网卡协议栈

---

## 1. 摘要

为 Axion-Ban 增加真实网络能力，同时建立可扩展的设备驱动框架。在**保持"无中断 + 全轮询 + 恒等映射"架构不变**的前提下，实现：

1. PCI 总线枚举（配置空间 0xCF8/0xCFC）与驱动匹配
2. RTL8139 网卡驱动（轮询收发，无中断）
3. Ethernet II + ARP（含缓存表与一次性 pending 槽）
4. IPv4 + ICMP Echo（`ping` 可用）+ UDP
5. socket 系统调用分流层（`SOCK_DGRAM` 走真实栈，`SOCK_STREAM` 保持 loopback）
6. 主循环集成 `net_poll()` 与阻塞/唤醒机制（`WAIT_NET`）

预计新增约 1,600 行 C 代码，新增 8 个源文件、2 个头文件，修改 6 个现有文件。

---

## 2. 当前状态分析（基于实际代码勘察）

### 2.1 轮询与调度模型

| 事实 | 位置 | 对 W8 的影响 |
|---|---|---|
| 唯一主循环是 `kmain()` 的 `while(1)` | `src/kernel/kmain.c:313-502` | 网卡轮询只能插在这里 |
| 轮询点集中在 482–493（`hotness_age` / `policy_apply` / `doorbell_poll(0)` / `sched_tick`） | 同上 | 插入点选在 `doorbell_poll(0)` 旁 |
| PIC 已全掩码、无 IRQ handler、全仓无 `sti` | `kmain.c:41-52`、`idt.c:121-133` | **只能轮询**，不能靠中断收包 |
| IDT 32–255 共用一个通用门 | `idt.c:121-133` | 不存在可挂 IRQ 的入口 |
| AP 核只 `hlt`，不参与轮询 | `kmain.c:98-112` | 不能靠 AP 帮忙收包 |

**关键推论（决定协议栈形态）**：协作式调度下，用户任务在跑用户态代码时主循环不执行。
因此 socket 阻塞读**必须** `sched_block_and_switch()` 让出 CPU，由主循环的 `net_poll()` 收包后 `sched_wake_cond()` 唤醒。现有 `tty_getc()`（`src/kernel/fs/devfs.c:118-125`）就是这个模式，W8 照抄。

### 2.2 现有网络实现

| 事实 | 位置 |
|---|---|
| `src/kernel/net/` 下**只有** `loopback.c`，纯内存回环 TCP，无任何硬件抽象 | `src/kernel/net/loopback.c` |
| `socket/bind/listen/connect/accept/sendto/recvfrom/shutdown`（41-45,48-50）已注册，全部指向 loopback，且是真实实现 | `src/kernel/syscall/table.c:2489-2496`、实现见 `loopback.c:311/330/361/375/429/474/484/494` |
| `sys_socket` 只接受 `AF_INET + SOCK_STREAM`，其他返回 `-93 EPROTONOSUPPORT` | `loopback.c:311-327` |
| `sys_bind` / `sys_connect` 校验 `is_lo`（127.0.0.1），非环回返回 `-99` / `-101` | `loopback.c:330-345`、`375-390` |
| 46/47/51-55（sendmsg/recvmsg/getsockname/…）**未注册**，统一返回 `-38 ENOSYS` | `src/kernel/syscall/syscall.c:76-81` |
| **无** PCI、**无** RTL8139/e1000、**无** ARP/IP/ICMP/TCP/UDP 任何代码或常量 | 全仓 `src/` 检索零命中 |

### 2.3 可复用的现成设施

| 设施 | 位置 | 用途 |
|---|---|---|
| `inb/outb/inw/outw/**ind/outd**/port_delay` | `src/kernel/core/port.h`（32 位叫 `ind/outd`，**没有** `inl/outl`） | PCI 配置空间、RTL8139 I/O BAR |
| `pmalloc_contig(pages)` / `pfree_contig` | `src/kernel/mm/mm.h:19-20`，实现 `mm.c:111-148` | 物理连续 + 4KB 对齐的 RX/TX DMA 缓冲 |
| 恒等映射（物理地址即可当指针用） | `vm.c:4-16`、`mm.c:261-264` 注释 | DMA 缓冲无需 ioremap |
| `volatile` 指针直访 MMIO | `ap.c:38`、`ipc/doorbell.c:85-89`（LAPIC，页表标 `PTE_PCD`） | RTL8139 MMIO BAR |
| `mfence()` / `sfence()` | `src/include/axion.h:86-93` | 收发环的写序保证 |
| `sched_block_and_switch(kind,arg)` / `sched_wake_cond(kind,arg)` | 声明 `sched.h:183-184` | 网络阻塞/唤醒（照抄 `devfs.c:123` + `kmain.c:373`） |
| `WAIT_*` 枚举（当前到 `WAIT_SIGTIMED 8`） | `sched.h:32-40` | **新增 `WAIT_NET 9`** |
| `file_t`（`ops` + `private_data`）/ `vfs_fd_alloc` | `src/kernel/fs/vfs.h:17-28`、`vfs.c:37` | socket fd 的承载方式（loopback 已验证可行） |
| 设备驱动集成范式：导出 `xxx_init()` + kmain 显式调用 | `src/kernel/dev/ata.c:34`、`kmain.c:204` | PCI/net 驱动照此办理 |
| `timer_ms()`（TSC 轮询计时） | `src/kernel/core/timer.c` | ARP 老化、发送超时 |

### 2.4 硬约束（实施时必须遵守）

- `build.ps1` 的 `$cSrcs`（第 41–58 行）是**手工列表**，新增 `.c` 必须逐条加进去，否则不参与编译链接。
- `Invoke-Clean`（`build.ps1:190-216`）**未覆盖** `dev/`、`net/` 目录，需补。
- `shell.c` 的 `MAX_CMDS = 32`（`shell.c:24`），当前已注册 **25** 条（`shell.c:140-164`）→ 只剩 7 个名额，本轮用 4 个。
- `printk` 仅支持 `%d %u %x %p %s`（无 `%llu/%llx`），64 位值需显式 `cast` 后用 `%u/%x`。
- 引导扇区最多加载 `KERNEL_SECTORS = 1007`（≈503 KB），当前 `kernel.flat` 约 137 KB；本轮预计 +35 KB，仍在限内（`build.ps1:144-148` 会硬性拦截）。
- QEMU 启动参数**当前完全没有网卡**，必须显式加 `-device rtl8139`（`-machine pc` 的默认网卡类型不保证是 RTL8139）。

---

## 3. 建议改动

### 3.1 新增文件

#### (1) `src/kernel/dev/pci.h` / `src/kernel/dev/pci.c` —— PCI 枚举与驱动匹配（≈220 行）

- 常量：`PCI_CONFIG_ADDR 0xCF8`、`PCI_CONFIG_DATA 0xCFC`。
- 读写原语（基于 `port.h` 的 `ind/outd`）：
  `pci_read32(bus,dev,func,off)` / `pci_read16` / `pci_read8` / `pci_write32`。
  32 位地址字格式：`0x80000000 | bus<<16 | dev<<11 | func<<8 | (off & 0xFC)`。
- 设备结构（最多 32 项静态数组）：
  ```c
  typedef struct {
      uint8_t  bus, dev, func;
      uint16_t vendor, device;
      uint8_t  class, subclass, progif, header;
      uint32_t bar[6];
      uint8_t  irq;
  } pci_device_t;
  ```
- `void pci_init(void)`：扫描 bus 0–255 / dev 0–31；`header & 0x80` 时展开 func 0–7；填充表并打印一行摘要（vendor/device/class）。
- `pci_device_t* pci_find(uint16_t vendor, uint16_t device)`、`pci_device_t* pci_find_class(uint8_t cls, uint8_t sub)`、`int pci_count(void)`、`pci_device_t* pci_at(int i)`。
- BAR 解析：`bar & 1` → I/O 端口（`base = bar & ~0x3`）；否则 MMIO（`base = bar & ~0xF`）。
- **不加** MSI/IRQ 相关代码（架构无中断）。

#### (2) `src/kernel/net/net.h` —— 核心抽象（≈130 行）

- 网卡抽象：
  ```c
  typedef struct net_device {
      char     name[16];        /* "eth0" */
      uint8_t  mac[6];
      uint32_t ip, mask, gw;    /* 主机字节序 */
      uint64_t io_base;         /* I/O 端口或 MMIO 基址 */
      int      is_mmio;
      /* 统计 */
      uint64_t rx_packets, tx_packets, rx_errors, tx_errors, rx_dropped;
      void*    priv;            /* 驱动私有（rtl8139_priv_t*） */
      int  (*init)(struct net_device*);
      int  (*send)(struct net_device*, const void* data, uint32_t len);
      int  (*poll)(struct net_device*);   /* 返回本轮处理的包数 */
  } net_device_t;
  ```
- 协议常量：`ETH_TYPE_IP 0x0800`、`ETH_TYPE_ARP 0x0806`、`IP_PROTO_ICMP 1`、`IP_PROTO_UDP 17`、`ETH_ALEN 6`、`ETH_HDR_LEN 14`、`MTU 1500`。
- **字节序辅助（当前全仓缺失，必须新增）**：`net_htons/ntohs/htonl/ntohl`（x86 小端，用 `__builtin_bswap16/32`）。
- 对内接口：`net_init()`、`net_poll()`、`net_tx(frame,len)`、`net_rx(frame,len)`、`net_register_device()`、`net_default(void)`、`net_set_addr(ip,mask,gw)`。

#### (3) `src/kernel/net/net.c` —— 设备表与轮询入口（≈180 行）

- `net_register_device(net_device_t*)`、`net_default()`（返回第一个已初始化设备）。
- `net_init()`：调 `pci_init()`（若尚未调用）→ 按匹配表 probe（当前只有 `0x10EC:0x8139`）→ 设置默认地址 **10.0.2.15 / 255.255.255.0 / gw 10.0.2.2**（QEMU user-mode 约定）→ 打印 `[NET] eth0 <mac> 10.0.2.15/24`。
- `net_poll()`：遍历设备：`dev->poll(dev)`；驱动内部收包时回调 `net_rx()`。无设备则为空操作（保证无网卡时不影响启动与现有测试）。
- `net_tx()`：`dev->send()`；设备不存在时计 `tx_errors` 并返回 -1。
- `net_rx(frame,len)`：入口即做长度与类型检查，按 EtherType 分发到 `arp_input()` / `ip_input()`（见 3.2）。

#### (4) `src/kernel/net/rtl8139.h` / `rtl8139.c` —— 网卡驱动（≈380 行）

- 寄存器偏移（I/O BAR0）：`IDR0 0x00`、`TSD0 0x10`、`TSAD0 0x20`、`RBSTART 0x30`、`CR 0x37`、`CAPR 0x38`、`CBR 0x3A`、`IMR 0x3C`、`ISR 0x3E`、`TCR 0x40`、`RCR 0x44`、`CONFIG1 0x52`、`CMD 0x37`。
- `int rtl8139_probe(pci_device_t* d)`：匹配 `vendor==0x10EC && device==0x8139`；使能 PCI 命令寄存器的 **I/O space(bit0)** 与 **bus master(bit2)**（`pci_write32` 到 config `0x04`）。
- `rtl8139_init(net_device_t* dev)` 步骤：
  1. 读 `IDR0..IDR5` 得到 MAC（QEMU 已预填，无需写 EEPROM 93C46 时序）；
  2. `outb(CR, 0x10)` 软复位，轮询 `CR` bit4 直到清零；
  3. `pmalloc_contig(3)` 分配 **12 KB RX 环**（8 KB 环 + 16 B 头 + 1500 B 余量，4 KB 页对齐），`outd(RBSTART, phys)`；
  4. `pmalloc_contig(4)` 分配 4 个各 2 KB 的 TX 缓冲；
  5. `RCR = 0x0000000F | (1<<7) | (1<<15)`（AB+APM+AM+W，允许 >4K？取常规组合），`TCR = 1<<8 | 1<<24`（IFG/默认）；
  6. **`IMR = 0`**（禁用全部中断，纯轮询）；`ISR` 写清；
  7. `CAPR = CBR`；`CR = 0x08 | 0x04`（RE | TE，开始收发）。
- `static int rtl8139_poll(net_device_t*)`：
  - 读 `ISR`，若 `ROK` 位被置则处理，处理完 `outw(ISR, 0x0001|0x0002|0x0004|0x0010)` 写清（W8 只关心 ROK/TOK/ERR/RXOVW）；
  - 用 `CBR` 与本地 `capr` 比较判断是否有新包：包长在 `rx_ring[capr]` 起始的 4 字节（含 4 字节 CRC），数据结构为 `[status(2)][length(2)][payload][CRC(4)]`；
  - 提取 payload → `net_rx(buf, len)`；更新 `capr`（16 字节对齐 + 环绕 8 KB），写回 `CAPR`；
  - 单次 `poll` 最多处理 **16 个包**，避免长时间占用主循环。
- `static int rtl8139_send(net_device_t*, const void* data, uint32_t len)`：
  - 轮转 4 个 TX 槽（`tx_idx`），`memcpy` 到该槽缓冲；
  - `outd(TSAD0 + 4*i, phys)`、`outd(TSD0 + 4*i, len)`；
  - 轮询 `TSD` bit15（TOK）或 bit14（TABT），**带 `timer_ms()` 超时 10 ms**（超时计 `tx_errors` 并返回 -1）。
- 不注册 devfs 节点（避免动 vfs 层）；状态经 `ifconfig` 命令暴露。

#### (5) `src/kernel/net/eth.h` / `eth.c` —— Ethernet II + ARP（≈200 行）

- `eth_hdr_t`（`__attribute__((packed))`，14 B）、`arp_hdr_t`（28 B）。
- ARP 表：`struct { uint32_t ip; uint8_t mac[6]; uint64_t ts; int valid; } g_arp[16]`，老化阈值 300 s（`timer_ms()`）。
- `int arp_lookup(uint32_t ip, uint8_t out[6])` / `void arp_input(const uint8_t* pkt, uint32_t len)` / `void arp_request(uint32_t ip)` / `void arp_flush_stale()`。
- **pending 槽（1 个）**：`ip_send()` 遇到 ARP 未命中时，把该包指针暂存（数据拷贝进一个 1520 B 静态缓冲），发 ARP 请求；收到 reply 后立即补发。这解决了"首个 ping 必然失败"的问题，且不引入队列管理复杂度。
- `eth_send(dst_mac, ethertype, payload, len)` → 填 14 B 头 → `net_tx()`。
- `arp_input` 处理：request（目标 IP == 本机 → 回 reply，并顺手学习发送方）、reply（更新表 + 触发 pending 补发）。

#### (6) `src/kernel/net/ip.h` / `ip.c` —— IPv4 + ICMP（≈300 行）

- `ip_hdr_t`（packed，20 B 标准头）。
- `uint16_t ip_checksum(const void* data, uint32_t len)`（one's complement，用于 IP 头与 ICMP）。
- `int ip_send(uint32_t dst, uint8_t proto, const void* payload, uint32_t len)`：
  填头（`version=4, ihl=5, ttl=64, id` 递增）→ 算校验和 → `arp_lookup` 拿目的 MAC → `eth_send(ETH_TYPE_IP, …)`；ARP 未命中走 pending 槽。
- `void ip_input(const uint8_t* pkt, uint32_t len)`：
  校验 `version==4`、`ihl>=5`、`tot_len<=len`、头校验和；**若 `frag` 字段非 0（MF 置位或 offset≠0）→ 直接丢弃并计 `rx_dropped`**（见假设 4.2）；否则按 `proto` 分发 `icmp_input` / `udp_input`。
- ICMP（同文件或独立 `icmp.c`，本轮放同文件以省一个编译单元）：
  - `icmp_input(pkt,len,src_ip)`：`type 0`（echo reply）→ 记 `g_ping_replies`/RTT；`type 8`（echo request）→ 构造 reply 发出；`type 3`（unreachable）→ 计数（用于验证 UDP 发送路径）；
  - `void icmp_send_ping(uint32_t dst)`：发 `type 8`，记 `seq` 与时间戳；
  - 导出 `ping_sent/ping_recv/ping_last_rtt_ms` 供 `ping` 命令打印。

#### (7) `src/kernel/net/udp.h` / `udp.c` —— UDP（≈180 行）

- `udp_hdr_t`（packed，8 B；校验和填 0，IPv4 下合法）。
- 绑定表：`g_udp[16] { uint16_t port; int used; uint64_t owner_pid; uint8_t rxq[8][512]; int len[8]; int head, tail; }`（每端口固定 8 个 512 B 槽的环形队列，避免动态分配）。
- `int udp_bind(uint16_t port, uint64_t pid)`（冲突返回 `-98 EADDRINUSE`）/ `udp_unbind(port)`。
- `int udp_send(uint32_t dst, uint16_t dport, const void* data, uint32_t len)`。
- `void udp_input(pkt,len,src_ip)`：查表 → 入队 → `sched_wake_cond(WAIT_NET, port)`。
- `int udp_recv(uint16_t port, void* buf, int max, uint32_t* from_ip)`：无数据返回 0（由调用方决定是否 `sched_block_and_switch(WAIT_NET, port)`）。

#### (8) `src/kernel/net/socket.c` —— socket 系统调用分流层（≈200 行）

这是"分流"方案的落点：`table.c` 的注册改指向本文件，本文件按 `socket kind` 转发。

- `net_sock_t { int kind; /* 0 = loopback stream, 1 = udp */ void* priv; uint16_t port; }`，同样经 `file_t.private_data` 承载，`ops` 复用 loopback 的 recv/send/close（UDP 需要一对新的 `udp_ops`）。
- `sys_socket(domain, type, proto)`：
  - `AF_INET + SOCK_STREAM` → 调 `loop_socket()`（保持现有行为）
  - `AF_INET + SOCK_DGRAM` → 分配 `udp_ops` 文件对象，`kind=1`
  - 其他 → `-97 EAFNOSUPPORT` / `-93 EPROTONOSUPPORT`
- `sys_bind(fd, addr, len)`：`kind==1` → `udp_bind(port, current pid)`；`kind==0` → `loop_bind()`
- `sys_sendto(fd, buf, len, flags, addr, alen)`：`kind==1` → 解析 `sockaddr_in` → `udp_send()`；`kind==0` → `loop_sendto()`
- `sys_recvfrom(fd, buf, len, flags, addr, alen)`：`kind==1` → `udp_recv()`；**无数据且非 O_NONBLOCK → `sched_block_and_switch(WAIT_NET, port)` 后重试**（关键：让主循环收包）；`kind==0` → `loop_recvfrom()`
- `sys_connect/accept/listen/shutdown`：`kind==0` → 直接转发 `loop_*`；`kind==1` → `connect` 仅记录默认目的地址并返回 0（UDP 无连接语义），其余返回 `-95 EOPNOTSUPP`
- 统一在 `close` 时释放端口绑定（复用现有 loop 池归还逻辑 + `udp_unbind`）

#### (9) `src/kernel/net/loopback.h` —— 导出 loop_* 原型（新增，≈30 行）

把 `loopback.c` 中现有的 8 个 `sys_*` 函数**重命名为 `loop_*`** 并在本头文件声明，供 `socket.c` 调用。函数体逻辑与返回值**完全不变**，只改名字与可见性声明。

### 3.2 修改现有文件

| 文件 | 改动 | 为什么 / 怎么做 |
|---|---|---|
| `src/kernel/net/loopback.c` | 8 个 `sys_xxx` 改名为 `loop_xxx`；`#include "loopback.h"` | 为分流层让出 `sys_*` 名字；逻辑零改动，`test_tcp.S` 回归行为不变 |
| `src/kernel/syscall/table.c` | 注册区（2489–2496）改为指向 `socket.c` 的 `sys_*`；`#include "net/socket.h"` 或 extern 声明 | 分流层接管 41-45/48-50；syscall 编号与名字完全不变，用户态无感 |
| `src/kernel/sched/sched.h` | `WAIT_*` 后新增 `#define WAIT_NET 9`（arg = UDP 端口号） | UDP 阻塞读的唤醒条件 |
| `src/kernel/kmain.c` | ① `boomerang_init()`(201) 之后、`ata_init()`(204) 之前插入 `pci_init();`<br>② `devfs_init()`(227) 之后插入 `net_init();`（放在 `shell_init()`(256) 之前）<br>③ 主循环 490 行 `doorbell_poll(0)` 块之后插入 `net_poll();` | ①PCI 扫描只需 `port.h`，放设备探测段；②`net_init` 需要 `pmalloc_contig`（故在 `mm_init` 之后）且要在 shell 命令注册前完成；③唯一的轮询入口，位置遵循"键盘→调度→门铃→网卡→热度→降频" |
| `src/kernel/core/shell.c` | 新增 4 条命令 `lspci` / `ifconfig` / `arp` / `ping`：前向声明区（40–64）+ `shell_init()` 注册（140–164）+ 4 个 `static void cmd_xxx(int argc, char** argv)` 实现 | 命令数 25 → 29，未超 `MAX_CMDS=32`；`ping` 用 `argv[1]` 解析点分十进制 IP（需自写 `parse_ip()`，只有十进制 `atoi_s` 可用） |
| `build.ps1` | ①`$cSrcs`（41–58）加入 8 个新 `.c`<br>②3 条 QEMU 命令行（229/235/241）加 `-device rtl8139,netdev=n0 -netdev user,id=n0`<br>③`Invoke-Clean`（190–216）补 `src/kernel/dev/*.o` 与 `src/kernel/net/*.o` | 编译列表手工维护，漏加则新文件不参与链接；无网卡参数则驱动探测不到设备；clean 遗漏会留脏对象 |
| `iso_s_imports.ps1` / `test_bash_restart.ps1` | QEMU 参数同步加 `-device rtl8139,netdev=n0 -netdev user,id=n0` | 保持与 `build.ps1` 环境一致；两者断言只匹配串口文本（`Running /bin/bash`、`home`、`OK1..10`），加网卡不影响判定 |
| `.gitignore` | 在 `/test_*.ps1` 后加 `!/test_net.ps1` | 新增的 W8 验收脚本需要入库 |
| `README.md` / `README.zh-CN.md` | 新增 "Network stack (W8)" 小节：网卡参数、`lspci/ifconfig/arp/ping` 命令、IP 配置约定（10.0.2.15/24 → gw 10.0.2.2）、当前限制（无 TCP/无分片/轮询收包） | 让 clone 者知道怎么跑与边界在哪 |

### 3.3 主循环的最终形态

```
1. 调度（sched_next → 运行用户/内核任务）
2. 唤醒条件轮询（kbd_has_key / sched_poll_timeouts / sched_poll_signals）    ← 现有 371-377
3. 前台任务结束检测（bash 登录循环）                                          ← 现有 379-401
4. 内核 Shell 逐字符输入                                                      ← 现有 403-480
5. 定时任务与功耗：timer_ms → hotness_age → policy_apply
      → doorbell_poll(0)
      → net_poll();          ★ 新增（唯一插入点）
      → sched_tick()
6. 空载 pause
```

`net_poll()` 无设备时立即返回，保证在无网卡环境（未加 `-device rtl8139`）下行为与今天完全一致。

### 3.4 新增验收脚本 `test_net.ps1`（入库）

结构复刻 `iso_s_imports.ps1`（`$env:QEMU` 解析 + `-MessageData` 日志 + 串口断言），断言序列：

1. `Running /bin/bash` → 启动成功
2. `lspci` → 输出含 `10EC:8139`
3. `ifconfig` → 输出含 `eth0` 与 MAC（形如 `52:54:00:12:34:56`）与 `10.0.2.15`
4. `ping 10.0.2.2` → 输出 `4/4 replies`
5. 退出码 0 表示通过

---

## 4. 假设与决策

1. **架构不变**：不引入中断、不引入 DMA 映射层、不引入内核线程。收包只在主循环发生。
   - 已知代价：用户态程序长时间不间断计算时（不阻塞、不让出）会丢包。当前所有交互路径（bash 等键盘、`recvfrom` 等网络）都会阻塞让出，因此**交互式与阻塞式网络场景可正常工作**。这一点会写进 README 的"已知限制"。
2. **TCP 不在本轮**：简化 TCP 也需状态机 + 序列号 + 重传 + 窗口，风险最高。本轮把 socket 分流层与 UDP 路径打通，W8.1 只需在 `socket.c` 里加 `SOCK_STREAM + 非环回` 分支。
3. **不移除 loopback**：`127.0.0.1` 全部保持现状，`tools/test_tcp.S` 回归零改动。
4. **IPv4 分片不做**：收到 `MF=1` 或 `frag_off≠0` 的包直接丢弃并计 `rx_dropped`。理由：重组需要额外缓冲池、超时与状态管理，而 QEMU user-mode 网络下 MTU 1500 且我们的包都远小于 MTU，实际遇不到分片。**这是对原草案 3.5 的偏离**，如需保留请告知。
5. **RTL8139 优先**：不实现 e1000（W9+）。
6. **不收 EEPROM**：直接读 `IDR0..5`，QEMU 已在复位后填好 MAC。
7. **`curl` 不作为验收手段**：`tools/rootfs` 中无 `curl`；`python3` 的 socket 模块会引入 epoll 等未实现 syscall。UDP 验收用内核 `udp` 相关命令与 `test_net.ps1` 完成（`ping` 为硬性通过项；UDP 通过发送计数 + ICMP port-unreachable 计数证明双向路径）。
8. **稳定性测试缩短**：原草案"24 小时"改为"`test_net.ps1` × 10 轮 + 一次 30 分钟长跑（反复 ping）"，因为不可自动化 24 小时。
9. **本计划文件位置**：写入 `.trae/documents/`。该目录此前已归档到 `docs/engineering-log/`，实施完成后建议把本文件移入 `docs/specs/w8-drivers-network/spec.md` 或删除。

---

## 5. 验证步骤

1. **编译**：`.\build.ps1 -SkipFs` 通过；确认 `[OK] kernel.flat` 仍 < 503 KB（关注增长量，预计 ≈ +35 KB）。
2. **无网卡回归（防止破坏现有功能）**：不改 QEMU 参数启动 → `net_init` 输出"no device" → 跑 `iso_s_imports.ps1` 得 **10/10 ok**、`test_bash_restart.ps1` 得 **PASSED**（证明新增轮询与 socket 分流未破坏既有路径）。
3. **PCI**：带网卡启动 → `lspci` 输出含 `10EC:8139`（以及 IDE `8086:7010`、VGA `1234:1111`）。
4. **驱动**：`ifconfig` 显示 `eth0`、MAC、`10.0.2.15/24`、gw `10.0.2.2`、TX/RX 计数。
5. **ARP**：`ping 10.0.2.2` 后 `arp` 显示 `10.0.2.2 → 52:54:00:12:35:02`（QEMU 网关 MAC）。
6. **ICMP**：`ping 10.0.2.2` 输出 `4/4 replies`、RTT 数值。
7. **UDP 发送**：`udp send 10.0.2.2 5555 8`（新增子命令或用 `ping` 同族命令）→ `ifconfig` 的 TX 计数增加；对未监听端口应收到 ICMP type 3 并计入 `rx_dropped`/`icmp_unreach`。
8. **socket 分流**：`iso_s_imports.ps1`（走 loopback 的 `test_tcp.S` 等价路径不变）+ 新增 `test_net.ps1` 全绿。
9. **自动化**：`powershell -NoProfile -ExecutionPolicy Bypass -File .\test_net.ps1` → `=== NET TEST PASSED ===`，连续 10 轮无失败。
10. **长跑**：30 分钟内每 30 s 一次 `ping`，无 panic、无 `user fault`、RX/TX 计数单调增长、无内存泄漏迹象（`mem` 命令的 used 值稳定）。
11. **提交**：`git status` 干净，单次提交（或按阶段 1-2 / 3-4 两次提交），推送后核对 `ls-remote` 与本地一致。

---

## 6. 工作量与风险

| 模块 | 文件 | 预估行数 |
|---|---|---|
| PCI 枚举 | `dev/pci.{c,h}` | 220 |
| 驱动框架/设备表 | `net/net.{c,h}` | 310 |
| RTL8139 驱动 | `net/rtl8139.{c,h}` | 380 |
| Ethernet + ARP | `net/eth.{c,h}` | 200 |
| IPv4 + ICMP | `net/ip.{c,h}` | 300 |
| UDP | `net/udp.{c,h}` | 180 |
| socket 分流 | `net/socket.c`、`net/loopback.h` | 230 |
| shell 命令 ×4 | `core/shell.c`（改） | 180 |
| 验收脚本 | `test_net.ps1` | 90 |
| **合计** | **8 新 .c + 5 新 .h** | **≈1,900**（含头文件） |

| 风险 | 应对 |
|---|---|
| RTL8139 RX 环指针同步（CAPR/CBR）写错 → 收不到包或死循环 | 单次 `poll` 限 16 包；`capr` 严格 16 字节对齐并环绕 8 KB；用 `ifconfig` 计数与 `ping` 逐步验证 |
| 无中断导致丢包 | 已作为已知限制写入 README；所有阻塞路径都会让出 CPU 给主循环 |
| 手工 `$cSrcs` 漏加文件 → 链接期 undefined | 编译后立刻检查 `kernel.bin` 是否含新符号（`objdump -t`） |
| `shell.c` 命令表上限 | 25 + 4 = 29 < 32，留 3 个余量 |
| ARP 首次未命中导致首个 ping 丢失 | pending 槽：ARP reply 到达后自动补发 |
| 变更 socket 注册影响现有 bash/python 行为 | 第 2 步"无网卡回归"先证明既有测试 10/10 通过 |

---

## 7. 后续（W8.1 / W9+）

- **W8.1**：简化 TCP（三次握手 / 数据流 / 四次挥手）+ `SOCK_STREAM` 非环回分支 + `curl`/自研客户端验收
- **W9+**：e1000 驱动、DHCP/DNS、多网卡、可选 MSI/MSI-X 中断收包、IPv4 分片重组

---

## 8. 实施结果（W8 收尾）

状态：**已完成**（TCP 按计划留 W8.1）。

### 8.1 验收

| 项 | 结果 |
|---|---|
| `build.ps1 -SkipFs` | 通过，`kernel.flat` 156,564 B（上限 503 KB） |
| `test_net.ps1` | **NET TEST PASSED，12/12**（netinit/bash/shell/lspci/ifconfig/mac/addr/ping/arp/udp/udpecho/udpstat） |
| `test_net.ps1` × 10 轮 | 10/10 `exit=0` |
| `iso_s_imports.ps1` | 10/10 ok（带网卡参数） |
| `test_bash_restart.ps1` | PASSED（带网卡参数） |
| 长跑（缩短版） | 20×`ping` 全 4/4，RX=81 / TX=81、errors=0、dropped=0、无 panic |

### 8.2 实施中发现并修复的真实缺陷（供 W8.1 参考）

1. **RTL8139 `CAPR` 初值**：CAPR 语义是「已读指针 − 16」，初值 0 必须写成 `0xFFF0`。
   写 0 会让 QEMU 认为还有 16 字节未读 → `can_receive` 判定环形缓冲空间不足 →
   **所有收包在进入驱动前被丢弃**（现象：TX 正常、RX 恒 0，errors/dropped 也都是 0）。
2. **`ip_checksum()` 字节序**：必须返回**网络字节序**（`net_htons(~sum)`）才能直接写入报头；
   返回主机序会让 IP/ICMP 校验和在线路上字节颠倒 → slirp 静默丢包。
3. `build.ps1` 曾为无 BOM 的 UTF-8 + 中文注释 → PowerShell 5.1 按 ANSI 解码产生语法错误；
   且 `$cSrcs` 曾漏加 7 个 `net/*.c`（新文件不参与链接）。两者已修，提交前勿回退。

排查手段（可复用）：QEMU `-object filter-dump,id=fd0,netdev=n0,file=x.pcap` 抓 netdev 上
全部帧，先判断「包有没有出去 / 回复有没有到」；再对照 QEMU `hw/net/rtl8139.c` 的
`can_receive` / `RxBufPtr_write` 确认寄存器语义。

### 8.3 UDP 端到端验证方式

`test_net.ps1` 在宿主起一个只绑 `127.0.0.1:5555` 的 UDP 回显监听：guest 的
`udp send 10.0.2.2 5555 8` 经 slirp 转发到宿主 loopback，宿主原样回显，guest 断言
`reply 8 bytes from …:5555` + `data=BANANAUD` + `udp stat` 计数增长。
**注意**：未监听端口时 slirp 通常**不会**回 ICMP port-unreachable（Windows 尤其如此），
因此 `icmp_unreach` 只作诊断打印、不作断言；反向验证（把宿主监听改到 5556）确认三项断言
会 FAIL、退出码 1。

### 8.4 遗留

- TCP（状态机 / 重传 / 窗口）与 `curl` 级验收 → W8.1
- 24 小时长跑 → W8.1 或发布前；本轮用 `test_net.ps1` ×10 + 80 次 ping 覆盖
- `udp_send()` 仍把源端口设为目的端口（简化）；改为随机源端口时需同步调整 `udp send` 命令
- 未做 DHCP/DNS/netfilter/IPv4 分片重组（原计划即如此）
