# Banana_OS

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

一个**从零自研的 x86-64 操作系统内核**：从手写的 BIOS 引导扇区启动，在自研
EXT2 驱动与 Linux 兼容系统调用层之上运行**真实的 Ubuntu 用户态** —— GNU
bash 5.2、coreutils、GCC 11、GNU Make、Python 3.10、Vim 都以用户进程身份运行。

> English documentation: [README.md](README.md)

---

## 核心特性

| 子系统 | 设计 |
|---|---|
| **引导** | 16 位 BIOS 引导扇区（`bootsect.S`，CHS）→ `kernel.flat` → 长模式入口（`boot.S`）。同时内嵌 Multiboot2 头，因此 `qemu -kernel kernel.bin` 亦可启动。 |
| **Ω 弹性内存** | 三区物理位图分配器 —— 热区 `[8MB, 1.03GB)`、温区、回旋镖池 —— 配套热页释放缓存、SWAR 热度统计与 XOR 页压缩。 |
| **Ψ 公平调度** | 5 级 MLFQ，支持优先级老化与前台抢占；协作式切换（不对用户任务做定时抢占）。 |
| **Δ 门铃 IPC** | 64 通道，每任务 ACL 权限掩码；唤醒延迟目标 < 1 µs。 |
| **Σ 功耗管家** | 基于 `MONITOR`/`MWAIT` 的 C-state 管理、频率导向与策略引擎；空闲 AP 进入 C-state，通过专属门铃通道唤醒。 |
| **SMP** | INIT-SIPI 启动 AP，门铃唤醒，逐核状态上报（`power status`）。 |
| **虚拟内存** | 每任务独立 4 级页表；内核恒等映射保留在监管者高地址区间，用户页以 `U/S=1` 映射。按需把 2MB/1GB 巨页懒拆分为 4KB。 |
| **文件系统** | VFS + devfs / tmpfs / procfs / sysfs / pipe，以及只读 EXT2 驱动（4KB 块，解析超级块、组描述符、inode、目录、符号链接），底层为 ATA PIO 驱动（28-bit LBA，轮询，无 DMA）。 |
| **用户态** | ELF 加载器支持 `PT_INTERP`（经 `ld-linux` 动态链接）、`syscall`/`sysret` 入口、带 sigframe 与 restorer 桩的 POSIX 信号、`fork`/`clone`/`execve`/`wait4`、futex 等待队列。 |
| **Linux ABI** | `src/kernel/syscall/table.c` 中实现 60+ 个 Linux/x86-64 系统调用，足以运行 Ubuntu 22.04 (jammy) 的 glibc 二进制。 |

---

## 仓库结构

```
.
├── src/
│   ├── boot/                 # bootsect.S、boot.S、AP trampoline
│   ├── include/              # axion.h、bananaos.h —— 共享类型与常量
│   └── kernel/
│       ├── core/             # kmain、GDT/IDT、VGA、键盘、定时器、shell、printk
│       ├── mm/               # Ω 分配器、vm、热度、压缩、回旋镖池
│       ├── sched/            # Ψ MLFQ 调度器
│       ├── ipc/              # Δ 门铃 IPC
│       ├── power/            # Σ cstate / freq / policy
│       ├── syscall/          # 系统调用入口、分发表、信号、共享内存
│       ├── elf/              # ELF 加载器、解释器、内嵌 hello.elf
│       ├── fs/               # VFS、devfs、tmpfs、procfs、sysfs、pipe、ext2
│       ├── dev/              # ATA PIO 块设备
│       └── net/              # 回环网络
├── tools/                    # rootfs 获取、EXT2 镜像构建、ELF 生成脚本
├── iso/grub/                 # GRUB 配置（早期遗留，build.ps1 不使用）
├── x86_64-elf/               # 交叉 binutils   ← 不入库，见下文
├── .trae/                    # 设计规格与修复报告（文档，不参与构建）
├── linker.ld                 # 内核装载地址 0x100000、PVH note、段布局
├── build.ps1                 # ← 权威构建/运行脚本
├── clean_asm.ps1             # 清理 GCC 生成指令，供 ELF 汇编器使用
└── Makefile                  # 早期构建脚本（路径已过时，见说明）
```

### 用户/内核地址布局

| 区间 | 用途 |
|---|---|
| `0x00100000` | 内核镜像（1 MB，Multiboot 约定） |
| `0x00800000 – 0x40800000` | Ω 热区 |
| `0x40800000 – 0x60800000` | 温区 |
| `0x60800000 – 0x70800000` | 回旋镖池 |
| `0xC0000000 – 0x100000000` | LAPIC |
| `0x4000000000` | **用户程序装载基址**（256GB —— 高于全部内核物理区，用户映射永不可能遮蔽内核的恒等映射访问） |
| `0x8000000000` | 用户栈顶 |
| `0x7F0000000000` | `ld-linux` / 动态链接器 |
| `0x7F0000100000+` | `mmap` 提示区 |

