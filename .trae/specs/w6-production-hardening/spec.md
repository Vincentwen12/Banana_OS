# Axion-Ban 生产级强化（W6 细化）Spec

change-id: `w6-production-hardening`

## Why

内核已具备完整"原型能力"——多核启动、ATA PIO 读取、EXT2 只读、动态链接 ELF、bash 交互（CPL3 运行）、fork/wait4/exit、系统调用框架，`bash-5.2# echo hello` 与 `help` 可跑通。但距"生产级"（崩溃自愈、进程隔离、可持久化、可观测、可长期运行）存在系统性缺口：

- 内核 identity map 位于低 0-2GB 且 U/S=1，**用户态可读写内核内存**（隔离未达成）
- 磁盘只读，**无法保存任何修改**（配置、文件）
- `kernel_panic` 直接挂死，**需硬重启、无诊断信息持久化**
- 无日志系统、无 /proc、无 /sys，**运行状态不可观测**
- 信号为空实现、终端 job control 不完整、无网络、无 POSIX IPC

按用户决策：**实施全部 3 阶段；放宽内核体积上限（80KB→512KB，改引导）；EXT2 完整写入+删除；Panic 5 秒后自动重启**。

## What Changes

按 4 个阶段（0=预算放宽，1=安全基础，2=日常可用，3=生产增强）落地。

### 阶段 0：内核体积预算放宽（前置，BREAKING）
- **BREAKING** 内核大小上限由 80KB 提升至 **512KB**：`src/boot/bootsect.S` 的 `KERNEL_SECTORS` 160→1024，加载方式由硬编码 CHS 三段改为 **LBA（int 13h AH=42h + DAP）** 循环读取；`prot_mode` 的 `rep movsl` 拷贝计数同步；`build.ps1` 的 `minKernelSectors` 与磁盘布局同步。
- 验收口径更新：`kernel.flat < 512KB`（仍鼓励紧凑实现）。

### 阶段 1：安全基础
- **内核/用户内存隔离**（修正用户侧"Ring 3 未实现"的过期判断——Ring 3 已实现，bash 现以 CPL3 运行；真正缺口是内存隔离）：
  - `vm_create()`：0-2GB identity 大页（`pdpt0[0]/[1]` 及 `pml4[0]`）移除 `PTE_USER`，内核区域仅 CPL0 可访问。
  - `vm_map_page()` 拆分大页时**仅目标页** `U/S=1`，兄弟页保持 `U/S=0`（当前拆分把整个 0-2GB 变 U/S=1）。
  - 用户可访问范围收敛为：ELF 段（0x400000 起）、用户栈（0x8000000000 起）、brk/mmap（0x600000 / 0x7F0000100000+）等显式映射页。
  - 检查并适配：DOORBELL_BASE / IPC_SHM_BASE 等共享物理区若需用户访问，必须显式 USER 映射；LAPIC 区（3-4GB）保持 U/S=0。
- **用户态异常终止进程**：IDT 异常处理器（`idt.c`）区分用户/内核态——用户态 `#PF/#GP/#UD/#DF` → 终止当前任务（复用 exit 展开路径，含 wait4 子进程场景），Shell 与其它进程不受影响；内核态 → 走 panic。
- **Panic 恢复机制**（`core/panic.c`）：
  - panic 时打印：异常/消息、当前进程名与 PID、`rbp` 栈回溯、寄存器快照（CR2/RIP/RSP）。
  - 将 panic 摘要写入专用磁盘扇区（panic.log，依赖阶段 1 EXT2 写入；若写入不可用则退化为内存保留）。
  - 5 秒倒计时后经 `0xCF9` 软重启。
  - 重启后若存在 panic 记录，打印 "Previous panic: ..."。
  - 连续 3 次 panic 进入安全模式（仅内核 Shell）。
- **EXT2 写入**（`fs/ext2.c`，读路径之上）：
  - 位图管理：inode 位图 / 块位图的分配与释放。
  - 组描述符更新与回写。
  - `ext2_write_inode()`、`ext2_write_block()`、`ext2_write_file(ino, off, data, size)`（含单间接块）。
  - `ext2_create_file(parent, name, mode)`、`ext2_mkdir()`、`ext2_unlink()`、目录项增删。
  - VFS 侧：`file_ops.write` 接盘；`sys_open` 支持 `O_CREAT|O_TRUNC|O_WRONLY`；`sys_write`/`sys_close` 贯通。

