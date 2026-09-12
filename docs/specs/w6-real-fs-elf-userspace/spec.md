# W6 — 真实文件系统 + ELF 加载 + 用户态切换 Spec

版本：W6 · Baseline
状态：待实施
依赖：W5 完成（13 个系统调用、ELF 静态加载骨架、tmpfs 原型）

## Why
W5 已建立 Linux ABI 兼容层雏形，但存在关键缺口，导致无法运行真实的 Linux 发行版程序：
- 存储：tmpfs 文件内容硬编码进内核，无法读取磁盘上的任何文件
- 文件系统：无块设备驱动、无 EXT2 解析，`/bin/bash` 只能伪造
- 地址空间：所有任务共享内核页表（identity map），无进程隔离
- 用户态切换：`execve` 返回 `-ENOSYS` 或伪造成功，无法真正执行用户程序
- 动态链接：`PT_INTERP` 只打印警告，无法加载动态链接器

W6 目标是彻底关闭这些缺口，使内核具备运行真实动态链接 Linux 程序（如 `/bin/bash`）的能力。

## What Changes
- 新增块设备层 `dev/ata.c/h`：ATA PIO 读写扇区（QEMU / 真实 IDE 兼容）
- 新增 EXT2 只读驱动 `fs/ext2.c/h`：解析超级块、组描述符、inode、目录、符号链接
- 修改 VFS `fs/vfs.c/h`：真实的 `vfs_open`、路径解析、挂载点管理
- 新增文件对象扩展 `fs/file.c/h`：文件描述符表，`dup`/`close` 真实支持
- 新增用户态页表管理 `mm/vm.c/h`：每任务独立页表，`vm_map` 建立 U/S=1 映射
- 新增用户态切换 `sched/switch.S`：`switch_to_user` 通过 iretq 进入 Ring 3
- 重构 ELF 加载器 `elf/loader.c/h`：使用 `vm_map` 映射 `PT_LOAD` 段，识别 `PT_INTERP`
- 修改动态链接器加载 `elf/interp.c/h`：加载 `/lib/ld-linux-x86-64.so.2`
- 修改系统调用 `syscall/table.c/h`：完整 `execve` 路径 + 补充 `fork`/`wait4`/`kill`
- 修改集成初始化 `kmain.c`、`build.ps1`：挂载 `/dev/sda` 到 `/`，编译新文件

## Impact
- Affected specs: w5-linux-abi-compat（VFS、ELF 加载器、syscall、mm、sched）
- Affected code:
  - 新增：`src/kernel/dev/ata.c` `ata.h`、`src/kernel/fs/ext2.c` `ext2.h`、`src/kernel/fs/file.c` `file.h`、`src/kernel/mm/vm.c` `vm.h`、`src/kernel/sched/switch.S`
  - 修改：`src/kernel/fs/vfs.c`、`src/kernel/elf/loader.c`、`src/kernel/elf/interp.c`、`src/kernel/syscall/table.c`、`src/kernel/mm/mm.c`、`src/kernel/sched/sched.c`、`src/kernel/kmain.c`、`build.ps1`

---

## ADDED Requirements

### Requirement: 块设备层（ATA PIO 驱动）
系统 SHALL 提供 ATA PIO 驱动的扇区读写接口，支持 28-bit LBA 寻址。

接口：
- `void ata_init(void)`
- `int ata_read_sector(uint32_t lba, uint8_t* buffer)`
- `int ata_write_sector(uint32_t lba, const uint8_t* buffer)`
- `int ata_read_sectors(uint32_t lba, uint32_t count, uint8_t* buffer)`

#### Scenario: 读取 EXT2 超级块
- **WHEN** 调用 `ata_read_sector(2, buf)`
- **THEN** 返回 0，且 `buf[0:2] == 0xEF53`（EXT2 魔数）

#### Scenario: 设备未就绪超时
- **WHEN** ATA 状态寄存器在超时时间内未就绪
- **THEN** 返回负值错误码，内核不挂死

### Requirement: EXT2 只读驱动
系统 SHALL 解析 EXT2 文件系统（4096 字节块大小），支持超级块、组描述符、inode、目录和符号链接读取（只读）。

#### Scenario: 挂载文件系统
- **WHEN** 调用 `ext2_mount()`
- **THEN** 打印块大小、inode 数、总块数，无错误，根 inode（#2）设为 VFS 根

#### Scenario: 列出根目录
- **WHEN** 用户执行 `ls /`
- **THEN** 列出磁盘镜像中的 `/bin` `/lib` `/etc` 等目录项

#### Scenario: 读取文件内容
- **WHEN** 用户执行 `cat /etc/hostname`
- **THEN** 显示该文件内容（若文件存在）

### Requirement: VFS 增强与路径解析
系统 SHALL 提供真实的 `vfs_open`，支持以 `/` 开头的绝对路径逐级解析，并支持挂载点管理。

#### Scenario: 打开磁盘文件
- **WHEN** 调用 `vfs_open("/bin/bash", O_RDONLY)`
- **THEN** 返回指向磁盘 inode 的 `file` 结构，而非硬编码 tmpfs 内容