---

## 环境准备

- **Windows 10/11** + PowerShell（构建系统基于 PowerShell）
- **MinGW-w64 GCC**，命令名 `gcc`（仅作 C→汇编前端使用，内核以 `-ffreestanding` 构建）
- **x86_64-elf 交叉 binutils**：`as`、`ld`、`objcopy` —— 放入 `./x86_64-elf/bin/`
- **Python 3**（`fetch_deps.py`、`ext2_mkfs.py`、`gen_*.py`）
- **QEMU** —— 默认路径 `D:\qemu\qemu-system-x86_64.exe`，可在 `build.ps1` 中修改

`./x86_64-elf/`（约 100 MB）与 `./tools/rootfs/`（约 470 MB）**刻意不入库**，
按下面步骤重建即可。

---

## 快速开始

```powershell
# 1. 工具链：把 x86_64-elf binutils 放到 ./x86_64-elf/bin/
#    （可用 --target=x86_64-elf 自行编译 binutils，或使用现成的
#      x86_64-elf Windows 工具链发行版）

# 2. 获取 Ubuntu jammy 用户态（解包后约 470 MB）
#    下载 .deb 到 tools/debs/，解包到 tools/rootfs/，
#    并重新生成 tools/fs_manifest.txt（EXT2 注入清单）
python tools/fetch_deps.py

# 3. 编译内核与磁盘镜像
.\build.ps1 -SkipFs      # 只编译内核（disk.img）
.\build.ps1              # 编译内核 + 依 tools/rootfs 重建 fs.img

# 4. 启动
.\build.ps1 run          # QEMU，串口控制台
.\build.ps1 run-gui      # QEMU，图形窗口
.\build.ps1 debug        # QEMU 等待 GDB（:1234）
.\build.ps1 clean        # 清理构建产物
```

第 4 步实际执行的命令：

```
qemu-system-x86_64 -drive format=raw,file=disk.img \
                   -drive format=raw,file=fs.img \
                   -m 2G -nographic -no-shutdown \
                   -device rtl8139,netdev=n0 -netdev user,id=n0 \
                   -smp 4,sockets=1,cores=4,threads=1 -machine pc,accel=tcg
```

其中 `fs.img` 是由 `tools/ext2_mkfs.py` 按清单构建的 EXT2 根盘
（512 MB，4096 字节块）；`disk.img` 是承载引导扇区与 `kernel.flat` 的引导盘。

### 构建产物

| 文件 | 含义 | 典型大小 |
|---|---|---|
| `kernel.bin` | 链接后的 ELF | ~171 KB |
| `kernel.flat` | 引导扇区加载的平坦二进制 | ~137 KB |
| `bootsect.bin` | 512 字节引导扇区 | 512 B |
| `disk.img` | 引导盘（扇区 0 + 内核） | ~516 KB |
| `fs.img` | EXT2 根文件系统 | 512 MB |

引导扇区最多读取 `KERNEL_SECTORS = 1007` 个扇区（约 503 KB）的内核镜像，
`kernel.flat` 的体积增长受此上限约束。

---

## Shell 命令

内核内置 Shell（`src/kernel/core/shell.c`）提供：

`help` `hello` `mem` `alloc` `free` `clear` `reboot` `panic` `echo` `stats`
`compress` `ps` `kill` `ipc` `power` `run` `jobs` `fg` `bg` `ls` `cat` `dmesg`
`fsalloc` `fsfree` `wfile` `lspci` `ifconfig` `arp` `ping`

启动完成后，内核把控制权交给 EXT2 盘上的 `/bin/bash`，得到带作业控制的完整
交互式 Shell。`lspci` / `ifconfig` / `arp` / `ping` 属于内核 Shell：只有在没有
前台程序运行时内核 Shell 才接管键盘，因此这四条命令需在 `/bin/bash` 无法加载
的启动中使用（见 `test_net.ps1`）。

---

## 网络栈（W8）

W8 加入 PCI 枚举、真实网卡驱动与 IPv4 协议栈，全部在无中断架构下运行——主循环
轮询网卡，因此驱动是唯一的收包路径，阻塞式 socket 读必须主动让出 CPU。