### 阶段 2：日常可用
- **printk 日志系统**（新增 `core/printk.c`）：8 级日志、默认 INFO、环形缓冲区（≥64 条）、`dmesg` 命令；panic 后内存保留，写入磁盘后重启不丢。
- **/proc**（新增 `fs/procfs.c`）：`/proc/self/status`、`/proc/uptime`、进程列表（pid/name/state）；Shell `ps` 命令基于 /proc 输出。
- **/sys** 基础（新增 `fs/sysfs.c`，只读参数：`/sys/kernel/...`，如版本、启动时间）。
- **多用户基础**：`/etc/passwd` 解析、`getuid/geteuid/getgid` 贯通、文件权限位（owner 位）检查（`access`/`open` 校验）。
- **信号完整性**（`syscall/table.c`）：`rt_sigaction` 保存 handler 表；`kill` 支持 SIGKILL 之外的基本信号；信号投递路径（用户态返回前检查 pending）；`SIGSEGV`/`SIGBUS` 由用户态异常路径产生。

### 阶段 3：生产增强
- **终端控制**：完整 termios 状态机（ECHO/ICANON/ISIG、VMIN/VTIME）、`tcsetpgrp`/`tcgetpgrp`、前台进程组与 job control（`&`、`fg`、`bg` 基础）。
- **进程间通信**：SysV/POSIX 最小 `shm`（`shmget/shmctl/shmget` 或 `mmap` 共享）+ `sem` + `msg`（选一实现）。
- **基础网络**：loopback —— `socket(AF_INET, SOCK_STREAM)`、`bind`、`listen`、`accept`、`connect`、`send/recv` 的最小实现（回环地址 127.0.0.1，无网卡驱动）。
- **磁盘写入优化**：块缓冲/延迟写（可选，阶段 3 末尾）。

## Impact

- Affected specs：`w6-real-fs-elf-userspace`（继承基础）、`w5-linux-abi-compat`（信号完整性）、`w3-scheduler-ipc`（进程/异常）、`w4-sigma-power-manager`（0xCF9 重启与功耗）。
- Affected code：
  - 引导/构建：`src/boot/bootsect.S`、`build.ps1`、`src/boot/boot.S`（若高半区需要）、`linker.ld`
  - 内存：`src/kernel/mm/vm.c`、`src/kernel/mm/mm.c`
  - 异常/调试：`src/kernel/core/idt.c`、`src/kernel/core/idt.S`、`src/kernel/core/panic.c`
  - 文件系统：`src/kernel/fs/ext2.c/h`、`src/kernel/fs/vfs.c/h`、`src/kernel/fs/tmpfs.c`、新增 `fs/procfs.c`、`fs/sysfs.c`
  - 系统调用：`src/kernel/syscall/table.c`、`src/kernel/syscall/syscall_entry.S`
  - Shell：`src/kernel/core/shell.c`（ps/dmesg 命令）
  - 新增模块：`core/printk.c`、`ipc/shm.c`、`net/loopback.c`
- 验收口径：`kernel.flat < 512KB`（原 <80KB）、启动 <2s、40 个 POSIX 系统调用兼容目标。

## ADDED Requirements

### Requirement: 内核/用户内存隔离
系统 SHALL 保证用户态（CPL3）进程无法读写任何未显式映射为 U/S=1 的内存页，包括内核代码/数据/栈（0-2GB identity 区）。

#### Scenario: 用户态访问内核内存被隔离
- **WHEN** 用户程序尝试写 `0x100000`（内核代码）或 `0xFFFF800000000000`（未映射）
- **THEN** 触发用户态 `#PF`，进程被终止，内核与 Shell 继续运行（非整机 halt）

#### Scenario: 用户程序正常内存不受影响
- **WHEN** 用户程序读写自己的 ELF 段 / 栈 / brk / mmap 页
- **THEN** 正常访问，无异常

### Requirement: 用户态异常终止进程
系统 SHALL 将用户态异常（#PF/#GP/#UD）视为进程级错误而非内核级错误。