#### Scenario: 文件不存在
- **WHEN** 调用 `vfs_open("/nonexistent", ...)`
- **THEN** 返回 NULL，上层显示 "File not found"

### Requirement: 独立用户态页表
系统 SHALL 为每个任务维护独立的页表（`cr3`），内核映射保持不变，用户空间低地址映射设置 U/S=1。

#### Scenario: 建立用户映射
- **WHEN** 调用 `vm_map(ctx, vaddr, size, flags, 0)`
- **THEN** 分配物理页并填充页表条目，设置用户可访问权限位

#### Scenario: 切换地址空间
- **WHEN** 调用 `vm_switch(ctx)`
- **THEN** `cr3` 切换到该任务的页表，用户态不可访问内核空间

### Requirement: 用户态切换（Ring 3）
系统 SHALL 通过 iretq 切换到 Ring 3，并支持 `syscall` 返回内核。

#### Scenario: 进入用户态
- **WHEN** 调用 `switch_to_user(entry, rsp)`
- **THEN** 通过 iretq 跳转到用户态入口，CS/SS 选择子指向 Ring 3 段

#### Scenario: 系统调用返回
- **WHEN** 用户态执行 `syscall` 指令
- **THEN** 内核处理完毕后通过 `sysretq` 返回用户态继续执行

### Requirement: 动态链接器加载
系统 SHALL 识别 `PT_INTERP` 并加载解释器 `/lib/ld-linux-x86-64.so.2`（fallback `/lib64/ld-linux-x86-64.so.2`）。

#### Scenario: 解析动态链接程序
- **WHEN** ELF 含 `PT_INTERP` 段
- **THEN** 记录解释器路径，实际入口为解释器入口，程序头信息通过用户栈传递

### Requirement: 完整 execve 路径
系统 SHALL 实现「打开文件 → 加载 ELF → 准备用户栈 → 切换用户态」的完整 `execve` 系统调用。

#### Scenario: 执行程序
- **WHEN** 调用 `execve("/bin/bash", argv, envp)`
- **THEN** 加载 ELF（含解释器），压入 argc/argv/envp，切换到用户态执行，正常不返回

### Requirement: 补充系统调用（支持 bash）
系统 SHALL 提供运行 bash 所需的基础进程管理调用。

| 编号 | 名称 | 功能 |
|------|------|------|
| 57 | fork | 复制当前任务及页表（暂不实现 COW） |
| 61 | wait4 | 阻塞等待子进程退出 |
| 62 | kill | 仅支持 SIGKILL |
| 13 | rt_sigaction | 空实现，返回成功 |
| 14 | rt_sigprocmask | 空实现，返回成功 |
| 15 | rt_sigreturn | 空实现 |
| 72 | fcntl | 支持 F_DUPFD / F_GETFD |
| 79 | getcwd | 返回 "/" |
| 80 | chdir | 空实现（总是成功） |

#### Scenario: fork 与 wait
- **WHEN** bash 执行外部命令调用 `fork` + `wait4`
- **THEN** 创建子进程，父进程阻塞等待子进程退出后回收

## MODIFIED Requirements

### Requirement: ELF 加载器重构
W5 的 `elf_load` 改为使用 `vm_map` 映射 `PT_LOAD` 段到用户虚拟地址，返回入口、栈顶和解释器路径，替代当前「段直接写到虚拟=物理地址」的做法。

#### Scenario: 加载动态链接 ELF
- **WHEN** 调用 `elf_load("/bin/bash", ctx, &entry, &stack, &interp)`
- **THEN** `PT_LOAD` 段通过 `vm_map` 映射，识别 `PT_INTERP`，返回解释器入口

### Requirement: VFS 接口修订
W5 的 `file_ops`/`file` 结构扩展 `refcount` 字段，`vfs_open` 从空实现改为真实路径解析与挂载分发。

### Requirement: 任务结构扩展
`sched.c` 的 `task_t` 新增 `mm_context` 字段，用户任务携带独立页表上下文。

### Requirement: 内存映射接口扩展
`mm.c` 的 `mmap_user` 改为委托 `vm_map`，使用任务自身的 `mm_context`。

## REMOVED Requirements
无（W6 不删除 W5 需求，仅在 tmpfs 硬编码 ELF、identity-map 直写基础上演进）

## Risk Mitigation
| 风险 | 影响 | 缓解措施 |
|------|------|------|
| ATA 读取在真实硬件失败 | 无法启动 | W6 仅在 QEMU 验证，真实硬件驱动留待后续 |
| EXT2 多种块大小/特性 | 解析失败 | 先支持 4096 字节块，忽略稀疏超级块、扩展属性 |
| 页表切换导致 #PF | 内核崩溃 | 单独验证 vm_switch，逐页打印页表条目 |
| 用户栈布局错误 | 程序段错误 | 严格遵循 Linux x86-64 ABI，memcpy argv 到栈底 |
| 动态链接器路径错误 | bash 无法启动 | 支持 fallback 路径并打印日志 |
| Windows 主机缺少 mkfs.ext2 | 无法生成测试盘 | 使用预生成 ext2 镜像或 WSL 生成，提交到仓库 |