| 层次 | 源码 | 说明 |
|---|---|---|
| PCI 总线 | `src/kernel/dev/pci.{c,h}` | 通过 `0xCF8/0xCFC` 读配置空间，解析 BAR，驱动匹配表 |
| 网卡驱动 | `src/kernel/net/rtl8139.{c,h}` | RTL8139（QEMU 默认网卡），PIO，4 个发送描述符，12 KB 接收环，轮询 |
| 链路层 ARP | `src/kernel/net/eth.{c,h}` | Ethernet II，16 项 ARP 缓存，单个 pending 帧槽保证首个包不丢 |
| IPv4 + ICMP | `src/kernel/net/ip.{c,h}` | 校验和验证、echo 请求/应答、ping 统计 |
| UDP | `src/kernel/net/udp.{c,h}` | 8 个绑定 × 4 个数据报，通过 `WAIT_NET` 唤醒阻塞读 |
| socket 分流 | `src/kernel/net/socket.c` | `AF_INET+SOCK_DGRAM` → UDP/网卡，`AF_INET+SOCK_STREAM` → 内存回环（127.0.0.1） |

QEMU user-mode 网络把 guest 地址固定为 **10.0.2.15/24**、网关 **10.0.2.2**，
这两个值硬编码在 `net_init()` 中。验证命令：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\test_net.ps1   # 期望 NET TEST PASSED
```

当前限制：仅 IPv4；TCP 不上线（仅回环）；不做分片重组（分片包直接丢弃并计数）；
无 DHCP/DNS/netfilter；无中断与 DMA 映射层。

---

## 测试

仓库内附带三个回归脚本，通过串口驱动 QEMU；其余测试脚本仅在本地开发目录
（见 `.gitignore`）。

```powershell
.\build.ps1                                                                  # 先构建
powershell -NoProfile -ExecutionPolicy Bypass -File .\iso_s_imports.ps1      # 10 次 python3 import — 期望 10/10 ok
powershell -NoProfile -ExecutionPolicy Bypass -File .\test_bash_restart.ps1  # bash 退出/登录循环 — 期望 PASSED
powershell -NoProfile -ExecutionPolicy Bypass -File .\test_net.ps1           # PCI + 网卡 + ARP + ICMP — 期望 NET TEST PASSED
```

三个脚本按 `$env:QEMU` → `.\qemu-system-x86_64.exe` → 默认 `D:\qemu\` 的顺序
定位 QEMU，并要求 `disk.img` / `fs.img` 已构建。`iso_s_imports.ps1` 是
fork/exec/ELF 回归门禁（反复执行 `python3 -c "import ..."`）；
`test_bash_restart.ps1` 验证 `exit` 后登录循环能重新拉起可用的 bash；
`test_net.ps1` 跑两次启动（带/不带 `fs.img`），既验证 `net_init()` 不影响 bash
登录路径，又驱动 `lspci` / `ifconfig` / `arp` / `ping 10.0.2.2`。

---

## 设计文档

[docs/engineering-log/](docs/engineering-log/) 与 [docs/specs/](docs/specs/)
保存了各里程碑（W1 闪电启动 → W7 用户态工具链）的规格与复盘，包括若干疑难
bug 的根因报告（恒等映射被用户页遮蔽、`wait4` 栈恢复、fd 表冲突、brk/mmap
堆损坏等）。这些文档作为工程日志保留在仓库中。

## 参与贡献

构建/测试环境、代码约定与 bug 报告清单见
[CONTRIBUTING.md](CONTRIBUTING.md)；本项目遵循
[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md)。

---

## 已知限制

- **无线程**：`clone()` 传入非 NULL 栈返回 `ENOSYS`；`fork()` 为地址空间全量
  深拷贝（无写时复制）。
- **协作式调度**：用户任务主动让出 CPU，ring 3 代码无定时抢占。
- **`mprotect` 为桩实现** —— 直接返回成功但不改变页权限；另有少数系统调用为
  兼容 glibc 的 no-op。
- **EXT2 只读**（写侧辅助函数仅供内核 Shell 使用）。
- 测试脚本默认使用 QEMU 路径 `D:\qemu\`，可用 `$env:QEMU` 覆盖。

---

## 许可证

本项目以 [MIT 许可证](LICENSE) 发布 —— © 2026 Vincentwen12。

`fs.img` 中的 Ubuntu 用户态**不适用**本许可证，详见下方致谢。

## 致谢

`fs.img` 中的用户态由 `tools/fetch_deps.py` 从 Ubuntu 22.04 (jammy) 的 `.deb`
包解包而来；这些二进制属于 Ubuntu，遵循各自许可证，并非本项目成果。`src/`
与 `tools/` 中的内核、引导程序、驱动、构建脚本与工具链脚本均为原创工作。