#### Scenario: 子进程崩溃不影响父进程
- **WHEN** 子进程发生用户态 `#GP`（如执行 `hlt`）或 `#PF`（空指针）
- **THEN** 子进程被终止并标记为异常退出；父进程（bash）的 `wait4` 返回其状态，Shell 继续

#### Scenario: 内核态异常触发 panic
- **WHEN** 内核态（CPL0）发生异常
- **THEN** 打印现场并走 panic 恢复流程

### Requirement: Panic 恢复与诊断
系统 SHALL 在 panic 时记录诊断信息并自动重启。

#### Scenario: 主动触发 panic
- **WHEN** 执行 `panic` 命令
- **THEN** 打印栈回溯与进程信息，5 秒后经 0xCF9 自动重启；重启后显示 "Previous panic: ..."；连续 3 次 panic 进入仅 Shell 的安全模式

### Requirement: EXT2 文件写入
系统 SHALL 支持在 EXT2 只读盘上创建、写入、删除文件与目录，并持久化到磁盘。

#### Scenario: 创建并写入文件
- **WHEN** bash 执行 `echo "hello" > /tmp/test.txt`
- **THEN** 创建 `/tmp/test.txt`，内容为 `hello\n`；`cat /tmp/test.txt` 显示 `hello`；重启后文件依然存在

#### Scenario: 删除文件
- **WHEN** bash 执行 `rm /tmp/test.txt`
- **THEN** 文件被删除，`cat /tmp/test.txt` 报不存在

### Requirement: 系统日志
系统 SHALL 提供带级别的内核日志与查看命令。

#### Scenario: dmesg 查看日志
- **WHEN** 执行 `dmesg`
- **THEN** 显示最近 64 条内核消息（含级别）；panic 后 dmesg 内存内容保留（写入磁盘则不丢）

### Requirement: /proc 与进程可观测性
系统 SHALL 提供进程信息读取接口与 `ps` 命令。

#### Scenario: ps 查看进程
- **WHEN** 执行 `ps`
- **THEN** 列出当前进程（PID、名称、状态）；`cat /proc/self/status` 返回当前进程状态

### Requirement: 信号投递
系统 SHALL 支持注册信号处理器并投递基本信号。

#### Scenario: kill 发送信号
- **WHEN** `kill <pid>` 发送默认动作信号（如 SIGTERM）
- **THEN** 目标进程执行默认动作或注册的 handler；SIGKILL 仍强制终止

### Requirement: 基础网络（loopback）
系统 SHALL 提供最小 TCP 回环能力。

#### Scenario: 回环 socket 连接
- **WHEN** 进程 A `socket+bind+listen+accept`（127.0.0.1:port），进程 B `connect+send/recv`
- **THEN** 建立连接并双向传输数据

### Requirement: 引导扇区 LBA 加载
系统 SHALL 通过 LBA 方式从磁盘加载最多 512KB 的内核镜像。

#### Scenario: 大内核引导
- **WHEN** `kernel.flat` 大于 80KB 但小于 512KB
- **THEN** bootsect 通过 LBA 读取完整内核并正确跳转，无磁盘错误

## MODIFIED Requirements

### Requirement: 内核体积约束（原：<80KB）
放宽为 **<512KB**（引导扇区 `KERNEL_SECTORS` 160→1024，CHS→LBA）。仍鼓励紧凑实现。
**原因**：全部 3 阶段新增约 1500+ 行无法在 3.4KB 余量内落地。
**迁移**：`build.ps1` 的 `minKernelSectors` 与 `bootsect.S` 拷贝计数同步更新；`kernel.flat` 验收阈值改为 512KB。

### Requirement: 动态链接器路径 Fallback（强化）
`elf/interp.c` 候选路径扩展为：PT_INTERP 硬编码 → `/lib/ld-linux-x86-64.so.2` → `/lib64/ld-linux-x86-64.so.2` → `/usr/lib/ld-linux-x86-64.so.2`；全部失败时打印 "No dynamic linker found" 并返回明确错误。

## REMOVED Requirements

### Requirement: 用户态#GP 触发整机 Triple Fault（旧行为，已在本会话前修复）
**原因**：内核已安装最小 IDT，任何异常不再静默重启。
**迁移**：异常现在打印 vector/err/RIP/CR2 并 halt；本 Spec 将其升级为"用户态杀进程、内核态 panic 恢复"。
